// ─────────────────────────────────────────────────────────────────────────────
//  The fixed engine runtime, embedded so that `sec` is self-contained.
//
//  It is NEVER generated: it does not vary with the model, the same text
//  serves a 3-state model and a 30,000-state one. By default it is pasted
//  into the generated header, so the emitted model is one file that compiles
//  on its own; `--runtime=separate` writes it beside the model as
//  `se_runtime.hpp` instead (§15.5).
//
//  Editing it here changes every future emission. It is ordinary C++, parked
//  in raw string literals only because a compiler that has to find its own
//  data files at run time is a compiler that breaks when it is moved.
//
//  The text carries a version tag, `@VER@`, that se_runtime_hpp() replaces
//  with a hash of the text itself. The tag names both the include guard and
//  an inline namespace, so two models in one program share one copy when
//  their runtimes are identical and cannot collide when they are not.
// ─────────────────────────────────────────────────────────────────────────────
#include "emit.hpp"
#include "units.hpp"

#include <cstdint>
#include <cstdio>

namespace se {

namespace {

const char kRuntime[] =
    R"SERT(// ─────────────────────────────────────────────────────────────────────────────
//  THE ENGINE RUNTIME — fixed, hand-written, NEVER generated
//
//  Nothing here knows anything about any particular model. It holds what the
//  generated scheduler needs that is not model-shaped:
//
//      state views    section 15.5 — `state.omega` as a view into Sim::x
//      log.*          section 10.1 — qualitative, observational
//      sim.stop/abort section 10.2 — the only way a run ends early
//      the recorder   section 13.5 — quantitative, an aligned numeric table
//      pacing         section 9.4  — wait for the wall, when the host asks
//      settings       section 14.1 — units, and reading and dumping a source
//
//  None of it is a process global. Everything a run accumulates (the clock,
//  the stop flag, the contract registry, the pacing, the log) lives in one
//  `Run` per model, so a program can run a model again, or run several, and
//  no two runs see each other.
//
//  The generator emits `se_rt::Log log{...}` into the preamble of the methods
//  where logging is legal and simply does not emit it elsewhere, which is how
//  confinement is enforced: by name withholding, not by an analyser
//  (section 15.4).
//
//  The guard and the inline namespace are named for a hash of this text, so a
//  second model built from the same runtime skips it, and one built from a
//  different runtime gets its own copy under a different name.
//
//  C++20, no third-party packages.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_RUNTIME_@VER@
#define SE_RUNTIME_@VER@

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

@STRING_TYPE@
namespace se_rt {
inline namespace @VER@ {

// ─── `{}` positional formatting (section 10.1) ───────────────────────────────
// The engine auto-tags every record with sim time, node path and level, so
// those are never arguments.

inline void format_into(std::ostringstream& o, const char* f) {
    while (*f) {
        if (f[0] == '{' && f[1] == '}') {
            o << "{}";
            f += 2;
            continue;
        }
        o << *f++;
    }
}

template <class A, class... R>
void format_into(std::ostringstream& o, const char* f, const A& a, const R&... rest) {
    while (*f) {
        if (f[0] == '{' && f[1] == '}') {
            o << a;
            format_into(o, f + 2, rest...);
            return;
        }
        o << *f++;
    }
}

// Every call below is written `se_rt::format(...)`, qualified. Unqualified, an
// argument of a std type drags `std::format` in by ADL -- C++20 makes <format>
// reachable from headers this one already includes -- and the two overloads are
// then ambiguous. Qualifying the name turns ADL off at the call site.
template <class... A>
std::string format(const char* f, const A&... a) {
    std::ostringstream o;
    format_into(o, f, a...);
    return o.str();
}

// ─── Output files (section 13.5) ─────────────────────────────────────────────
// `{time}` is the one placeholder a file name may hold. A name with it is a
// new file every run; a name without it is opened as it always was.

// Why `name` is not a well-formed file name, or "" when it is.
inline std::string file_pattern_error(const std::string& name) {
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '}')
            return "a `}` with no `{` before it; `{time}` is the only placeholder (section 13.5)";
        if (name[i] != '{') continue;
        const std::size_t close = name.find('}', i);
        if (close == std::string::npos)
            return "a `{` with no `}` after it; `{time}` is the only placeholder (section 13.5)";
        const std::string word = name.substr(i + 1, close - i - 1);
        if (word != "time")
            return "`{" + word + "}` is not a placeholder; `{time}` is the only one (section 13.5)";
        i = close;
    }
    return "";
}

// An inputs file is read, not written (section 13.8), so `{time}`, which
// makes a written name unique, has nothing to do in its name.
inline std::string inputs_file_error(const std::string& name) {
    if (name.find("{time}") != std::string::npos)
        return "`{time}` names a file the run writes, and an inputs file is read (section 13.8)";
    if (name.find_first_of("{}") != std::string::npos)
        return "a brace in the name: an inputs file is read, so its name is the file's own, "
               "with no placeholder (section 13.8)";
    return "";
}

// `name` as a source in directory `base` means it (section 13.8): joined,
// unless it is absolute or there is no directory to join.
inline std::string beside(const std::string& base, const std::string& name) {
    const bool absolute = !name.empty() && (name[0] == '/' || name[0] == '\\' ||
                                            (name.size() > 1 && name[1] == ':'));
    return base.empty() || name.empty() || absolute ? name : base + name;
}

// Now, as `{time}` writes it: local time, YYYYMMDD-HHMMSS.
inline std::string time_stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &tm);
    return text;
}

// Opens `pattern` for writing; `used` is the name it opened. Without `{time}`
// it replaces a file already there. With it, `stamp` goes in (now, if empty),
// and the file is created exclusively: a name already taken gets `-2`, `-3`,
// ... before its extension, so two runs started in the same second write two
// files.
inline std::FILE* open_output(const std::string& pattern, std::string stamp, std::string& used) {
    used = pattern;
    if (pattern.find("{time}") == std::string::npos) return std::fopen(pattern.c_str(), "w");
    if (stamp.empty()) stamp = time_stamp();
    std::string name = pattern;
    for (std::size_t at = name.find("{time}"); at != std::string::npos;
         at = name.find("{time}", at + stamp.size()))
        name.replace(at, 6, stamp);
    const std::size_t slash = name.find_last_of("/\\");
    const std::size_t start = slash == std::string::npos ? 0 : slash + 1;
    std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot <= start) dot = name.size();
    for (int n = 1; n < 10000; ++n) {
        used = n == 1 ? name : name.substr(0, dot) + "-" + std::to_string(n) + name.substr(dot);
        if (std::FILE* f = std::fopen(used.c_str(), "wx")) return f;
        if (errno != EEXIST) return nullptr;
    }
    return nullptr;
}

// ─── Logging (section 13.7) ──────────────────────────────────────────────────

enum class Level { Trace = 0, Debug, Info, Warn, Error };

inline const char* level_name(Level l) {
    switch (l) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info:  return "info";
        case Level::Warn:  return "warn";
        case Level::Error: return "error";
    }
    return "?";
}

// One line the console would have received, kept for a host that has no
// console (section 14.1).
struct LogLine {
    Level       level = Level::Info;
    std::string path;
    double      time = 0.0;
    std::string text;
};

class LogSink {
public:
    ~LogSink() {
        if (file_) std::fclose(file_);
    }

    // Re-runnable: a second configure() closes the first file rather than
    // leaking it, and starts the path levels over. `path` may hold `{time}`,
    // which `stamp` fills (section 13.5).
    void configure(const std::string& path, Level global, const std::string& stamp) {
        if (file_) std::fclose(file_);
        file_ = nullptr;
        used_.clear();
        paths_.clear();
        global_ = global;
        if (!path.empty()) file_ = open_output(path, stamp, used_);
    }

    // The file's name as opened, `{time}` filled in; empty before configure().
    const std::string& used() const { return used_; }

    // Section 14.1 -- while capturing, the console's lines are kept here
    // instead, up to a cap, for the host to take one at a time.
    void capture(bool on) { capture_ = on; }
    bool capturing() const { return capture_; }

    void keep(Level l, const char* path, double t, const std::string& msg) {
        if (kept_.size() >= kCap) {
            dropped_time_ = kept_.front().time;
            kept_.pop_front();
            ++dropped_;
        }
        kept_.push_back(LogLine{l, path, t, msg});
    }

    // The oldest kept line; first, if the cap dropped any, one saying so.
    bool next(LogLine& out) {
        if (dropped_) {
            out = LogLine{Level::Warn, "sim", dropped_time_,
                          se_rt::format("{} log lines dropped: the queue holds {}", dropped_, kCap)};
            dropped_ = 0;
            return true;
        }
        if (kept_.empty()) return false;
        out = std::move(kept_.front());
        kept_.pop_front();
        return true;
    }

    // Per-node-path thresholds; the longest matching prefix wins.
    void set_path_level(const char* path, Level l) { paths_.emplace_back(path, l); }

    Level level_for(const char* path) const {
        Level best = global_;
        std::size_t best_len = 0;
        for (const auto& p : paths_) {
            const std::size_t n = p.first.size();
            if (std::strncmp(path, p.first.c_str(), n) != 0) continue;
            if (path[n] != '\0' && path[n] != '.') continue;
            if (n >= best_len) {
                best = p.second;
                best_len = n;
            }
        }
        return best;
    }

    void write(Level l, const char* path, double t, const std::string& msg) {
        const bool passes = l >= level_for(path);
        // The console always receives warn and above regardless of the
        // configured threshold (section 13.7).
        const bool to_console = (l >= Level::Warn) || (!file_ && passes);
        const bool to_file = file_ && passes;
        if (!to_console && !to_file) return;

        char head[96];
        std::snprintf(head, sizeof head, "[%12.6f] %-5s %s: ", t, level_name(l), path);
        if (to_file) {
            std::fputs(head, file_);
            std::fputs(msg.c_str(), file_);
            std::fputc('\n', file_);
        }
        if (to_console && capture_) {
            keep(l, path, t, msg);
        } else if (to_console) {
            std::fputs(head, stderr);
            std::fputs(msg.c_str(), stderr);
            std::fputc('\n', stderr);
        }
    }

    // section 10.1 — trace in the pure methods is suppressed unless this is set,
    // so the four-evaluations-per-step hazard is visible rather than hidden.
    bool trace_enabled = false;

private:
    static constexpr std::size_t kCap = 10000;

    std::FILE* file_ = nullptr;
    std::string used_;
    Level global_ = Level::Info;
    std::vector<std::pair<std::string, Level>> paths_;
    bool capture_ = false;
    std::deque<LogLine> kept_;
    std::uint64_t dropped_ = 0;
    double dropped_time_ = 0.0;
};

// The `log` objects a body writes through are below, after `Run`: each one
// reaches its model's sink, and a sink belongs to a run, not to the process.

)SERT"
    // MSVC caps a single string literal at 16380 bytes, and this text passed
    // it long ago. Adjacent literals concatenate, so the splits are invisible
    // in the output; each must fall on a blank line between sections, and each
    // PIECE must stay under the cap as the runtime grows.
    R"SERT(// ─── State views (section 15.5) ─────────────────────────────────────────────
//
// A named view of one slot in a model-wide block, so a node body can write
// `state.omega` instead of `sim.x[7]`, while an external solver (CVODE,
// odeint, a hand-rolled RK4) still sees nothing but `double*`.
//
// THE UNIT CONTRACT. Units are checked by the DSL compiler at every
// declarative site and then ERASED. They are never C++ types. What survives
// into C++ is a guarantee: the slot holds the value IN THE STATE'S DECLARED
// UNIT. `omega (rad/s)` => x[i] is rad/s, `state.omega` reads rad/s, and a
// write to `state.omega` had better be rad/s — the compiler will not check
// that for you. This is a contract with the node implementer and with
// whoever writes the external solver, and it is why the generator emits a
// unit manifest (Sim.units.txt) alongside the model code.

// Not a `T&`. A reference member can be neither default-constructed nor
// rebound, but the generated State and Der structs must be default-constructible
// before Sim::bind() runs. Hence a pointer proxy.
//
// It IS a template, because a continuous state is always `double` (the solver's
// ABI is a flat double array) but a discrete state may be `int` or `bool`, and
// both now live in a model-wide block that the node only views. Templating does
// not disturb the argument above: every instantiation is still a default-
// constructible, rebindable pointer proxy.
//
// Constness propagates because the containing State struct is held BY VALUE in
// the node. In a const method `state` is a `const State`, so `state.omega` is a
// `const value_ref`, and `operator=` is not callable. A `T&` or `T*` member
// would leak, which is exactly why this class exists.
template <class T>
class value_ref {
    T* slot_ = nullptr;

public:
    value_ref() = default;
    explicit value_ref(T& slot) : slot_(&slot) {}

