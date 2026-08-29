// ─────────────────────────────────────────────────────────────────────────────
//  The fixed engine runtime headers, embedded so that `sec` is self-contained.
//
//  These are NEVER generated: they do not vary with the model, the same text
//  serves a 3-state model and a 30,000-state one. They are written out beside
//  the generated code so the emitted directory compiles on its own, with no
//  include path outside itself and no third-party package (§15.5).
//
//  Editing them here changes every future emission. They are ordinary C++,
//  parked in raw string literals only because a compiler that has to find its
//  own data files at run time is a compiler that breaks when it is moved.
// ─────────────────────────────────────────────────────────────────────────────
#include "emit.hpp"

namespace se {

const char* state_ref_hpp() {
    return R"SERT(// ─────────────────────────────────────────────────────────────────────────────
//  state_ref.hpp — FIXED ENGINE RUNTIME HEADER
//
//  Hand-written, ships with the engine, NEVER generated. It does not vary with
//  the model: the same header serves a 3-state model and a 30,000-state one.
//  The generator's only job is to emit `state_ref omega;` members and bind them
//  to their slots in Sim::bind().
//
//  Purpose: a named view of one slot in a model-wide block, so a node body can
//  write `state.omega` instead of `sim.x[7]`, while an external solver (CVODE,
//  odeint, a hand-rolled RK4) still sees nothing but `double*`.
//
//  ── THE UNIT CONTRACT ────────────────────────────────────────────────────────
//  Units are checked by the DSL compiler at every declarative site and then
//  ERASED. They are never C++ types. What survives into C++ is a guarantee:
//
//      the slot holds the value IN THE STATE'S DECLARED UNIT.
//
//  `omega (rad/s)` => x[i] is rad/s, `state.omega` reads rad/s, and a write to
//  `state.omega` had better be rad/s — the compiler will not check that for
//  you. This is a contract with the node implementer and with whoever writes
//  the external solver, and it is why the generator emits a unit manifest
//  (Sim.units.txt) alongside the model code.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>

namespace sim {

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

}  // namespace sim
)SERT";
}

const char* se_runtime_hpp() {
    return R"SERT(// ─────────────────────────────────────────────────────────────────────────────
//  se_runtime.hpp — FIXED ENGINE RUNTIME HEADER
//
//  Hand-written, ships with the engine, NEVER generated. It holds the four
//  things the generated scheduler needs that are not model-shaped:
//
//      log.*          SPECIFICATION.md section 10.1 — qualitative, observational
//      sim.stop/abort section 10.2 — the only way a run ends early
//      the recorder   section 13.5 — quantitative, an aligned numeric table
//      the frame      section 9.4  — free-run the batch, then spin
//
//  Nothing here knows anything about any particular model. The generator emits
//  `se_rt::Log log{...}` into the preamble of the methods where logging is
//  legal and simply does not emit it elsewhere, which is how confinement is
//  enforced: by name withholding, not by an analyser (section 15.4).
//
//  C++17, no third-party packages.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace se_rt {

// The tick time a log record is stamped with. A global because a LogSink is a
// global: every record carries a time and no caller passes one.
//
// This is NOT what a body reads. `sim.time()` comes from the model's own
// RunState (below), which advances at every minor step and belongs to one Sim
// rather than to the process. The two agree at tick boundaries and are
// deliberately different objects.
inline double& sim_time() {
    static double t = 0.0;
    return t;
}

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

template <class... A>
std::string format(const char* f, const A&... a) {
    std::ostringstream o;
    format_into(o, f, a...);
    return o.str();
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

class LogSink {
public:
    ~LogSink() {
        if (file_) std::fclose(file_);
    }

    void configure(const char* path, Level global) {
        global_ = global;
        if (path && *path) file_ = std::fopen(path, "w");
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
        if (to_console) {
            std::fputs(head, stderr);
            std::fputs(msg.c_str(), stderr);
            std::fputc('\n', stderr);
        }
    }

    // section 10.1 — trace in the pure methods is suppressed unless this is set,
    // so the four-evaluations-per-step hazard is visible rather than hidden.
    bool trace_enabled = false;

private:
    std::FILE* file_ = nullptr;
    Level global_ = Level::Info;
    std::vector<std::pair<std::string, Level>> paths_;
};

inline LogSink& logs() {
    static LogSink s;
    return s;
}

// The `log` object the generator puts in scope of init(), on_step() and final().
//
// It holds the node path and NOTHING else — the sim time is read at call time,
// from sim_time(). That is what lets the restricted form below be a class
// member rather than a line of preamble in every method.
struct Log {
    const char* path;

    template <class... A> void trace(const char* f, const A&... a) const {
        logs().write(Level::Trace, path, sim_time(), format(f, a...));
    }
    template <class... A> void debug(const char* f, const A&... a) const {
        logs().write(Level::Debug, path, sim_time(), format(f, a...));
    }
    template <class... A> void info(const char* f, const A&... a) const {
        logs().write(Level::Info, path, sim_time(), format(f, a...));
    }
    template <class... A> void warn(const char* f, const A&... a) const {
        logs().write(Level::Warn, path, sim_time(), format(f, a...));
    }
    template <class... A> void error(const char* f, const A&... a) const {
        logs().write(Level::Error, path, sim_time(), format(f, a...));
    }
};

// The restricted form for the pure methods: `log.trace` alone (section
// 10.1). `log.info` there is "no such member".
//
// Every leaf holds one of these as a member called `log`, so a pure method
// needs no preamble at all. The mutating methods declare a local `Log log`
// which shadows it, widening what is legal exactly where section 8.4 says it is.
struct TraceLog {
    const char* path;

    template <class... A> void trace(const char* f, const A&... a) const {
        if (!logs().trace_enabled) return;
        logs().write(Level::Trace, path, sim_time(), format(f, a...) + "  [minor step]");
    }
};

)SERT"
    // MSVC caps a single string literal at 16380 bytes, and this header passed
    // it. Adjacent literals concatenate, so the split is invisible in the
    // output; it must fall on a blank line between sections, and each half must
    // stay under the cap as the header grows.
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
    const char* type;     // "double", "int", "bool", "float"
    const char* unit;     // the declared unit; "-" is dimensionless
};