    // Bound once, in Sim::bind().
    void bind(T& slot) { slot_ = &slot; }

    // Read: behaves as a T, so every operator, every <cmath> function and every
    // third-party API accepts it with no interop work whatsoever.
    operator T() const { return *slot_; }

    // Write.
    value_ref& operator=(T v) {
        *slot_ = v;
        return *this;
    }

    // Copy-ASSIGNMENT moves the value, not the binding — `der.w = state.wd;`
    // writes the derivative slot, which is the only thing that line can mean.
    //
    // This overload is not optional and must never be deleted back to the
    // implicit one. Declaring `operator=(T)` does NOT suppress the compiler's
    // copy-assignment, and between the two the implicit one wins outright on a
    // proxy-to-proxy assignment: it is an exact match, while `operator=(T)`
    // needs the user-defined `operator T()`. The implicit one copies `slot_`,
    // so the write silently REBINDS the target at the source's slot instead of
    // storing anything — the derivative stays zero and the state never moves.
    // It compiles clean and it is wrong at run time, which is the worst
    // combination available. Copy-CONSTRUCTION still shares the slot: a copy of
    // a view is a view, and only assignment through one is a write.
    value_ref(const value_ref&) = default;
    value_ref& operator=(const value_ref& o) {
        *slot_ = *o.slot_;
        return *this;
    }
};

// The same argument one step further, for an array-shaped state (section 6.4a).
//
// `State`, `Der` and `Store` must still be default-constructible before
// Sim::bind_state() runs, and now the LENGTH is not known at compile time
// either, so a std::array member is not available even in principle: the extent
// is a configuration result (section 6.2b step 4). A pointer plus a length,
// rebound once, is what is left.
//
// Constness propagates through the subscript, which is the property that makes
// this a view of the same kind as value_ref rather than a parallel mechanism:
// in a const method `state` is a const State, `state.w1` is a const array_ref,
// and `state.w1[i]` is a `const T&`. So `state.w1[i] = ...` fails to compile in
// output() for exactly the reason `state.omega = ...` does.
template <class T>
class array_ref {
    T* p_ = nullptr;
    std::size_t n_ = 0;

public:
    array_ref() = default;

    // Bound once, from Sim::bind_state() / Sim::bind_discrete_array().
    void bind(T* p, std::size_t n) {
        p_ = p;
        n_ = n;
    }

    std::size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }

    T& operator[](std::size_t i) { return p_[i]; }
    const T& operator[](std::size_t i) const { return p_[i]; }

    T* data() { return p_; }
    const T* data() const { return p_; }
    T* begin() { return p_; }
    T* end() { return p_ + n_; }
    const T* begin() const { return p_; }
    const T* end() const { return p_ + n_; }

    void fill(T v) {
        for (std::size_t i = 0; i < n_; ++i) p_[i] = v;
    }

    // Copy-ASSIGNMENT moves the elements, not the binding — the same rule as
    // value_ref, and load-bearing for the same reason. `dis = nxt` at the
    // bottom of the tick is a whole-block struct copy, and this is the member
    // that makes the array-shaped slice of it commit rather than rebind. The
    // implicit copy-assignment would repoint dis at nxt's storage and the state
    // would never move; it compiles clean and is wrong at run time.
    array_ref(const array_ref&) = default;
    array_ref& operator=(const array_ref& o) {
        const std::size_t n = n_ < o.n_ ? n_ : o.n_;
        for (std::size_t i = 0; i < n; ++i) p_[i] = o.p_[i];
        return *this;
    }
};

using state_ref = value_ref<double>;
using state_arr = array_ref<double>;

)SERT"
    R"SERT(// ─── Buffers (section 5.1a) ───────────────────────────────────────────────
//
// Opaque bytes plus a size. A leaf's buffer OUTPUT and a buffer VAR are
// storage, `buffer<N>`; every buffer INPUT is a `buffer_view` onto some
// producer's storage, which is why a reader accepts any capacity. The three
// read members are spelled the same on both, so a body reads `in.packet` and
// `var.rx` alike.
//
// The size comes first so that a host reading the signal block through
// signal_map() finds the length at the slot's own offset without knowing N.
// An aggregate, so `buffer<N> data{};` zeroes it like every other member.
struct buffer_view;

template <std::size_t N>
struct buffer {
    static_assert(N > 0, "a buffer capacity is a positive byte count");
    std::int32_t size_;
    std::uint8_t data_[N];

    const std::uint8_t* data() const { return data_; }
    std::uint8_t* data() { return data_; }
    std::size_t size() const { return static_cast<std::size_t>(size_); }
    static constexpr std::size_t capacity() { return N; }
    bool empty() const { return size_ == 0; }

    // Clamped to the capacity: the bytes past the old size are whatever the
    // caller writes into data() next.
    void resize(std::size_t n) { size_ = static_cast<std::int32_t>(n < N ? n : N); }
    void clear() { size_ = 0; }

    // Copies min(n, N) bytes, and says whether that was all of them. Never
    // truncates silently.
    bool assign(const void* p, std::size_t n) {
        const std::size_t k = n < N ? n : N;
        if (k) std::memcpy(data_, p, k);
        size_ = static_cast<std::int32_t>(k);
        return k == n;
    }
    inline bool assign(const buffer_view& v);

    // Copying one buffer to another copies the bytes that are THERE, not the
    // capacity: an output pass copies a staged datagram into `out` every
    // tick, and a 64-byte payload in a 64 KiB buffer should cost 64 bytes.
    buffer() = default;
    buffer(const buffer& o) { *this = o; }
    buffer& operator=(const buffer& o) {
        size_ = o.size_;
        if (size_ > 0) std::memcpy(data_, o.data_, static_cast<std::size_t>(size_));
        return *this;
    }
};

// What a reader holds: a pointer into the producer's storage, the size, the
// capacity. Never owns, never copies the bytes (section 15.3).
struct buffer_view {
    buffer_view() = default;
    template <std::size_t N>
    buffer_view(const buffer<N>& b) : p_(b.data_), n_(b.size()), cap_(N) {}   // implicit, by design

    const std::uint8_t* data() const { return p_; }
    std::size_t size() const { return n_; }
    std::size_t capacity() const { return cap_; }
    bool empty() const { return n_ == 0; }

private:
    const std::uint8_t* p_ = nullptr;
    std::size_t n_ = 0;
    std::size_t cap_ = 0;
};

template <std::size_t N>
inline bool buffer<N>::assign(const buffer_view& v) {
    return assign(v.data(), v.size());
}

)SERT"
    R"SERT(// ─── Block descriptors (section 15.6) ────────────────────────────────────────
//
// A generated model exposes its signal and discrete-state blocks as one
// contiguous object each, plus a table of these. The offsets come from
// offsetof() in the generated header, not from the DSL compiler: padding is the
// C++ compiler's business, so the table is built where that is known.
//
// With the block's address and this table, an inspector, a shared-memory host,
// a replay recorder or a co-simulation peer needs no generated accessor code.
struct Slot {
    const char* path;     // model path, as section 13.2 addresses it
    std::size_t offset;   // bytes from the start of the block
    std::size_t size;     // bytes
    const char* type;     // "double", "int", "bool", "float", an enum's C++
                          // name, or "se_rt::buffer<N>" (size first, section 5.1a)
    const char* unit;     // the declared unit; "-" is dimensionless
};

// ─── Array extents (section 6.4a, section 15.5a) ─────────────────────────────
//
// An extent is a section-7 expression over ordinary overridable settings, so it
// is evaluated at configuration and only then does it have to be an exact
// non-negative integer. The
// DSL compiler applied the rule to what it could see, and this applies it to
// what actually arrived. Zero is legal and yields no slots.
inline bool extent_of(double v, std::size_t& out) {
    out = 0;
    if (!(v >= 0.0)) return false;
    const double n = std::floor(v + 0.5);
    // A tolerance rather than equality: the value reaches here through decimal literals and possibly a
    // unit conversion, so an intended integer need not be exact.
    if (std::fabs(v - n) > 1e-9 * (n > 1.0 ? n : 1.0)) return false;
    out = static_cast<std::size_t>(n);
    return true;
}

inline std::string extent_error(const char* path, double v) {
    std::ostringstream o;
    o << "the extent of " << path << " is " << v
      << ", which is not an exact non-negative integer";
    return o.str();
}

// section 13.4 / section 13.5 — an index past the extent CONFIGURATION computed.
// Not a compile error: the same settings source may be valid against one order
// and invalid against another.
inline std::string index_error(const char* path, std::size_t n) {
    std::ostringstream o;
    o << path << " is past the configured extent (" << n
      << (n == 1 ? " element)" : " elements)");
    return o.str();
}

// section 15.5a — the one error the host handshake makes possible, and the
// cheapest possible check.
inline std::string bind_error(const char* what, std::size_t given, std::size_t want) {
    std::ostringstream o;
    o << what << ": " << given << " slots given, " << want << " expected";
    return o.str();
}

// ─── Termination (section 10.2) ──────────────────────────────────────────────

enum class Reason { Running, Completed, Stopped, Aborted, Error };

inline const char* reason_name(Reason r) {
    switch (r) {
        case Reason::Running:   return "running";
        case Reason::Completed: return "completed";
        case Reason::Stopped:   return "stopped";
        case Reason::Aborted:   return "aborted";
        case Reason::Error:     return "error";
    }
    return "?";
}

// Read-only at teardown; a parameter rather than a global, because the outcome
// is only knowable then (section 10.3).
struct RunContext {
    const char* reason = "completed";
    std::string message;
    double time = 0.0;
};

class Control {
public:
    void stop(const std::string& m) { set(Reason::Stopped, m); }
    void abort(const std::string& m) { set(Reason::Aborted, m); }
    void fail(const std::string& m) { set(Reason::Error, m); }
    void complete() { set(Reason::Completed, std::string()); }

    bool halted() const { return reason_ != Reason::Running; }
    Reason reason() const { return reason_; }
    const std::string& message() const { return message_; }

    RunContext context(double t) const {
        RunContext c;
        c.reason = reason_name(reason_ == Reason::Running ? Reason::Completed : reason_);
        c.message = message_;
        c.time = t;
        return c;
    }

private:
    void set(Reason r, const std::string& m) {
        // First writer wins: a later stop must not relabel an abort.
        if (reason_ != Reason::Running) return;
        reason_ = r;
        message_ = m;
    }

    Reason reason_ = Reason::Running;
    std::string message_;
};

)SERT"
    R"SERT(// ─── Contract checks (section 10.6) ──────────────────────────────────────────
//
// A contract is a claim a body makes about its own inputs, states or outputs:
// this quaternion is unit-norm, this gain is finite, this duty cycle is in
// [0,1]. It is checked where the claim lives — at run time, in a named block
// instance, at a sim instant — which is exactly what separates it from
// anything `sec` could have decided at generation.
//
// Reporting is OBSERVATIONAL, on section 10.1's rule and for section 10.1's
// reason. `output()` and `derivative()` are evaluated at every minor step of
// every tick, so a check that halted there would both break section 10.2's
// confinement and end runs over trial states the trajectory never visits. A
// violation still FAILS THE RUN — summary() prints it and Sim::run() returns
// nonzero — it simply does so at teardown rather than mid-step. The halting
// form is RunControl::require below, confined where stop/abort are.

// One (node, contract) pair that has been violated at least once.
//
// The key is the two `const char*` values THEMSELVES, not their text. Both are
// string literals — the node path is emitted into the model, the contract name
// is written in the body — so their addresses are stable for the life of the
// process, and the same wording in two different leaves stays two different
// sites, which is what a reader wants to see.
struct CheckSite {
    const char*   path;
    const char*   contract;
    std::uint64_t count      = 0;
    double        first_time = 0.0;
    std::uint64_t first_tick = 0;
    std::string   first_detail;   // the values, as the first report printed them
};

class Checks {
public:
    // How many times one site reports before it goes quiet. Throttling is not
    // a nicety here: rk4 evaluates output() four times per tick, so a contract
    // violated for a whole 20 s run at 1 kHz would write 80,000 identical
    // lines and bury the first one, which is the only one that locates the
    // cause. The count keeps rising after the reports stop.
    static constexpr std::uint64_t report_limit = 5;

    // Reports go to `log`, the sink of the run this registry belongs to; the
    // two sit side by side in one `Run`, and the caller passes it.
    void fail(LogSink& log, const char* path, const char* type, const char* contract,
              const std::string& detail, double t, std::uint64_t tick) {
        CheckSite& s = site(path, contract);
        if (s.count == 0) {
            s.first_time   = t;
            s.first_tick   = tick;
            s.first_detail = detail;
        }
        ++s.count;
        if (s.count > report_limit) return;
        log.write(Level::Error, path, t,
                  se_rt::format("contract failed: {}; {}  ({}, tick {})",
                                contract, detail, type, tick));
        if (s.count == report_limit)
            log.write(Level::Error, path, t,
                      se_rt::format("contract failed: {}; further reports "
                                    "suppressed, the total is in the summary",
                                    contract));
    }

    bool any() const { return !sites_.empty(); }

    // The report a reader actually acts on, one line per violated site: the
    // first failure locates the cause in time, and the count says whether it
    // was a transient or the whole run.
    void summary(LogSink& log, double t) const {
        if (sites_.empty()) return;
        log.write(Level::Error, "sim", t,
                  se_rt::format("{} contract(s) violated", sites_.size()));
        for (const CheckSite& s : sites_)
            log.write(Level::Error, "sim", t,
                      se_rt::format("  {}: {} -- {} time(s), first at "
                                       "t = {} s (tick {}); {}",
                                       s.path, s.contract, s.count,
                                       s.first_time, s.first_tick,
                                       s.first_detail));
    }

private:
    // Linear, and deliberately: this runs only when a contract has ALREADY
    // failed, the site count is a handful, and two pointer comparisons beat
    // hashing a string. Nothing on the passing path reaches here at all.
    CheckSite& site(const char* path, const char* contract) {
        for (CheckSite& s : sites_)
            if (s.path == path && s.contract == contract) return s;
        sites_.push_back(CheckSite{path, contract, 0, 0.0, 0, std::string()});
        return sites_.back();
    }

    std::vector<CheckSite> sites_;
};

// ─── The clock (section 10.5) ────────────────────────────────────────────────
//
// What `sim.time()` reads, one per model, inside its `Run`. `now` is the
// instant of the CURRENT evaluation, not of the current tick: derivatives()
// rewrites it at every minor step, which is the whole reason a continuous
// source can be written at all.
struct RunState {
    double        now  = 0.0;   // the instant this evaluation is at
    double        step = 0.0;   // the global base step
    std::uint64_t tick = 0;     // base tick index
};

)SERT"
    R"SERT(// ─── Recording (section 13.5) ────────────────────────────────────────────────

class Recorder {
public:
    ~Recorder() { close(); }

    // `path` may hold `{time}`, which `stamp` fills (section 13.5).
    bool open(const std::string& path, const std::string& stamp) {
        file_ = open_output(path, stamp, used_);
        return file_ != nullptr;
    }

    // The table's name as opened, `{time}` filled in.
    const std::string& used() const { return used_; }

    void header(const std::vector<std::string>& columns) {
        if (!file_) return;
        std::fputs("time (s)", file_);
        for (const std::string& c : columns) {
            std::fputc(',', file_);
            std::fputs(c.c_str(), file_);
        }
        std::fputc('\n', file_);
    }

    void row(double t, const std::vector<double>& values) {
        if (!file_) return;
        std::fprintf(file_, "%.9g", t);
        for (double v : values) std::fprintf(file_, ",%.9g", v);
        std::fputc('\n', file_);
        ++rows_;
    }

    // Section 13.5: a row with string columns. `texts[c]` is column c's text
    // where `is_text[c]`, and every such cell is quoted the RFC 4180 way.
    void row(double t, const std::vector<double>& values, const std::vector<char>& is_text,
             const std::vector<std::string_view>& texts) {
        if (!file_) return;
        std::fprintf(file_, "%.9g", t);
        for (std::size_t c = 0; c < values.size(); ++c) {
            if (!is_text[c]) {
                std::fprintf(file_, ",%.9g", values[c]);
                continue;
            }
            std::fputs(",\"", file_);
            for (char ch : texts[c]) {
                if (ch == '"') std::fputc('"', file_);
                std::fputc(ch, file_);
            }
            std::fputc('"', file_);
        }
        std::fputc('\n', file_);
        ++rows_;
    }

    void close() {
        if (!file_) return;
        std::fclose(file_);
        file_ = nullptr;
    }

    std::uint64_t rows() const { return rows_; }
    bool active() const { return file_ != nullptr; }

private:
    std::FILE* file_ = nullptr;
    std::string used_;
    std::uint64_t rows_ = 0;
};

// ─── Real-time pacing (section 9.4, section 13.6) ────────────────────────────
//
// The host paces, not the model. tick() never waits; a host that wants to keep
// up with wall time calls sync() between ticks, passing time(), the instant the
// next tick samples. sync() busy-waits on the high-resolution timer until the
// wall time since the FIRST TICK (anchor()) equals that instant.
//
// A call that finds the wall already past it returns at once, and so do the
// calls after it until the model is level again: the model CATCHES UP, with no
// limit, and every base step still runs. Dropping steps to keep pace would
// punch a hole in a fixed-step integration; catching up is only late.
//
// Overrun is INSTRUMENTED, NOT POLICED: there is no abort escalation here. The
// metrics average the calls in the last second of model time, so they mean the
// same thing whatever number of ticks the host puts between calls.

class Realtime {
public:
    using clock = std::chrono::steady_clock;
    static constexpr double window_seconds = 1.0;   // of model time (section 13.6)

    // The first tick: what every later sync() measures from.
    void anchor() {
        start_ = last_ = clock::now();
        anchored_ = true;
    }

    void sync(double sim_time) {
        if (!anchored_ || sim_time <= last_sim_) return;   // no tick since the last call
        const clock::time_point target =
            start_ + std::chrono::duration_cast<clock::duration>(
                         std::chrono::duration<double>(sim_time));
        const clock::time_point now = clock::now();
        const double compute = std::chrono::duration<double>(now - last_).count();
        const bool overran = now > target;
        lag_ = overran ? std::chrono::duration<double>(now - target).count() : 0.0;

        window_.push_back({sim_time, compute / (sim_time - last_sim_) * 100.0, overran});
        while (window_.front().sim_time <= sim_time - window_seconds) window_.pop_front();
        ++frames_;
        if (overran) ++overruns_;
        last_sim_ = sim_time;

        if (!overran) {
            while (clock::now() < target) {
            }
        }
        last_ = clock::now();
    }

    double utilization() const {
        if (window_.empty()) return 0.0;
        double sum = 0.0;
        for (const Call& c : window_) sum += c.utilization;
        return sum / static_cast<double>(window_.size());
    }

    double overrun_pct() const {
        if (window_.empty()) return 0.0;
        std::size_t n = 0;
        for (const Call& c : window_)
            if (c.overran) ++n;
        return 100.0 * static_cast<double>(n) / static_cast<double>(window_.size());
    }

    double        lag() const { return lag_; }             // s the wall was ahead, last call
    std::uint64_t frames() const { return frames_; }       // sync() calls, the whole run
    std::uint64_t overruns() const { return overruns_; }   // of which overran

    // The whole-run cumulative summary, free off the same counters. Silent
    // when the host never paced the run. On the console, or in the log's
    // queue while the host is capturing it (section 14.1).
    void summary(LogSink& log, double t) const {
        if (frames_ == 0) return;
        char text[128];
        std::snprintf(text, sizeof text, "realtime: %llu frames, %llu overran (%.2f%% of the run)",
                      static_cast<unsigned long long>(frames_),
                      static_cast<unsigned long long>(overruns_),
                      100.0 * static_cast<double>(overruns_) / static_cast<double>(frames_));
        if (log.capturing()) log.keep(Level::Info, "sim", t, text);
        else std::fprintf(stderr, "%s\n", text);
    }

private:
    struct Call {
        double sim_time;      // s, the instant the call caught up to
        double utilization;   // % of the model time it covered, spent computing
        bool   overran;
    };
    bool anchored_ = false;
    clock::time_point start_;
    clock::time_point last_;   // when the previous call returned
    double last_sim_ = 0.0;
    double lag_ = 0.0;
    std::deque<Call> window_;
    std::uint64_t frames_ = 0;
    std::uint64_t overruns_ = 0;
};

)SERT"
    R"SERT(// ─── The run (one per model) ─────────────────────────────────────────────────
//
// Everything a run accumulates, in one object that `Sim::Impl` owns and every
// leaf points at. Nothing here is a process global, which is the whole point:
// a finished run leaves nothing behind for the next `Sim`, and two models in
// one program have two of these and cannot see each other's stop flag,
// contracts, pacing or log.
//
// A leaf holds `Run* const`, not a const pointer, because a contract check in
// a pure method records into `checks`. The pointer is what makes that legal;
// what a body may CALL is still decided by the handle it is given (RunView,
// RunControl, TraceLog, Log), exactly as before.
struct Run {
    RunState clock;              // section 10.5 — what `sim.time()` reads
    Control  control;            // section 10.2 — how the run ends, first writer wins
    Checks   checks;             // section 10.6 — every violated contract
    Realtime realtime;           // section 9.4  — pacing, when the host syncs
    LogSink  log;                // section 13.7 — this model's file and levels
    double   log_time = 0.0;     // the tick instant a record is stamped with
};

// `log_time` is NOT what a body reads. `sim.time()` is `clock.now`, which moves
// to a solver stage's instant at every minor step; a log record is stamped with
// the tick it was written in. The two agree at tick boundaries and are
// deliberately different values.

// The `log` object the generator puts in scope of init(), next() and final().
//
// It holds its model's run and the node path, and reads the time when it is
// called. That is what lets the restricted form below be a class member rather
// than a line of preamble in every method.
struct Log {
    Run*        run;
    const char* path;

    template <class... A> void trace(const char* f, const A&... a) const {
        run->log.write(Level::Trace, path, run->log_time, se_rt::format(f, a...));
    }
    template <class... A> void debug(const char* f, const A&... a) const {
        run->log.write(Level::Debug, path, run->log_time, se_rt::format(f, a...));
    }
    template <class... A> void info(const char* f, const A&... a) const {
        run->log.write(Level::Info, path, run->log_time, se_rt::format(f, a...));
    }
    template <class... A> void warn(const char* f, const A&... a) const {
        run->log.write(Level::Warn, path, run->log_time, se_rt::format(f, a...));
    }
    template <class... A> void error(const char* f, const A&... a) const {
        run->log.write(Level::Error, path, run->log_time, se_rt::format(f, a...));
    }
};

// The restricted form for the pure methods: `log.trace` alone (section
// 10.1). `log.info` there is "no such member".
//
// Every leaf holds one of these as a member called `log`, so a pure method
// needs no preamble at all. The mutating methods declare a local `Log log`
// which shadows it, widening what is legal exactly where section 8.4 says it is.
struct TraceLog {
    Run*        run;
    const char* path;

    template <class... A> void trace(const char* f, const A&... a) const {
        if (!run->log.trace_enabled) return;
        run->log.write(Level::Trace, path, run->log_time,
                       se_rt::format(f, a...) + "  [minor step]");
    }
};

// The `sim` object every body sees. Reflection only: every member is a
// function of the current instant or of the node itself, never of run
// history. That is the admission rule, and it is what lets `sim.time()` in
// while keeping `sim.overrun_count` out (section 10.2) — the latter's value
// depends on when you look, which is the objection that was never about time.
//
// Every leaf holds one of these as a member called `sim`, so a pure method
// needs no preamble at all. The mutating methods declare a local `RunControl`
// which shadows it, widening what is legal exactly where section 8.4 says it
// is. Same shape as TraceLog / Log above, for the same reason.
struct RunView {
    Run*            run;
    const char*     node_path;
    const char*     node_type;

    double        time() const { return run->clock.now; }
    double        step() const { return run->clock.step; }
    std::uint64_t tick() const { return run->clock.tick; }
    const char*   path() const { return node_path; }
    const char*   type() const { return node_type; }