// ─── Configuration (section 6.2b, section 9.2) ───────────────────────────────
//
// A node's decimation is derived from its `rate` setting rather than baked at
// elaboration. `rate` is an ordinary runtime setting, so an override has to
// move the SCHEDULE as well as the coefficients; a decimation frozen at code
// generation would leave the node running at its compiled rate while every
// value derived from `sample_rate` followed the override, which is worse than
// refusing the override outright.
//
// This is SE0450's runtime counterpart: section 9.2's divisor rule, applied
// where the value now arrives. Returns false and leaves `out` at 1 on a rate
// that is not a positive integer divisor of the base rate. The caller knows
// which path it was configuring; this function does not (section 16.4).
inline bool decimation_of(double base_rate, double rate, std::uint64_t& out) {
    out = 1;
    if (!(rate > 0.0) || !(base_rate > 0.0)) return false;
    const double ratio = base_rate / rate;
    const double n = std::floor(ratio + 0.5);
    // Tolerance, not equality: `rate` reaches here through a decimal literal
    // and possibly a unit conversion, so the ratio need not be exact even when
    // the intent was. One part in 1e9 is far tighter than any adjacent legal
    // rate, which differ by whole decimation counts.
    if (n < 1.0 || std::fabs(ratio - n) > 1e-9 * ratio) return false;
    out = static_cast<std::uint64_t>(n);
    return true;
}

// The message SE0450 would have produced, for a rate that arrived after the
// model was compiled. Same wording and the same nearest-legal-rates note, so a
// rate rejected by the compiler and one rejected by the loader read alike.
inline std::string rate_error(const char* path, double rate, double base_rate) {
    std::ostringstream o;
    o << path << " = " << rate << " Hz is not an integer divisor of the "
      << base_rate << " Hz base rate";
    if (rate > 0.0 && base_rate > 0.0) {
        const double ratio = base_rate / rate;
        const double lo = std::floor(ratio) < 1.0 ? 1.0 : std::floor(ratio);
        const double hi = std::ceil(ratio) < 1.0 ? 1.0 : std::ceil(ratio);
        o << "; the nearest legal rates are " << base_rate / lo << " Hz and "
          << base_rate / hi << " Hz";
    }
    return o.str();
}