    // ── Contracts (section 10.6) ────────────────────────────────────────────
    //
    // Legal in every body, observational everywhere, and each returns the
    // condition — so a body that cannot proceed on bad input can say
    //
    //     if (!sim.check_finite(in.a, "acceleration is finite")) return;
    //
    // The overloads that take VALUES are the point of the family. A body can
    // always write `check(fabs(n - 1.0) < tol, "unit norm")`, but the report
    // can then only say the condition was false; `check_near(n, 1.0, tol,
    // "unit norm")` says what `n` was and by how much it missed, which is the
    // difference between a diagnostic and a notification.
    bool check(bool ok, const char* contract) const {
        if (!ok) contract_failed(contract, "condition is false");
        return ok;
    }
    bool check(bool ok, const char* contract, double v) const {
        if (!ok) contract_failed(contract, se_rt::format("value {}", v));
        return ok;
    }
    bool check_finite(double v, const char* contract) const {
        const bool ok = std::isfinite(v);
        if (!ok)
            contract_failed(contract,
                            se_rt::format("got {}, want a finite value", v));
        return ok;
    }
    bool check_range(double v, double lo, double hi, const char* contract) const {
        // NaN fails this, and says so through the same channel: neither
        // comparison holds, and the overshoot below prints as nan too.
        const bool ok = v >= lo && v <= hi;
        if (!ok)
            contract_failed(
                contract, se_rt::format("got {}, want [{}, {}], outside by {}",
                                        v, lo, hi, v < lo ? lo - v : v - hi));
        return ok;
    }
    bool check_near(double a, double b, double tol, const char* contract) const {
        const double e  = std::fabs(a - b);
        const bool   ok = e <= tol;
        if (!ok)
            contract_failed(contract,
                            se_rt::format("got {}, want {}, |err| {} > tol {}",
                                          a, b, e, tol));
        return ok;
    }

protected:
    // Off the caller's path on purpose: every check above is a branch that does
    // no work at all when the contract holds, and the message is built only
    // here. RunControl::require reaches it for the halting form.
    void contract_failed(const char* contract, const std::string& detail) const {
        run->checks.fail(run->log, node_path, node_type, contract, detail,
                         run->clock.now, run->clock.tick);
    }
};

// The widened form for init(), next() and final(): everything above plus
// the two calls that end a run (section 10.2). `sim.stop` in output() is "no
// member named stop" — a compile error at the .se line, not a runtime rule.
struct RunControl : RunView {
    RunControl(Run* r, const char* p, const char* t) : RunView{r, p, t} {}

    void stop(const std::string& m) const { run->control.stop(m); }
    void abort(const std::string& m) const { run->control.abort(m); }

    // section 10.6 — the halting form of a contract, confined here for exactly
    // the reason stop/abort are. A claim that must not be survived belongs in a
    // mutating method; in `output()` this is "no member named require", a
    // compile error at the .se line, while `sim.check_*` there still works.
    bool require(bool ok, const char* contract) const {
        if (!ok) require_failed(contract, "condition is false");
        return ok;
    }
    bool require(bool ok, const char* contract, double v) const {
        if (!ok) require_failed(contract, se_rt::format("value {}", v));
        return ok;
    }

private:
    // Reported through the same registry as a check, so the summary counts it
    // and the exit code follows, and THEN the run is ended. Control::fail is
    // first-writer-wins, so the first violated contract is the one named.
    void require_failed(const char* contract, const std::string& detail) const {
        contract_failed(contract, detail);
        run->control.fail(se_rt::format("contract failed: {} at {}",
                                        contract, node_path));
    }
};

)SERT"
    R"SERT(// ─── Units at configuration (section 14.1) ──────────────────────────────────
//
// A settings source reaches a compiled model after units have been erased
// (section 15.6), so the loader carries what it needs to check and convert a
// value's unit: the unit symbols, and a parser for section 4.1's grammar. The
// table inside unit_symbol() is not written by hand. `sec` fills it in from
// its own table when it writes this runtime, so any unit `sec` accepts, the
// model accepts, and the conversions agree to the last bit.

struct UnitSymbol {
    const char* sym;
    signed char dim[8];   // m kg s A K mol cd rad
    double scale;         // SI = value * scale + offset
    double offset;
};

struct UnitPrefix {
    const char* sym;
    double scale;
};

// A unit as the loader uses it. The exponents are doubles because section
// 4.1 allows rational ones, `m^(1/2)`, so they are compared with a tolerance.
struct Unit {
    double dim[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    double scale = 1.0;
    double offset = 0.0;
};

// An exact symbol first, then the longest prefix on an exact symbol, so `min`
// is the minute, `Pa` the pascal and `mg` a milligram. A prefixed offset unit
// (`mdegC`) is not a unit at all.
inline bool unit_symbol(const std::string& s, Unit& out) {
@UNIT_TABLE@
    auto take = [&](const UnitSymbol& d, double prefix) {
        for (int i = 0; i < 8; ++i) out.dim[i] = d.dim[i];
        out.scale = d.scale * prefix;
        out.offset = d.offset;
    };
    for (const UnitSymbol& d : symbols)
        if (s == d.sym) {
            take(d, 1.0);
            return true;
        }
    for (const UnitPrefix& p : prefixes) {
        const std::size_t n = std::strlen(p.sym);
        if (s.size() <= n || s.compare(0, n, p.sym) != 0) continue;
        for (const UnitSymbol& d : symbols)
            if (d.offset == 0.0 && s.compare(n, std::string::npos, d.sym) == 0) {
                take(d, p.scale);
                return true;
            }
    }
    return false;
}

// Section 4.1's grammar, over the text between a value's parentheses:
//
//     unit   := '-' | term { ('*' | '/') term }
//     term   := factor [ '^' ( int | '(' int [ '/' int ] ')' ) ]
//     factor := '1' | symbol | '%' | '(' unit ')'
//
// An offset unit (degC, degF) is legal only on its own, as it is at
// elaboration: `degC/s` cannot say whether it means a temperature or a
// difference of two.
class UnitParser {
public:
    explicit UnitParser(const std::string& text) : s_(text) {}

    bool parse(Unit& out) {
        if (!expr(out)) return false;
        skip();
        if (i_ != s_.size()) return fail("unexpected `" + s_.substr(i_, 1) + "`");
        if (!offset_sym_.empty() && compound_)
            return fail("offset unit `" + offset_sym_ +
                        "` in a compound unit; it is only legal on its own");
        return true;
    }
    const std::string& error() const { return err_; }

private:
    void skip() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t')) ++i_;
    }
    char peek() {
        skip();
        return i_ < s_.size() ? s_[i_] : '\0';
    }
    bool fail(const std::string& m) {
        if (err_.empty()) err_ = m;
        return false;
    }

    bool expr(Unit& u) {
        // `(-)`, dimensionless: alone, or alone inside parentheses.
        if (peek() == '-') {
            const std::size_t at = i_++;
            const char next = peek();
            if (next == '\0' || next == ')') {
                u = Unit{};
                return true;
            }
            i_ = at;
        }
        if (!term(u)) return false;
        for (char op = peek(); op == '*' || op == '/'; op = peek()) {
            ++i_;
            compound_ = true;
            Unit r;
            if (!term(r)) return false;
            const double sign = op == '*' ? 1.0 : -1.0;
            for (int k = 0; k < 8; ++k) u.dim[k] += sign * r.dim[k];
            u.scale = op == '*' ? u.scale * r.scale : u.scale / r.scale;
            u.offset = 0.0;
        }
        return true;
    }

    bool term(Unit& u) {
        if (!factor(u)) return false;
        if (peek() != '^') return true;
        ++i_;
        compound_ = true;
        long num = 1, den = 1;
        if (peek() == '(') {
            ++i_;
            if (!integer(num)) return false;
            if (peek() == '/') {
                ++i_;
                if (!integer(den)) return false;
                if (den == 0) return fail("a unit exponent has a zero denominator");
            }
            if (peek() != ')') return fail("expected `)` to close the exponent");
            ++i_;
        } else if (!integer(num)) {
            return false;
        }
        const double e = static_cast<double>(num) / static_cast<double>(den);
        for (int k = 0; k < 8; ++k) u.dim[k] *= e;
        u.scale = std::pow(u.scale, e);
        u.offset = 0.0;
        return true;
    }

    bool integer(long& out) {
        bool neg = false;
        if (peek() == '-') {
            neg = true;
            ++i_;
        }
        skip();
        const std::size_t at = i_;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        if (i_ == at) return fail("a unit exponent must be an integer");
        out = std::strtol(s_.substr(at, i_ - at).c_str(), nullptr, 10);
        if (neg) out = -out;
        return true;
    }

    static bool ident_char(char c, bool first) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
               (!first && c >= '0' && c <= '9');
    }

    bool factor(Unit& u) {
        const char c = peek();
        if (c == '(') {
            ++i_;
            if (!expr(u)) return false;
            if (peek() != ')') return fail("expected `)` to close the nested unit");
            ++i_;
            return true;
        }
        if (c == '1' && (i_ + 1 >= s_.size() || !(s_[i_ + 1] >= '0' && s_[i_ + 1] <= '9'))) {
            ++i_;
            u = Unit{};
            return true;
        }
        std::string sym;
        if (c == '%') {
            ++i_;
            sym = "%";
        } else if (ident_char(c, true)) {
            const std::size_t at = i_;
            while (i_ < s_.size() && ident_char(s_[i_], i_ == at)) ++i_;
            sym = s_.substr(at, i_ - at);
        } else {
            return fail(c ? std::string("expected a unit symbol, found `") + c + "`"
                          : std::string("expected a unit symbol"));
        }
        if (!unit_symbol(sym, u)) return fail("unknown unit symbol `" + sym + "`");
        if (u.offset != 0.0) offset_sym_ = sym;
        return true;
    }

    std::string s_;
    std::size_t i_ = 0;
    std::string err_;
    std::string offset_sym_;
    bool compound_ = false;
};

// Section 4.2 with section 4.5's angle rule, exactly as `sec` applies it: the
// angle exponent is ignored where the VALUE's length exponent is positive
// (`rad*m` is a length), and counts everywhere else (`rad/s` is not `Hz`).
inline bool unit_compatible(const Unit& value, const Unit& target) {
    auto same = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    for (int k = 0; k < 7; ++k)
        if (!same(value.dim[k], target.dim[k])) return false;
    if (value.dim[0] > 1e-9) return true;
    return same(value.dim[7], target.dim[7]);
}

inline double unit_convert(double v, const Unit& from, const Unit& to) {
    if (from.scale == to.scale && from.offset == to.offset) return v;
    return (v * from.scale + from.offset - to.offset) / to.scale;
}

)SERT"
    R"SERT(// ─── Settings sources (section 14) ───────────────────────────────────────────
//
// The model's half of section 14.1. A generated Sim describes every path a
// source may bind with one SettingRow, and these functions do the rest:
// read the text, check each binding against its row, and write a dump back
// out. The generated side only moves numbers in and out of its members.

struct Enumerator {
    const char* name;
    long value;
};

enum class RowKind : unsigned char {
    Setting,   // a setting, at any depth; one leaf of a record setting
    Initial,   // a scalar state's initial value (section 13.4)
    Element,   // an array state, bound one element at a time
    Run,       // sim.duration, the run's length (section 13.3)
};

struct SettingRow {
    const char* path;
    RowKind kind;
    const char* type;        // "double", "float", "int", "bool", or a finite type
    const char* unit;        // canonical, as the manifest spells it; "-" for none
    double dim[8];
    double scale;
    double offset;
    const Enumerator* enumerators;   // a finite type's, in declaration order
    std::size_t n_enumerators;
    bool structural;         // fixed at generation (section 6.2); refused on load
    std::size_t capacity;    // a string's, in bytes (section 5.1b); 0 for anything else
};

// Section 5.1b: whether a row holds text.
inline bool is_text_row(const SettingRow& r) { return std::strcmp(r.type, "string") == 0; }

// One checked binding, in the target's canonical unit. An enumerator is its
// value; a bool is 0 or 1; a string is `text`.
struct Assignment {
    std::size_t row;
    std::size_t index;       // an Element row's element; 0 otherwise
    double value;
    std::string text;        // a string row's value (section 5.1b)
};

// ─── The run's own configuration (section 13.3) ─────────────────────────────
//
// What a source says about the run rather than the model: which channels are
// recorded, where and how often (section 13.5), the log (section 13.7), and
// the file that drives the root inputs (section 13.8). The .sim's entries are
// where a run starts; each section edits that, in order, and a later source
// edits what an earlier one left.

// One recordable channel. An array state is one row, its elements named by
// index at the extent configuration settles.
struct ChannelRow {
    const char* path;        // canonical: "whl.state.omega"; an array without its index
    const char* label;       // the CSV header's bracket: a unit, a finite type, buffer(N)
    bool array;
};

// A bare spelling of a leaf member: `whl.omega` for `whl.state.omega`. When
// `other` is set the spelling is ambiguous, and `row` is the state while
// `other` names the output (section 6.11).
struct ChannelAlias {
    const char* spelling;
    std::size_t row;
    const char* other;
};

// One column: a channel, its element, and the path as written where it was
// added, which is what the header shows.
struct Column {
    std::size_t row;
    std::size_t index;
    std::string written;
};

struct RunConfig {
    std::vector<Column> columns;
    std::string record_file = "run.csv";
    std::uint64_t every = 1;
    std::string log_file;                 // empty: the console only
    Level level = Level::Info;
    std::vector<std::pair<std::string, Level>> levels;
    std::string inputs_file;              // empty: the host drives every input (section 13.8)

    // Adding a channel that is already selected keeps its place; removing one
    // that is not does nothing, so a "turn these off" source layers on any
    // baseline (section 13.5).
    void add(const Column& c) {
        for (const Column& k : columns)
            if (k.row == c.row && k.index == c.index) return;
        columns.push_back(c);
    }
    void remove(std::size_t row, std::size_t index) {
        for (std::size_t k = 0; k < columns.size(); ++k)
            if (columns[k].row == row && columns[k].index == index) {
                columns.erase(columns.begin() + static_cast<std::ptrdiff_t>(k));
                return;
            }
    }
    void path_level(const std::string& path, Level l) {
        for (auto& p : levels)
            if (p.first == path) {
                p.second = l;
                return;
            }
        levels.emplace_back(path, l);
    }
};

// One edit a source makes to it. A source's edits are kept, in order, until
// the whole source has been checked, so a refused source changes nothing.
struct RunEdit {
    enum class Kind : unsigned char {
        Clear, Add, Remove, File, Every, LogFile, LogLevel, PathLevel, InputsFile
    };
    Kind kind = Kind::Clear;
    Column column{0, 0, std::string()};
    std::string text;        // a file name, or the path a level is for
    std::uint64_t every = 0;
    Level level = Level::Info;
};

inline void apply(RunConfig& c, const RunEdit& e) {
    switch (e.kind) {
        case RunEdit::Kind::Clear:     c.columns.clear(); break;
        case RunEdit::Kind::Add:       c.add(e.column); break;
        case RunEdit::Kind::Remove:    c.remove(e.column.row, e.column.index); break;
        case RunEdit::Kind::File:      c.record_file = e.text; break;
        case RunEdit::Kind::Every:     c.every = e.every; break;
        case RunEdit::Kind::LogFile:   c.log_file = e.text; break;
        case RunEdit::Kind::LogLevel:  c.level = e.level; break;
        case RunEdit::Kind::PathLevel: c.path_level(e.text, e.level); break;
        case RunEdit::Kind::InputsFile: c.inputs_file = e.text; break;
    }
}

inline bool level_of(const std::string& s, Level& out) {
    for (int k = 0; k <= static_cast<int>(Level::Error); ++k)
        if (s == level_name(static_cast<Level>(k))) {
            out = static_cast<Level>(k);
            return true;
        }
    return false;
}

// What a source is checked against: the paths its bindings may reach, and the
// channels its `record` sections may name.
struct Tables {
    const SettingRow* rows;
    std::size_t n_rows;
    const ChannelRow* channels;
    std::size_t n_channels;
    const ChannelAlias* aliases;
    std::size_t n_aliases;
    double step;             // s: sim.duration is a whole number of these
};

// A channel path as `sec` resolves one (section 13.5): canonical, or a bare
// spelling that is not ambiguous. `shown` is the path as written, its index
// included. False, with `why`, when it names no channel.
inline bool channel_of(const Tables& t, const std::string& path, bool indexed, std::size_t index,
                       const std::string& shown, Column& out, std::string& why) {
    std::size_t row = t.n_channels;
    for (std::size_t r = 0; r < t.n_channels; ++r)
        if (path == t.channels[r].path) {
            row = r;
            break;
        }
    for (std::size_t a = 0; row == t.n_channels && a < t.n_aliases; ++a) {
        if (path != t.aliases[a].spelling) continue;
        if (t.aliases[a].other) {
            why = std::string("ambiguous: `") + t.channels[t.aliases[a].row].path +
                  "` names the state, and `" + t.aliases[a].other + "` the output (section 6.11)";
            return false;
        }
        row = t.aliases[a].row;
    }
    if (row == t.n_channels) {
        why = "not a recordable signal of this model; list_signals() names every one "
              "(section 13.5)";
        return false;
    }
    if (t.channels[row].array && !indexed) {
        why = "an array state, recorded one element at a time, as `" + path + "[0]`";
        return false;
    }
    if (!t.channels[row].array && indexed) {
        why = "not an array state, so it takes no index";
        return false;
    }
    out = Column{row, index, shown};
    return true;
}

// A channel path given whole, as the channel calls take it:
// "whl.state.omega", "bw.state.w1[3]".
inline bool channel_text(const Tables& t, const char* text, Column& out, std::string& why) {
    std::string s = text ? text : "";
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    std::string path = s;
    bool indexed = false;
    std::size_t index = 0;
    const std::size_t lb = s.find('[');
    if (lb != std::string::npos) {
        const std::string digits = s.substr(lb + 1, s.size() > lb + 2 ? s.size() - lb - 2 : 0);
        bool ok = s.back() == ']' && !digits.empty();
        for (char c : digits) ok = ok && c >= '0' && c <= '9';
        if (!ok) {
            why = "an element index is a non-negative integer, `[3]`";
            return false;
        }
        index = static_cast<std::size_t>(std::strtoull(digits.c_str(), nullptr, 10));
        path = s.substr(0, lb);
        indexed = true;
    }
    bool ok = !path.empty() && path.front() != '.' && path.back() != '.';
    for (std::size_t k = 0; ok && k < path.size(); ++k) {
        const char c = path[k];
        const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                          (c >= '0' && c <= '9');
        ok = word || (c == '.' && path[k + 1] != '.');
    }
    if (!ok) {
        why = "expected a path, as `plant.out.velocity`";
        return false;
    }
    return channel_of(t, path, indexed, index, s, out, why);
}

)SERT"
    R"SERT(// ─── The settings source, read by a compiled model (section 14.1) ───────────

// One source as the loader reads it: bindings of a number with an optional
// unit, or a bare name for an enumerator, and the run's `record`, `log` and
// `inputs` sections. Section 14.1 accepts nothing more at run time, and the
// reader says so rather than evaluating a section-7 expression.
class SourceReader {
public:
    // `located` false: a fault names only the path, as one key-value call
    // reports it, with no origin and line. `base`: the directory of the file
    // the text came from, with its trailing separator, where a relative
    // inputs file is found (section 13.8); empty for the working directory.
    SourceReader(const char* text, const char* origin, bool located = true,
                 const std::string& base = std::string())
        : s_(text ? text : ""), origin_(origin && *origin ? origin : "settings"),
          located_(located), base_(base) {
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) i_ = 3;   // a UTF-8 BOM, as editors save one
    }

    // All or nothing: false at the first fault, with error() naming it.
    bool check(const Tables& t, std::vector<Assignment>& out, std::vector<RunEdit>& edits) {
        out.clear();
        edits.clear();
        std::vector<Assignment> got;
        std::vector<RunEdit> run;
        for (;;) {
            if (!blank()) return false;
            if (i_ >= s_.size()) break;
            std::string which;
            if (at_section(which)) {
                const bool ok = which == "record" ? record_section(t, run)
                                : which == "log"  ? log_section(run)
                                                  : inputs_section(run);
                if (!ok) return false;
                continue;
            }
            Assignment a{};
            if (!binding(t, a)) return false;
            got.push_back(a);
        }
        out.swap(got);
        edits.swap(run);
        return true;
    }
    const std::string& error() const { return err_; }