// ─── Array extents (section 6.4a, section 15.5a) ─────────────────────────────
//
// An extent is a section-7 expression over ordinary overridable settings, so it
// is evaluated at configuration and only then does it have to be an exact
// non-negative integer. This is the same shape of check as decimation_of(): the
// DSL compiler applied the rule to what it could see, and this applies it to
// what actually arrived. Zero is legal and yields no slots.
inline bool extent_of(double v, std::size_t& out) {
    out = 0;
    if (!(v >= 0.0)) return false;
    const double n = std::floor(v + 0.5);
    // A tolerance rather than equality, for the same reason decimation_of()
    // uses one: the value reaches here through decimal literals and possibly a
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

inline Control& control() {
    static Control c;
    return c;
}

// ─── The run view (section 10.5) ─────────────────────────────────────────────
//
// The engine's per-evaluation state, one instance per model. `now` is the
// instant of the CURRENT evaluation, not of the current tick: derivatives()
// rewrites it at every minor step, which is the whole reason a continuous
// source can be written at all.
struct RunState {
    double        now  = 0.0;   // the instant this evaluation is at
    double        step = 0.0;   // the global base step
    std::uint64_t tick = 0;     // base tick index
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
    const RunState* run;
    const char*     node_path;
    const char*     node_type;

    double        time() const { return run->now; }
    double        step() const { return run->step; }
    std::uint64_t tick() const { return run->tick; }
    const char*   path() const { return node_path; }
    const char*   type() const { return node_type; }
};

// The widened form for init(), on_step() and final(): everything above plus
// the two calls that end a run (section 10.2). `sim.stop` in output() is "no
// member named stop" — a compile error at the .se line, not a runtime rule.
struct RunControl : RunView {
    RunControl(const RunState* r, const char* p, const char* t) : RunView{r, p, t} {}

    void stop(const std::string& m) const { control().stop(m); }
    void abort(const std::string& m) const { control().abort(m); }
};

// ─── Recording (section 13.5) ────────────────────────────────────────────────

class Recorder {
public:
    ~Recorder() { close(); }

    bool open(const std::string& path) {
        file_ = std::fopen(path.c_str(), "w");
        return file_ != nullptr;
    }

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

    void close() {
        if (!file_) return;
        std::fclose(file_);
        file_ = nullptr;
    }

    std::uint64_t rows() const { return rows_; }

private:
    std::FILE* file_ = nullptr;
    std::uint64_t rows_ = 0;
};

// ─── The real-time frame (section 9.4, section 13.6) ─────────────────────────
//
// Two-clock: free-run every base step in the frame as fast as it can, then
// busy-wait on the high-resolution timer until the frame boundary. An overrun
// starts the next batch immediately with no spin, so SIM TIME DRIFTS BEHIND
// WALL TIME. Every base step always runs — dropping one would punch a hole in
// a fixed-step integration, and drift is the honest failure mode.
//
// Overrun is INSTRUMENTED, NOT POLICED: there is no abort escalation here.

class Realtime {
public:
    using clock = std::chrono::steady_clock;

    void configure(double frame_seconds, double window_seconds) {
        frame_ = frame_seconds;
        enabled_ = frame_seconds > 0.0;
        std::size_t n = 1;
        if (enabled_ && window_seconds > 0.0)
            n = static_cast<std::size_t>(window_seconds / frame_seconds + 0.5);
        window_frames_ = n < 1 ? 1 : n;
        deadline_ = clock::now();
    }

    void start() { deadline_ = clock::now(); }

    void frame_boundary() {
        if (!enabled_) return;
        const clock::time_point target =
            deadline_ + std::chrono::duration_cast<clock::duration>(
                            std::chrono::duration<double>(frame_));
        const clock::time_point done = clock::now();
        const double compute = std::chrono::duration<double>(done - deadline_).count();
        const bool overran = done > target;

        if (!overran) {
            while (clock::now() < target) {
            }
            deadline_ = target;
        } else {
            deadline_ = done;   // no spin; sim time falls behind wall time
        }

        window_.push_back({compute / frame_ * 100.0, overran});
        if (window_.size() > window_frames_) window_.pop_front();
        ++frames_;
        if (overran) ++overruns_;
    }

    double utilization() const {
        if (window_.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& f : window_) sum += f.first;
        return sum / static_cast<double>(window_.size());
    }

    double overrun_pct() const {
        if (window_.empty()) return 0.0;
        std::size_t n = 0;
        for (const auto& f : window_)
            if (f.second) ++n;
        return 100.0 * static_cast<double>(n) / static_cast<double>(window_.size());
    }

    // The whole-run cumulative summary, free off the same counters.
    void summary() const {
        if (!enabled_ || frames_ == 0) return;
        std::fprintf(stderr,
                     "realtime: %llu frames, %llu overran (%.2f%% of the run)\n",
                     static_cast<unsigned long long>(frames_),
                     static_cast<unsigned long long>(overruns_),
                     100.0 * static_cast<double>(overruns_) /
                         static_cast<double>(frames_));
    }

private:
    bool enabled_ = false;
    double frame_ = 0.0;
    std::size_t window_frames_ = 1;
    std::deque<std::pair<double, bool>> window_;
    std::uint64_t frames_ = 0;
    std::uint64_t overruns_ = 0;
    clock::time_point deadline_;
};

inline Realtime& realtime() {
    static Realtime r;
    return r;
}

}  // namespace se_rt
)SERT";
}

}  // namespace se