private:
    // A fault is reported on the line its binding starts on, so a missing `;`
    // is blamed on the value that lacks it and not on the line after.
    bool fail(const std::string& path, const std::string& m) {
        err_ = (located_ ? origin_ + ":" + std::to_string(at_line_) + ": " : std::string()) +
               (path.empty() ? "" : path + ": ") + m;
        return false;
    }

    // Whitespace and section 3.2's comments, counting lines.
    bool blank() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\n') {
                ++line_;
                ++i_;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++i_;
            } else if (s_.compare(i_, 2, "//") == 0) {
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
            } else if (s_.compare(i_, 2, "/*") == 0) {
                const int opened = line_;
                const std::size_t end = s_.find("*/", i_ + 2);
                if (end == std::string::npos) {
                    at_line_ = opened;
                    return fail("", "unterminated block comment");
                }
                for (std::size_t k = i_; k < end; ++k)
                    if (s_[k] == '\n') ++line_;
                i_ = end + 2;
            } else {
                break;
            }
        }
        return true;
    }

    static bool ident_char(char c, bool first) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
               (!first && c >= '0' && c <= '9');
    }
    static bool digit(char c) { return c >= '0' && c <= '9'; }

    bool ident(std::string& out) {
        const std::size_t at = i_;
        while (i_ < s_.size() && ident_char(s_[i_], i_ == at)) ++i_;
        out = s_.substr(at, i_ - at);
        return !out.empty();
    }

    // Section 3.5: digits [ '.' digits ] [ exponent ], or '.' digits [ exponent ].
    bool number(double& out) {
        const std::size_t at = i_;
        while (i_ < s_.size() && digit(s_[i_])) ++i_;
        const bool whole = i_ > at;
        bool frac = false;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && digit(s_[i_])) {
                ++i_;
                frac = true;
            }
        }
        if (!whole && !frac) return false;
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            std::size_t k = i_ + 1;
            if (k < s_.size() && (s_[k] == '+' || s_[k] == '-')) ++k;
            if (k < s_.size() && digit(s_[k])) {
                i_ = k;
                while (i_ < s_.size() && digit(s_[i_])) ++i_;
            }
        }
        out = std::strtod(s_.substr(at, i_ - at).c_str(), nullptr);
        return true;
    }

    // A string, section 3.6: `"` ... `"`, nothing across a line, and exactly
    // the escapes `sec` reads: \" \\ \n \r \t \0.
    bool string_lit(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        out.clear();
        for (++i_; i_ < s_.size(); ++i_) {
            const char c = s_[i_];
            if (c == '\n') return false;
            if (c == '"') {
                ++i_;
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (++i_ >= s_.size()) return false;
            switch (s_[i_]) {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case '0':  out += '\0'; break;
                default:   return false;
            }
        }
        return false;
    }

    // path := identifier { '.' identifier } [ '[' integer ']' ]. `shown` is
    // the path as a message names it, index included.
    bool read_path(std::string& path, bool& indexed, std::size_t& index, std::string& shown) {
        std::string seg;
        if (!ident(seg)) return fail("", "expected a path");
        path = seg;
        while (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (!ident(seg)) return fail(path, "expected a name after `.`");
            path += "." + seg;
        }
        indexed = false;
        index = 0;
        shown = path;
        if (i_ < s_.size() && s_[i_] == '[') {
            ++i_;
            const std::size_t at = i_;
            while (i_ < s_.size() && digit(s_[i_])) ++i_;
            if (i_ == at || i_ >= s_.size() || s_[i_] != ']')
                return fail(path, "an element index is a non-negative integer, `[3]`");
            index = static_cast<std::size_t>(std::strtoull(s_.substr(at, i_ - at).c_str(),
                                                           nullptr, 10));
            ++i_;
            indexed = true;
            shown = path + "[" + std::to_string(index) + "]";
        }
        return true;
    }

    // `;` closes a line; at the same line's next token anything else is more
    // than a compiled model reads.
    bool semi(const std::string& shown, const char* what) {
        if (!blank()) return false;
        if (i_ < s_.size() && s_[i_] == ';') {
            ++i_;
            return true;
        }
        return fail(shown, std::string("expected `;` after ") + what);
    }

    // `record`, `log` or `inputs`, then `{`: a section (section 14). Anything
    // else, even a path that starts `record.`, is a binding.
    bool at_section(std::string& which) {
        const std::size_t save_i = i_;
        const int save_line = line_;
        std::string w;
        if (ident(w) && (w == "record" || w == "log" || w == "inputs")) {
            const std::string saved_err = err_;
            if (blank() && i_ < s_.size() && s_[i_] == '{') {
                ++i_;
                which = w;
                return true;
            }
            err_ = saved_err;
        }
        i_ = save_i;
        line_ = save_line;
        return false;
    }

)SERT"
    // Inside SourceReader: the split only has to keep each piece under the cap.
    R"SERT(    // Section 13.5: `clear;`, `path;`, `-path;`, `file = "...";`, `every = N;`.
    bool record_section(const Tables& t, std::vector<RunEdit>& run) {
        for (;;) {
            if (!blank()) return false;
            at_line_ = line_;
            if (i_ >= s_.size()) return fail("", "expected `}` to close the `record` section");
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            RunEdit e;
            bool remove = false;
            if (s_[i_] == '-') {
                ++i_;
                if (!blank()) return false;
                remove = true;
            } else {
                const std::size_t save_i = i_;
                const int save_line = line_;
                std::string w;
                if (!ident(w))
                    return fail("", "expected a channel path, `-path`, `clear`, `file =` or "
                                    "`every =`");
                if (!blank()) return false;
                if (i_ < s_.size() && s_[i_] == '=') {
                    ++i_;
                    if (!blank()) return false;
                    if (w == "file") {
                        e.kind = RunEdit::Kind::File;
                        if (!string_lit(e.text) || e.text.empty())
                            return fail(w, "expected a file name, in quotes");
                        const std::string bad = file_pattern_error(e.text);
                        if (!bad.empty()) return fail(w, bad);
                    } else if (w == "every") {
                        double v = 0.0;
                        if (!number(v) || v < 1.0 || std::floor(v) != v)
                            return fail(w, "takes a whole number of base steps, 1 or more");
                        e.kind = RunEdit::Kind::Every;
                        e.every = static_cast<std::uint64_t>(v);
                    } else {
                        return fail(w, "not a `record` key; a `record` section takes channels, "
                                       "`-channel;`, `clear;`, `file = ...;` and `every = ...;` "
                                       "(section 13.5)");
                    }
                    if (!semi(w, w == "file" ? "the file name" : "`every`")) return false;
                    run.push_back(e);
                    continue;
                }
                if (w == "clear" && i_ < s_.size() && s_[i_] == ';') {
                    ++i_;
                    e.kind = RunEdit::Kind::Clear;
                    run.push_back(e);
                    continue;
                }
                i_ = save_i;   // a channel path after all
                line_ = save_line;
            }
            std::string path, shown, why;
            bool indexed = false;
            std::size_t index = 0;
            if (!read_path(path, indexed, index, shown)) return false;
            if (!semi(shown, "the channel path")) return false;
            if (!channel_of(t, path, indexed, index, shown, e.column, why)) return fail(shown, why);
            e.kind = remove ? RunEdit::Kind::Remove : RunEdit::Kind::Add;
            run.push_back(e);
        }
    }

    bool level_word(const std::string& shown, Level& out) {
        std::string w;
        if (!ident(w) || !level_of(w, out))
            return fail(shown, "expected a log level: trace, debug, info, warn or error");
        return true;
    }

    // Section 13.7: `file = "...";`, `level = L;`, `levels { path = L; }`.
    bool log_section(std::vector<RunEdit>& run) {
        for (;;) {
            if (!blank()) return false;
            at_line_ = line_;
            if (i_ >= s_.size()) return fail("", "expected `}` to close the `log` section");
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            std::string w;
            if (!ident(w)) return fail("", "expected `file`, `level` or `levels`");
            if (!blank()) return false;
            if (w == "levels") {
                if (i_ >= s_.size() || s_[i_] != '{') return fail(w, "expected `{`");
                ++i_;
                for (;;) {
                    if (!blank()) return false;
                    at_line_ = line_;
                    if (i_ < s_.size() && s_[i_] == '}') {
                        ++i_;
                        break;
                    }
                    RunEdit e;
                    e.kind = RunEdit::Kind::PathLevel;
                    std::string shown;
                    bool indexed = false;
                    std::size_t index = 0;
                    if (!read_path(e.text, indexed, index, shown)) return false;
                    if (indexed) return fail(shown, "a node path takes no index");
                    if (!blank()) return false;
                    if (i_ >= s_.size() || s_[i_] != '=')
                        return fail(shown, "expected `=` after the node path");
                    ++i_;
                    if (!blank() || !level_word(shown, e.level) || !semi(shown, "the level"))
                        return false;
                    run.push_back(e);
                }
                continue;
            }
            if (w != "file" && w != "level")
                return fail(w, "not a `log` key; a `log` section takes `file = ...;`, "
                               "`level = ...;` and `levels { }` (section 13.7)");
            if (i_ >= s_.size() || s_[i_] != '=') return fail(w, "expected `=`");
            ++i_;
            if (!blank()) return false;
            RunEdit e;
            if (w == "file") {
                e.kind = RunEdit::Kind::LogFile;
                if (!string_lit(e.text))
                    return fail(w, "expected a file name, in quotes; \"\" for the console only");
                const std::string bad = file_pattern_error(e.text);
                if (!bad.empty()) return fail(w, bad);
            } else {
                e.kind = RunEdit::Kind::LogLevel;
                if (!level_word(w, e.level)) return false;
            }
            if (!semi(w, w == "file" ? "the file name" : "the level")) return false;
            run.push_back(e);
        }
    }

    // Section 13.8: `file = "...";`, found beside the source when it came
    // from a file.
    bool inputs_section(std::vector<RunEdit>& run) {
        for (;;) {
            if (!blank()) return false;
            at_line_ = line_;
            if (i_ >= s_.size()) return fail("", "expected `}` to close the `inputs` section");
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            std::string w;
            if (!ident(w)) return fail("", "expected `file`");
            if (!blank()) return false;
            if (w != "file")
                return fail(w, "not an `inputs` key; an `inputs` section takes `file = ...;` "
                               "(section 13.8)");
            if (i_ >= s_.size() || s_[i_] != '=') return fail(w, "expected `=`");
            ++i_;
            if (!blank()) return false;
            RunEdit e;
            e.kind = RunEdit::Kind::InputsFile;
            if (!string_lit(e.text))
                return fail(w, "expected a file name, in quotes; \"\" for none");
            const std::string bad = inputs_file_error(e.text);
            if (!bad.empty()) return fail(w, bad);
            e.text = beside(base_, e.text);
            if (!semi(w, "the file name")) return false;
            run.push_back(e);
        }
    }

    bool binding(const Tables& t, Assignment& a) {
        const SettingRow* rows = t.rows;
        const std::size_t n = t.n_rows;
        at_line_ = line_;
        std::string path, shown;
        bool indexed = false;
        std::size_t index = 0;
        if (!read_path(path, indexed, index, shown)) return false;

        if (!blank()) return false;
        if (i_ >= s_.size() || s_[i_] != '=') return fail(shown, "expected `=` after the path");
        ++i_;
        if (!blank()) return false;

        // value := [ '-' | '+' ] number [ '(' unit ')' ] | enumerator | string
        bool is_name = false, has_unit = false, is_text = false;
        std::string name, unit, text;
        double v = 0.0;
        if (i_ < s_.size() && s_[i_] == '"') {
            if (!string_lit(text))
                return fail(shown, "a string is closed on its own line, and its escapes are "
                                   "\\\" \\\\ \\n \\r \\t \\0 (section 3.6)");
            is_text = true;
        } else if (i_ < s_.size() && ident_char(s_[i_], true)) {
            ident(name);
            is_name = true;
        } else {
            double sign = 1.0;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) {
                sign = s_[i_] == '-' ? -1.0 : 1.0;
                ++i_;
                if (!blank()) return false;
            }
            if (!number(v)) return fail(shown, "expected a number or an enumerator");
            v *= sign;
        }
        if (!blank()) return false;
        if (i_ < s_.size() && s_[i_] == '(') {
            if (is_name) return fail(shown, "an enumerator takes no unit");
            if (is_text) return fail(shown, "a string takes no unit");
            int depth = 0;
            const std::size_t at = i_;
            for (; i_ < s_.size(); ++i_) {
                if (s_[i_] == '(') ++depth;
                if (s_[i_] == ')' && --depth == 0) break;
                if (s_[i_] == '\n' || s_[i_] == ';') break;
            }
            if (i_ >= s_.size() || s_[i_] != ')') return fail(shown, "expected `)` to close the unit");
            unit = s_.substr(at + 1, i_ - at - 1);
            ++i_;
            has_unit = true;
            if (!blank()) return false;
        }
        if (i_ >= s_.size() || s_[i_] != ';') {
            if (i_ < s_.size() && line_ == at_line_)
                return fail(shown, "a compiled model accepts a number with an optional unit, "
                                   "an enumerator or a string, and not an expression "
                                   "(section 14.1)");
            return fail(shown, "expected `;` after the value");
        }
        ++i_;

        // The target.
        const SettingRow* row = nullptr;
        for (std::size_t r = 0; r < n; ++r)
            if (path == rows[r].path) {
                row = &rows[r];
                a.row = r;
                break;
            }
        if (!row) {
            const std::string prefix = path + ".";
            // A record setting or an instance: a path with rows beneath it.
            for (std::size_t r = 0; r < n; ++r)
                if (std::strncmp(rows[r].path, prefix.c_str(), prefix.size()) == 0)
                    return fail(shown, std::string("not a single setting or state; a record or "
                                                   "an instance is set one leaf at a time, as `") +
                                           rows[r].path + "`");
            return fail(shown, "not a setting or a state of this model (an output is computed, "
                               "and cannot be set)");
        }
        if (row->kind == RowKind::Element && !indexed)
            return fail(shown, "an array state is set one element at a time, as `" + path + "[0]`");
        if (row->kind != RowKind::Element && indexed)
            return fail(shown, "not an array state, so it takes no index");
        if (row->structural)
            return fail(shown, "structural: it was fixed when the model was generated (section 6.2)");
        a.index = index;

        // The value. Section 5.1b: text goes only to text, and must fit.
        if (is_text_row(*row)) {
            if (!is_text)
                return fail(shown, "expected a string in double quotes, as `\"text\"` "
                                   "(section 5.1b)");
            if (text.size() > row->capacity)
                return fail(shown, std::to_string(text.size()) + " bytes, and it holds " +
                                       std::to_string(row->capacity) + " (section 5.1b)");
            a.value = 0.0;
            a.text = std::move(text);
            return true;
        }
        if (is_text)
            return fail(shown, "a string where a number is expected (section 5.1b)");
        if (row->enumerators) {
            if (!is_name)
                return fail(shown, std::string("expected an enumerator of ") + row->type +
                                       "; a finite type converts to nothing (section 5.2a)");
            for (std::size_t k = 0; k < row->n_enumerators; ++k)
                if (name == row->enumerators[k].name) {
                    a.value = static_cast<double>(row->enumerators[k].value);
                    return true;
                }
            std::string list;
            for (std::size_t k = 0; k < row->n_enumerators; ++k)
                list += (k ? ", " : "") + std::string(row->enumerators[k].name);
            return fail(shown, "`" + name + "` is not an enumerator of " + row->type +
                                   "; it has " + list);
        }
        if (is_name)
            return fail(shown, "a compiled model accepts a number with an optional unit, and "
                               "not a name or an expression (section 14.1)");
        if (has_unit) {
            Unit from, to;
            UnitParser p(unit);
            if (!p.parse(from)) return fail(shown, p.error());
            for (int k = 0; k < 8; ++k) to.dim[k] = row->dim[k];
            to.scale = row->scale;
            to.offset = row->offset;
            std::string written = unit;
            while (!written.empty() && written.front() == ' ') written.erase(0, 1);
            while (!written.empty() && written.back() == ' ') written.pop_back();
            if (!unit_compatible(from, to))
                return fail(shown, "incompatible units: (" + written + ") where (" + row->unit +
                                       ") is declared");
            v = unit_convert(v, from, to);
        }
        if (std::strcmp(row->type, "int") == 0) {
            const double r = std::floor(v + 0.5);
            if (std::fabs(v - r) > 1e-9) {
                std::ostringstream o;
                o << "lossy conversion to an `int` target: the value is " << v;
                return fail(shown, o.str());
            }
            v = r;
        } else if (std::strcmp(row->type, "bool") == 0) {
            v = v != 0.0 ? 1.0 : 0.0;
        }
        if (row->kind == RowKind::Run) {
            // Section 13.3: a whole number of base steps, at least one.
            const double ticks = std::floor(v / t.step + 0.5);
            if (ticks < 1.0 || std::fabs(ticks * t.step - v) > 1e-9 * v) {
                std::ostringstream o;
                o << "not a whole number of base steps; the step is " << t.step
                  << " s (section 13.3)";
                return fail(shown, o.str());
            }
        }
        a.value = v;
        return true;
    }

    std::string s_;
    std::string origin_;
    std::size_t i_ = 0;
    int line_ = 1;
    int at_line_ = 1;
    std::string err_;
    bool located_ = true;
    std::string base_;
};

)SERT"
    R"SERT(// ─── Writing a source (section 14.1) ────────────────────────────────────────

// The shortest decimal that reads back as the same double, as a source writes
// it: `340`, `0.08`, `1e-06`.
inline std::string source_number(double v) {
    char buf[40];
    for (int p = 15; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

// A string as a source writes it, with section 3.6's escapes, so that
// SourceReader reads back the same characters.
inline std::string source_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\0': out += "\\0"; break;
            default:   out += c;
        }
    }
    return out + "\"";
}

// Section 14.1's dump. `values[r]` is row r's current value for a Setting or
// Initial row and is unused for an Element row, whose bound elements arrive
// in `elements` instead: an array's uniform default has no path to write.
inline std::string dump_source(const char* title, const SettingRow* rows, std::size_t n,
                               const double* values, const std::vector<Assignment>& elements,
                               const std::string* texts = nullptr) {
    struct Line {
        std::string path, value;
        bool structural;
    };
    auto text = [&](const SettingRow& r, double v) {
        if (is_text_row(r)) return source_string(texts ? texts[&r - rows] : std::string());
        if (r.enumerators) {
            for (std::size_t k = 0; k < r.n_enumerators; ++k)
                if (static_cast<double>(r.enumerators[k].value) == v) return std::string(r.enumerators[k].name);
        }
        std::string s = source_number(v);
        if (std::strcmp(r.unit, "-") != 0 && !r.enumerators) s += std::string(" (") + r.unit + ")";
        return s;
    };
    std::vector<Line> settings, initial, run;
    for (std::size_t r = 0; r < n; ++r) {
        if (rows[r].kind == RowKind::Element) continue;
        Line l{rows[r].path, text(rows[r], values[r]), rows[r].structural};
        (rows[r].kind == RowKind::Setting   ? settings
         : rows[r].kind == RowKind::Initial ? initial
                                            : run).push_back(l);
    }
    for (const Assignment& e : elements)
        initial.push_back(Line{std::string(rows[e.row].path) + "[" + std::to_string(e.index) + "]",
                               text(rows[e.row], e.value), false});
    // The model's values share one column; the run's own line keeps to itself.
    std::size_t w = 0;
    for (const Line& l : settings) w = l.path.size() > w ? l.path.size() : w;
    for (const Line& l : initial) w = l.path.size() > w ? l.path.size() : w;

    std::string out = std::string("// ") + title +
                      ": how the run is configured (section 14.1): every setting and initial\n"
                      "// condition, then the run's length, the recording, the log and the inputs.\n";
    auto section = [&](const char* head, const std::vector<Line>& ls, std::size_t width) {
        if (ls.empty()) return;
        out += std::string("\n// ") + head + "\n";
        for (const Line& l : ls) {
            const std::size_t pad = width > l.path.size() ? width - l.path.size() : 0;
            std::string row = l.path + std::string(pad, ' ') + " = " + l.value + ";";
            if (l.structural) row = "// " + row + "   structural: fixed when the model was generated";
            out += row + "\n";
        }
    };
    section("settings", settings, w);
    section("initial conditions", initial, w);
    section("the run (section 13.3)", run, 0);
    return out;
}

// The run's own configuration, as the sections that reproduce it: the record
// section clears first, so loading it on top of anything gives exactly this
// selection, in this column order.
inline std::string dump_run(const RunConfig& c) {
    std::string out = "\n// the recording (section 13.5)\nrecord {\n    clear;\n";
    out += "    file  = " + source_string(c.record_file) + ";\n";
    out += "    every = " + std::to_string(c.every) + ";\n";
    for (const Column& k : c.columns) out += "    " + k.written + ";\n";
    out += "}\n\n// the log (section 13.7)\nlog {\n";
    out += "    file  = " + source_string(c.log_file) + ";\n";
    out += std::string("    level = ") + level_name(c.level) + ";\n";
    if (!c.levels.empty()) {
        std::size_t w = 0;
        for (const auto& p : c.levels) w = p.first.size() > w ? p.first.size() : w;
        out += "    levels {\n";
        for (const auto& p : c.levels)
            out += "        " + p.first + std::string(w - p.first.size(), ' ') + " = " +
                   level_name(p.second) + ";\n";
        out += "    }\n";
    }
    out += "}\n\n// the inputs (section 13.8)\ninputs {\n";
    out += "    file = " + source_string(c.inputs_file) + ";\n";
    return out + "}\n";
}

// Section 14.1's menu: every channel, in tree order, the selected ones as
// lines and the rest commented out, each with its header bracket. `extents`
// gives an array row's element count.
inline std::string list_source(const char* title, const ChannelRow* rows, std::size_t n,
                               const std::size_t* extents, const RunConfig& c) {
    struct Line {
        std::string path, label;
        bool on;
    };
    std::vector<Line> lines;
    for (std::size_t r = 0; r < n; ++r) {
        const std::size_t count = rows[r].array ? extents[r] : 1;
        for (std::size_t i = 0; i < count; ++i) {
            bool on = false;
            for (const Column& k : c.columns) on = on || (k.row == r && k.index == i);
            std::string p = rows[r].path;
            if (rows[r].array) p += "[" + std::to_string(i) + "]";
            lines.push_back(Line{(on ? "" : "// ") + p + ";", rows[r].label, on});
        }
    }
    std::size_t w = 0;
    for (const Line& l : lines) w = l.path.size() > w ? l.path.size() : w;
    std::string out = std::string("// ") + title +
                      ": every channel this model can record (section 13.5). The selected\n"
                      "// ones are lines and the rest are commented out: uncomment or delete,\n"
                      "// then load this back as a settings source.\n";
    out += "record {\n    clear;\n";
    for (const Line& l : lines)
        out += "    " + l.path + std::string(w - l.path.size(), ' ') + "   // [" + l.label + "]\n";
    return out + "}\n";
}

)SERT"
    R"SERT(// ─── Inputs from a file (section 13.8) ──────────────────────────────────────
//
// A CSV table that drives root inputs. It is read whole when the run starts,
// its columns matched to the inputs by name and converted to their canonical
// units, and then sampled once a tick: a double is interpolated between rows,
// and an int, a bool or a finite input holds the last row at or before the
// tick. The inputs are SettingRows, which already carry what a column is
// checked against.

class InputTable {
public:
    // Reads `path` for the inputs in `rows`. False, with `why` naming the file
    // and where in it, when the table cannot drive them.
    bool load(const std::string& path, const SettingRow* rows, std::size_t n, double step,
              std::string& why) {
        *this = InputTable();
        file_ = path;
        step_ = step;
        tol_ = step * 1e-3;   // a row meets a tick within a thousandth of a step
        std::string text;
        if (!read_all(path, text)) return fail(why, 0, "", "cannot read it");
        std::size_t at = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
        int next_line = 1;
        std::vector<std::string> head, cells;
        std::vector<char> quoted;
        std::vector<int> drives;   // per field: the input column it fills, or -1
        bool have_head = false;
        Unit second, time_unit;
        UnitParser("s").parse(second);
        while (at < text.size()) {
            const int line = next_line;
            std::string bad;
            if (!record(text, at, next_line, cells, quoted, bad)) return fail(why, line, "", bad);
            if (cells.size() == 1 && cells[0].empty() && !quoted[0]) continue;   // a blank line
            if (!have_head) {
                have_head = true;
                head = cells;
                if (!header(head, rows, n, second, time_unit, drives, why, line)) return false;
                continue;
            }
            if (cells.size() != head.size())
                return fail(why, line, "", std::to_string(cells.size()) + " fields where the header has " +
                                               std::to_string(head.size()));
            double t = 0.0;
            if (!number(cells[0], t)) return fail(why, line, head[0], "`" + cells[0] + "` is not a number");
            t = unit_convert(t, time_unit, second);
            if (!times_.empty() && t < times_.back()) {
                std::ostringstream o;
                o << "the time goes backwards, from " << times_.back() << " s to " << t << " s";
                return fail(why, line, "", o.str());
            }
            times_.push_back(t);
            for (std::size_t f = 1; f < cells.size(); ++f) {
                if (drives[f] < 0) continue;
                const Col& c = cols_[static_cast<std::size_t>(drives[f])];
                double v = 0.0;
                if (c.text) {
                    // Section 13.8: the cell's text, as long as it fits. A
                    // quoted empty cell is the empty string; a bare one is
                    // nothing at all.
                    if (cells[f].empty() && !quoted[f]) return fail(why, line, head[f], "the cell is empty");
                    if (cells[f].size() > rows[c.row].capacity)
                        return fail(why, line, head[f],
                                    std::to_string(cells[f].size()) + " bytes, and `" + rows[c.row].path +
                                        "` holds " + std::to_string(rows[c.row].capacity) + " (section 5.1b)");
                    texts_.push_back(cells[f]);
                } else {
                    const std::string why_not = value(cells[f], c, rows, v);
                    if (!why_not.empty()) return fail(why, line, head[f], why_not);
                }
                values_.push_back(v);
            }
        }
        if (!have_head) return fail(why, 0, "", "it is empty; the first line is the header, `time (s), ...`");
        if (times_.empty()) return fail(why, 0, "", "it has a header and no rows");
        if (times_.front() > tol_) {
            std::ostringstream o;
            o << "it starts at " << times_.front() << " s, and the run starts at 0 (section 13.8)";
            return fail(why, 0, "", o.str());
        }
        last_tick_ = static_cast<std::uint64_t>(std::floor((times_.back() + tol_) / step_));
        active_ = true;
        return true;
    }

    bool active() const { return active_; }
    const std::string& file() const { return file_; }
    std::size_t columns() const { return cols_.size(); }
    std::size_t column_row(std::size_t c) const { return cols_[c].row; }
    const std::vector<std::string>& ignored() const { return ignored_; }
    std::uint64_t last_tick() const { return last_tick_; }   // at or before the last row

    // Section 5.1b: a string column's text at the tick `at` last sampled. A
    // string holds the last row at or before it, as a finite input does.
    const std::string& text(std::size_t c) const {
        return texts_[row_ * n_text_ + cols_[c].tcol];
    }

    // Each column's value at tick k, canonical unit. Ticks only go forward,
    // so the search starts where the last one ended. At a step, two rows at
    // one time, the later row is the one at or before the tick.
    void at(std::uint64_t k, double* out) {
        const double t = static_cast<double>(k) * step_;
        const std::size_t rows = times_.size(), nc = cols_.size();
        while (row_ + 1 < rows && times_[row_ + 1] <= t + tol_) ++row_;
        const double t0 = times_[row_];
        const bool on_row = std::fabs(t0 - t) <= tol_ || row_ + 1 == rows;
        for (std::size_t c = 0; c < nc; ++c) {
            const double v0 = values_[row_ * nc + c];
            if (cols_[c].hold || on_row) {
                out[c] = v0;
                continue;
            }
            const double t1 = times_[row_ + 1], v1 = values_[(row_ + 1) * nc + c];
            out[c] = v0 + (v1 - v0) * ((t - t0) / (t1 - t0));
        }
    }

private:
    struct Col {
        std::size_t row;     // the input it drives
        bool hold;           // int, bool, finite, string: no interpolation
        Unit from, to;       // the header's unit and the input's
        bool convert;
        bool text = false;   // a string input (section 5.1b)
        std::size_t tcol = 0;   // its place among the string columns
    };

    bool fail(std::string& why, int line, const std::string& column, const std::string& m) {
        why = file_ + (line ? ":" + std::to_string(line) : std::string()) + ": " +
              (column.empty() ? "" : "column `" + column + "`: ") + m;
        return false;
    }

    static bool read_all(const std::string& path, std::string& out) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char buf[65536];
        for (std::size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, got);
        std::fclose(f);
        return true;
    }

    static std::string trim(const std::string& s) {
        const std::size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos) return "";
        std::string t = s.substr(a, s.find_last_not_of(" \t") - a + 1);
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
        return t;
    }

    // One record, RFC 4180, from `at`: comma-separated fields, each either
    // bare, with the spaces around it dropped, or in double quotes, where a
    // comma or a line break is just a character and `""` is one quote. The
    // record ends at a line break outside quotes. `quoted[f]` says whether
    // field f was quoted, which is what tells "" from nothing.
    static bool record(const std::string& s, std::size_t& at, int& line,
                       std::vector<std::string>& out, std::vector<char>& quoted, std::string& bad) {
        out.clear();
        quoted.clear();
        for (;;) {
            while (at < s.size() && (s[at] == ' ' || s[at] == '\t')) ++at;
            std::string f;
            bool q = false;
            if (at < s.size() && s[at] == '"') {
                q = true;
                for (++at;; ++at) {
                    if (at >= s.size()) {
                        bad = "a quoted field is never closed";
                        return false;
                    }
                    if (s[at] == '"') {
                        if (at + 1 < s.size() && s[at + 1] == '"') {
                            f += '"';
                            ++at;
                            continue;
                        }
                        ++at;
                        break;
                    }
                    // A line break inside quotes is one character, LF: the
                    // recorder writes in text mode, so on Windows it arrives
                    // as CRLF, and a replay must give back what was recorded.
                    if (s[at] == '\r' && at + 1 < s.size() && s[at + 1] == '\n') continue;
                    if (s[at] == '\n') ++line;
                    f += s[at];
                }
                while (at < s.size() && (s[at] == ' ' || s[at] == '\t')) ++at;
                if (at < s.size() && s[at] != ',' && s[at] != '\n' && s[at] != '\r') {
                    bad = "text after a quoted field's closing quote";
                    return false;
                }
            } else {
                while (at < s.size() && s[at] != ',' && s[at] != '\n') f += s[at++];
                if (!f.empty() && f.back() == '\r') f.pop_back();
                const std::size_t e = f.find_last_not_of(" \t");
                f.erase(e == std::string::npos ? 0 : e + 1);
            }
            out.push_back(f);
            quoted.push_back(q ? 1 : 0);
            if (at < s.size() && s[at] == '\r') ++at;
            if (at >= s.size()) return true;
            if (s[at] == '\n') {
                ++at;
                ++line;
                return true;
            }
            ++at;   // the comma
        }
    }

    // The whole cell, as a finite number.
    static bool number(const std::string& s, double& out) {
        if (s.empty()) return false;
        char* end = nullptr;
        out = std::strtod(s.c_str(), &end);
        return end && *end == '\0' && std::isfinite(out);
    }

    // `name [unit]` or `name (unit)`: the name, and the bracket's text.
    static void name_unit(const std::string& h, std::string& name, std::string& unit, bool& has) {
        name = h;
        unit.clear();
        has = false;
        if (h.empty() || (h.back() != ']' && h.back() != ')')) return;
        const char open = h.back() == ']' ? '[' : '(';
        int depth = 0;
        for (std::size_t k = h.size(); k-- > 0;) {
            if (h[k] == h.back()) ++depth;
            if (h[k] == open && --depth == 0) {
                name = trim(h.substr(0, k));
                unit = trim(h.substr(k + 1, h.size() - k - 2));
                has = true;
                return;
            }
        }
    }

)SERT"
    // Inside InputTable: the split only has to keep each piece under the cap.
    R"SERT(    bool header(const std::vector<std::string>& head, const SettingRow* rows, std::size_t n,
                const Unit& second, Unit& time_unit, std::vector<int>& drives, std::string& why,
                int line) {
        std::string name, unit;
        bool has = false;
        name_unit(head[0], name, unit, has);
        UnitParser tp(unit);
        if (name != "time" || !has || !tp.parse(time_unit) || !unit_compatible(time_unit, second))
            return fail(why, line, head[0], "the first column is the time, with its unit, as `time (s)`");
        drives.assign(head.size(), -1);
        for (std::size_t f = 1; f < head.size(); ++f) {
            name_unit(head[f], name, unit, has);
            std::size_t r = 0;
            while (r < n && name != rows[r].path) ++r;
            if (r == n) {
                ignored_.push_back(name);
                continue;
            }
            for (const Col& c : cols_)
                if (c.row == r) return fail(why, line, head[f], "a second column for `" + name + "`");
            const SettingRow& row = rows[r];
            Col c{r, row.enumerators || std::strcmp(row.type, "int") == 0 ||
                         std::strcmp(row.type, "bool") == 0 || is_text_row(row),
                  Unit{}, Unit{}, false};
            if (is_text_row(row)) {
                if (has && !unit.empty() && unit != "string")
                    return fail(why, line, head[f], "a string input's bracket is [string], or nothing");
                c.text = true;
                c.tcol = n_text_++;
                drives[f] = static_cast<int>(cols_.size());
                cols_.push_back(c);
                continue;
            }
            for (int k = 0; k < 8; ++k) c.to.dim[k] = row.dim[k];
            c.to.scale = row.scale;
            c.to.offset = row.offset;
            const bool dimensioned = std::strcmp(row.unit, "-") != 0;
            if (row.enumerators) {
                if (has && !unit.empty() && unit != row.type)
                    return fail(why, line, head[f], std::string("a finite input's bracket is its type, [") +
                                                        row.type + "], or nothing");
            } else if (!has || unit.empty() || unit == "-") {
                if (dimensioned)
                    return fail(why, line, head[f], std::string("`") + name + "` needs a unit, as [" +
                                                        row.unit + "] (section 13.8)");
            } else {
                UnitParser p(unit);
                if (!p.parse(c.from)) return fail(why, line, head[f], p.error());
                if (!unit_compatible(c.from, c.to))
                    return fail(why, line, head[f], "incompatible units: [" + unit + "] where [" +
                                                        row.unit + "] is declared");
                c.convert = true;
            }
            drives[f] = static_cast<int>(cols_.size());
            cols_.push_back(c);
        }
        return true;
    }

    // One cell for column `c`, checked and converted; "" or why not.
    std::string value(const std::string& cell, const Col& c, const SettingRow* rows, double& v) {
        const SettingRow& row = rows[c.row];
        if (cell.empty()) return "the cell is empty";
        if (row.enumerators) {
            const bool by_name = (cell[0] >= 'a' && cell[0] <= 'z') || (cell[0] >= 'A' && cell[0] <= 'Z') ||
                                 cell[0] == '_';
            double num = 0.0;
            if (!by_name && !number(cell, num)) return "`" + cell + "` is not a number";
            for (std::size_t k = 0; k < row.n_enumerators; ++k)
                if (by_name ? cell == row.enumerators[k].name
                            : num == static_cast<double>(row.enumerators[k].value)) {
                    v = static_cast<double>(row.enumerators[k].value);
                    return "";
                }
            return "`" + cell + "` is not an enumerator of " + row.type;
        }
        if (!number(cell, v)) return "`" + cell + "` is not a number";
        if (c.convert) v = unit_convert(v, c.from, c.to);
        if (std::strcmp(row.type, "int") == 0 && std::fabs(v - std::floor(v + 0.5)) > 1e-9)
            return "`" + cell + "` is not a whole number, and `" + row.path + "` is an int";
        if (std::strcmp(row.type, "int") == 0) v = std::floor(v + 0.5);
        if (std::strcmp(row.type, "bool") == 0 && v != 0.0 && v != 1.0)
            return "`" + cell + "` is not 0 or 1, and `" + row.path + "` is a bool";
        return "";
    }

    std::string file_;
    double step_ = 0.0, tol_ = 0.0;
    std::vector<Col> cols_;
    std::vector<std::string> ignored_;
    std::vector<double> times_;    // s
    std::vector<double> values_;   // row-major, one per column, canonical units
    std::vector<std::string> texts_;   // row-major, one per string column (section 5.1b)
    std::size_t n_text_ = 0;
    std::size_t row_ = 0;          // the last row at or before the last tick sampled
    std::uint64_t last_tick_ = 0;
    bool active_ = false;
};

}  // inline namespace @VER@
}  // namespace se_rt

#endif  // SE_RUNTIME_@VER@
)SERT";

const char kStringType[] = R"SESTR(// ─── Strings (section 5.1b) ───────────────────────────────────────────────────
//
// Bounded text: up to N bytes plus a size, never allocating. A setting, a
// leaf's string OUTPUT and a string VAR are storage, `string<N>`; every
// string INPUT reads through a `std::string_view` onto some producer's
// storage, which is why a reader accepts any capacity.
//
// The public header carries this too, because a root setting, input or
// output can be a string, so it is guarded and lives outside the runtime's
// versioned namespace: two models in one program share one definition.
#ifndef SE_RT_STRING_DEFINED
#define SE_RT_STRING_DEFINED

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <string_view>

namespace se_rt {

// The length of the longest prefix of `s` that is at most `n` bytes and ends
// on a whole UTF-8 character, so a cut never leaves half of one behind.
inline std::size_t utf8_cut(std::string_view s, std::size_t n) {
    if (s.size() <= n) return s.size();
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) --n;
    return n;
}

template <std::size_t N>
struct string {
    static_assert(N > 0 && N < 0x7fffffff, "a string capacity is a positive byte count");
    std::int32_t size_;      // first, as in buffer<N>
    char data_[N + 1];       // always NUL-terminated, for C APIs
    std::int32_t wanted_;    // the length last asked for; over N after a cut (section 5.1b)

    string() = default;      // `string<N> s{};` zeroes it: the empty string
    string(std::string_view s) : size_(0), data_{}, wanted_(0) { assign(s); }
    string(const char* s) : string(std::string_view(s ? s : "")) {}
    string(const string& o) : size_(0), data_{}, wanted_(0) { *this = o; }

    const char* c_str() const { return data_; }
    const char* data() const { return data_; }
    std::size_t size() const { return static_cast<std::size_t>(size_); }
    static constexpr std::size_t capacity() { return N; }
    bool empty() const { return size_ == 0; }
    operator std::string_view() const { return std::string_view(data_, size()); }

    // Keeps the longest prefix that fits and ends on a whole character, and
    // says whether that was all of it. A cut is remembered in `wanted_` for
    // the engine's one warning per destination.
    bool assign(std::string_view s) {
        const std::size_t k = utf8_cut(s, N);
        if (k) std::memmove(data_, s.data(), k);   // `s` may view this very storage
        data_[k] = '\0';
        size_ = static_cast<std::int32_t>(k);
        if (k != s.size())
            wanted_ = s.size() > 0x7fffffffu ? 0x7fffffff : static_cast<std::int32_t>(s.size());
        return k == s.size();
    }
    void clear() { size_ = 0; data_[0] = '\0'; }

    string& operator=(std::string_view s) { assign(s); return *this; }
    string& operator=(const char* s) { assign(std::string_view(s ? s : "")); return *this; }
    template <std::size_t M>
    string& operator=(const string<M>& o) { assign(std::string_view(o)); return *this; }
    // Copies the bytes that are THERE, not the capacity.
    string& operator=(const string& o) {
        if (this != &o) {
            size_ = o.size_;
            std::memcpy(data_, o.data_, static_cast<std::size_t>(size_) + 1);
        }
        return *this;
    }
};

template <std::size_t N>
bool operator==(const string<N>& a, std::string_view b) { return std::string_view(a) == b; }
template <std::size_t N>
bool operator==(std::string_view a, const string<N>& b) { return a == std::string_view(b); }
template <std::size_t N, std::size_t M>
bool operator==(const string<N>& a, const string<M>& b) {
    return std::string_view(a) == std::string_view(b);
}
template <std::size_t N>
bool operator!=(const string<N>& a, std::string_view b) { return !(a == b); }
template <std::size_t N>
bool operator!=(std::string_view a, const string<N>& b) { return !(a == b); }
template <std::size_t N, std::size_t M>
bool operator!=(const string<N>& a, const string<M>& b) { return !(a == b); }

// So `log.info("sending to {}", param.host)` formats it as text. A template,
// so the stream need only be complete where it is used.
template <class C, class T, std::size_t N>
std::basic_ostream<C, T>& operator<<(std::basic_ostream<C, T>& o, const string<N>& s) {
    o.write(s.data(), static_cast<std::streamsize>(s.size()));
    return o;
}

}  // namespace se_rt

#endif  // SE_RT_STRING_DEFINED
)SESTR";

}  // namespace

// §5.1b — the bounded string type, for the public header as well as the
// runtime. Guarded, so whichever comes first defines it.
const char* se_string_type() { return kStringType; }

// The runtime text with its version tag filled in: FNV-1a over the text as
// written, so any edit to it — a comment included — moves the tag. Computed
// once; the text never changes while `sec` runs.
const std::string& se_runtime_hpp() {
    static const std::string text = [] {
        std::string t = kRuntime;
        // §14.1 — the unit table goes in BEFORE the hash, so a change to
        // Appendix A moves the tag exactly as an edit to the text would.
        const std::string units = "@UNIT_TABLE@";
        const std::size_t u = t.find(units);
        if (u != std::string::npos) t.replace(u, units.size(), unit_table_cpp());
        const std::string strings = "@STRING_TYPE@";
        const std::size_t st = t.find(strings);
        if (st != std::string::npos) t.replace(st, strings.size(), kStringType);
        std::uint32_t h = 2166136261u;
        for (const unsigned char c : t) {
            h ^= c;
            h *= 16777619u;
        }
        char tag[16];
        std::snprintf(tag, sizeof tag, "r%08x", static_cast<unsigned>(h));
        const std::string key = "@VER@";
        for (std::size_t at = t.find(key); at != std::string::npos; at = t.find(key, at))
            t.replace(at, key.size(), tag);
        return t;
    }();
    return text;
}

}  // namespace se
