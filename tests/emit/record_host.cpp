// ─────────────────────────────────────────────────────────────────────────────
//  The run's own configuration in a compiled model (SPECIFICATION.md §13.3,
//  §13.5, §13.7, §14.1), against the generated drivetrain.
//
//  What a host can rely on when it chooses, at run time, what a run records
//  and where it logs:
//
//    1. Every path the .sim could record is a channel: composite ports, leaf
//       inputs, outputs, states and vars, and the engine's metrics, in tree
//       order, with the .sim's own selection marked.
//    2. The channel calls are the `record` section's lines: record() adds and
//       keeps a channel's place, unrecord() removes one by either spelling,
//       record_clear() empties the selection, and the file and the rate move.
//       A refused call changes nothing and does not stop the run.
//    3. Sections layer exactly as the calls do, and a refused one is all or
//       nothing.
//    4. dump_settings() carries the recording and the log, and reads back to
//       the same text; list_signals() reads back to the same selection.
//    5. Two copies of one model, each given its own files, write two tables
//       and two logs: the collision phase 2 left open is closed.
//    6. A channel handle reads, live, what the last tick sampled: the same
//       value outputs() holds and the CSV row records, selected or not.
//    7. A captured log keeps the console's lines for the host, and a full
//       queue says how many it dropped.
//    8. `{time}` in a file name: a fresh file each run, `-2` when the name is
//       taken, the pattern kept in the dump, the name used reported.
//
//  Like settings_host.cpp it #includes Drivetrain.cpp, so it can look at the
//  configuration a call lands in.
//
//  Built and run by tests/run.ps1. Exit status 0 means every check passed.
// ─────────────────────────────────────────────────────────────────────────────
#include "Drivetrain.cpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "record_host: FAIL %s\n", what);
    ++failures;
}

bool has(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

using Impl = drivetrain::Sim::Impl;
using Sim = drivetrain::Sim;

std::vector<std::string> lines_of(const char* path) {
    std::vector<std::string> out;
    std::ifstream f(path);
    for (std::string l; std::getline(f, l);) out.push_back(l);
    return out;
}

// A whole run on a Sim already configured, driving the boundary so the wheel
// turns.
int run(Sim& sim) {
    if (!sim.init()) return sim.finish();
    while (!sim.done()) {
        sim.inputs().axle_torque = 300.0;
        sim.inputs().ground_speed = 5.0;
        sim.tick();
    }
    return sim.finish();
}

// A source on a fresh model: the error, or "" if it was accepted.
std::string refusal_text(const char* text) {
    Impl m;
    return m.load_settings(text, "t") ? std::string() : std::string(m.error());
}

const Sim::ChannelInfo* find(const std::vector<Sim::ChannelInfo>& cs, const char* path) {
    for (const Sim::ChannelInfo& c : cs)
        if (c.path == path) return &c;
    return nullptr;
}

}  // namespace

int main() {
    // ── 1. every recordable path is a channel ────────────────────────────────
    {
        Sim sim;
        const std::vector<Sim::ChannelInfo> cs = sim.channels_list();
        const Sim::ChannelInfo* port = find(cs, "ws.slip");
        const Sim::ChannelInfo* input = find(cs, "whl.drive_torque");
        const Sim::ChannelInfo* state = find(cs, "whl.state.omega");
        const Sim::ChannelInfo* var = find(cs, "whl.var.inertia_inv");
        const Sim::ChannelInfo* metric = find(cs, "sim.utilization");
        check(port && !port->selected && port->unit == "-", "a composite port's field is a channel");
        check(input && input->unit == "N*m", "a leaf input is a channel");
        check(state && state->selected && state->unit == "rad/s",
              "a state is a channel, and the .sim selected it");
        check(var != nullptr, "a var is a channel");
        check(metric && metric->selected && metric->unit == "%", "an engine metric is a channel");
        check(!cs.empty() && cs.front().path == "axle_torque", "the root's ports come first");
        check(cs.back().path == "sim.overrun_pct", "the metrics come last");
        check(!find(cs, "whl.radius"), "a setting is not a channel");
        std::size_t on = 0;
        for (const Sim::ChannelInfo& c : cs) on += c.selected ? 1 : 0;
        check(on == 6, "the .sim's six channels are the selection");
    }

    // ── 2. the calls are the section's lines ─────────────────────────────────
    {
        Sim sim;
        sim.record_clear();
        check(sim.record("whl.omega"), "a bare spelling is accepted");
        check(sim.record("ground_speed"), "a root input is accepted");
        check(sim.record("whl.state.omega"), "the canonical spelling of a selected channel");
        check(sim.record("tc.out.torque_cut"), "an output is accepted");
        check(sim.unrecord("tc.torque_cut"), "unrecord takes the bare spelling too");
        check(sim.unrecord("tc.state.cut"), "unrecording an unselected channel is not an error");
        check(!sim.record("whl.nope"), "a path that names nothing is refused");
        check(has(sim.error(), "whl.nope: not a recordable signal"), "and error() says why");
        check(!sim.record("whl.omega[2]"), "an index on a scalar is refused");
        check(has(sim.error(), "not an array state"), "and error() says so");
        check(!sim.record_every(0), "every = 0 is refused");
        check(!sim.record_file(""), "an empty file name is refused");
        check(sim.record_file("rh_calls.csv") && sim.record_every(100), "the file and the rate");
        check(std::string(sim.record_file()) == "rh_calls.csv" && sim.record_every() == 100,
              "and read back");
        check(run(sim) == 0, "a refused call did not stop the run");
        check(!sim.record("whl.omega"), "a call after init() is refused");
        check(has(sim.error(), "record() comes before init()"), "and error() says configuration is over");
        const std::vector<std::string> csv = lines_of("rh_calls.csv");
        check(!csv.empty() && csv[0] == "time (s),whl.omega [rad/s],ground_speed [m/s]",
              "the header is the selection, in the order it was added, as written");
        check(csv.size() == 52, "one row every 100 ticks of 5000, and the header");
    }

    // ── 3. sections layer, all or nothing ────────────────────────────────────
    {
        static Impl m;
        check(m.load_settings("record { clear; whl.omega; tc.cut; ground_speed; }", "base"),
              "a baseline");
        check(m.load_settings("record { -tc.state.cut; every = 5; }\n"
                              "log { file = \"rh.log\"; level = warn; levels { whl = debug; } }",
                              "off"),
              "a second source layers on it");
        check(m.se_config_.columns.size() == 2 && m.se_config_.columns[1].written == "ground_speed",
              "the second source removed one column by its other spelling");
        check(m.se_config_.every == 5 && m.se_config_.log_file == "rh.log" &&
                  m.se_config_.level == se_rt::Level::Warn,
              "and moved the rate and the log");

        static Impl n;
        check(!n.load_settings("record { clear; whl.omega; }\nrecord { evrey = 3; }", "bad"),
              "a misspelt key refuses the source");
        check(has(n.error(), "bad:2: evrey: not a `record` key"), "naming the line and the key");
        check(n.se_config_.columns.size() == 6, "and the first section did not apply");
        check(has(refusal_text("log { level = loud; }"), "expected a log level"),
              "an unknown level is refused");
        check(has(refusal_text("record { whl.radius; }"), "whl.radius: not a recordable signal"),
              "a setting is not a channel");
        check(has(refusal_text("record { file = run.csv; }"), "expected a file name, in quotes"),
              "a file name is a string");
    }

    // ── 4. dump and menu read back ───────────────────────────────────────────
    {
        static Impl a, b;
        check(a.load_settings("record { clear; whl.omega; ws.slip; file = \"x.csv\"; every = 2; }\n"
                              "log { level = warn; levels { whl = debug; } }",
                              "a"),
              "the source to dump");
        const std::string d1 = a.dump_settings();
        check(has(d1, "record {\n    clear;\n    file  = \"x.csv\";\n    every = 2;\n"
                      "    whl.omega;\n    ws.slip;\n}\n"),
              "the dump's record section clears, then names the selection as written");
        check(has(d1, "    level = warn;\n    levels {\n        tc  = debug;\n        whl = debug;\n"),
              "the dump's log section keeps the .sim's levels and adds the new one");
        check(b.load_settings(d1.c_str(), "dump"), "a dump loads");
        check(std::string(b.dump_settings()) == d1, "and dumps back to the same text");

        Sim c, d;
        const std::string menu = c.list_signals();
        check(has(menu, "record {\n    clear;\n"), "the menu starts from scratch");
        check(has(menu, "\n    whl.state.omega;"), "a selected channel is a line");
        check(has(menu, "\n    // whl.drive_torque;"), "an unselected one is commented out");
        check(d.load_settings(menu.c_str(), "menu"), "the menu loads");
        const std::vector<Sim::ChannelInfo> cs = c.channels_list(), ds = d.channels_list();
        bool same = cs.size() == ds.size();
        for (std::size_t i = 0; same && i < cs.size(); ++i) same = cs[i].selected == ds[i].selected;
        check(same, "and selects exactly what it showed as selected");
    }

    // ── 5. two copies, two tables, two logs ──────────────────────────────────
    {
        {   // the log closes with its Sim, so read the files after both are gone
            Sim p, q;
            check(p.record_file("rh_p.csv") && p.log_file("rh_p.log"), "one copy's files");
            check(q.record_file("rh_q.csv") && q.log_file("rh_q.log"), "the other's");
            check(p.init() && q.init(), "both start");
            while (!p.done() || !q.done()) {
                if (!p.done()) p.tick();
                if (!q.done()) q.tick();
            }
            check(p.finish() == 0 && q.finish() == 0, "both finish clean");
        }
        const std::vector<std::string> pc = lines_of("rh_p.csv"), qc = lines_of("rh_q.csv");
        check(pc.size() == 502 && pc == qc, "two complete tables, identical runs");
        const std::vector<std::string> pl = lines_of("rh_p.log"), ql = lines_of("rh_q.log");
        check(!pl.empty() && !ql.empty() && has(pl.back(), "run complete"),
              "two logs, each ending with its own run's outcome");
    }

    // ── 6. a channel read live ───────────────────────────────────────────────
    {
        Sim sim;
        const int omega = sim.channel_handle("whl.state.omega");
        const int bare = sim.channel_handle("whl.omega");
        const int slip = sim.channel_handle("ws.slip");
        const int cut = sim.channel_handle("tc.out.torque_cut");   // not selected
        check(omega == 0 && bare == 1 && slip == 2 && cut == 3, "handles are numbered in order");
        check(sim.channel_handle("whl.nope") == -1, "a path that names nothing gets no handle");
        check(has(sim.error(), "whl.nope: not a recordable signal"), "and error() says why");
        check(std::isnan(sim.read(-1)) && std::isnan(sim.read(99)), "a bad handle reads NaN");
        check(sim.record_file("rh_live.csv") && sim.record_every(1), "a table to compare against");
        check(sim.init(), "the run starts");
        std::vector<double> seen;
        bool same_as_outputs = true, same_twice = true;
        while (!sim.done()) {
            sim.inputs().axle_torque = 300.0;
            sim.inputs().ground_speed = 5.0;
            sim.tick();
            same_as_outputs = same_as_outputs && sim.read(slip) == sim.outputs().ws.slip &&
                              sim.read(cut) == sim.outputs().torque_cut;
            same_twice = same_twice && sim.read(omega) == sim.read(bare);
            seen.push_back(sim.read(omega));
        }
        check(sim.channel_handle("whl.drive_torque") == 4, "a handle may be taken after init()");
        check(std::isnan(sim.read(4)), "and reads NaN until a tick samples it");
        check(sim.finish() == 0, "the run finishes clean");
        check(same_as_outputs, "a port's handle reads what outputs() holds");
        check(same_twice, "both spellings read the same storage");
        const std::vector<std::string> csv = lines_of("rh_live.csv");
        const std::size_t col = 1 + [&] {   // whl.state.omega's column after time
            const std::string& h = csv.empty() ? std::string() : csv[0];
            std::size_t n = 0;
            for (std::size_t at = 0, comma; (comma = h.find(',', at)) != std::string::npos;
                 at = comma + 1, ++n)
                if (h.compare(comma + 1, 15, "whl.state.omega") == 0) return n;
            return std::size_t(99);
        }();
        bool rows_agree = csv.size() == seen.size() + 1 && col < 99;
        for (std::size_t r = 1; rows_agree && r < csv.size(); ++r) {
            std::size_t at = 0;
            for (std::size_t c = 0; c < col; ++c) at = csv[r].find(',', at) + 1;
            const double v = std::strtod(csv[r].c_str() + at, nullptr);
            rows_agree = std::fabs(v - seen[r - 1]) <= 1e-8 * (1.0 + std::fabs(v));
        }
        check(rows_agree, "a state's handle reads what that tick's row records");
    }

    // ── 7. the console's lines, kept ─────────────────────────────────────────
    {
        se_rt::LogSink sink;
        sink.capture(true);
        for (int i = 0; i < 10003; ++i)
            sink.write(se_rt::Level::Warn, "n", static_cast<double>(i), "line " + std::to_string(i));
        se_rt::LogLine l;
        check(sink.next(l) && l.level == se_rt::Level::Warn && l.path == "sim" &&
                  l.text == "3 log lines dropped: the queue holds 10000" && l.time == 2.0,
              "a full queue first says how many it dropped");
        check(sink.next(l) && l.text == "line 3" && l.path == "n", "then the oldest kept line");
        std::size_t rest = 0;
        while (sink.next(l)) ++rest;
        check(rest == 9999 && l.text == "line 10002", "and every other one, in order");

        Sim sim;
        sim.log_capture(true);
        check(sim.log_file(""), "no log file: the console gets every line");
        check(run(sim) == 0, "a captured run finishes clean");
        Sim::LogLine line;
        check(sim.log_next(line) && line.level == Sim::LogLevel::Info && line.path == "sim" &&
                  has(line.text, "run completed") && line.time == 5.0,
              "the run's closing line is kept, not printed");
        check(!sim.log_next(line), "and nothing else is waiting");
    }

    // ── 8. {time} ────────────────────────────────────────────────────────────
    {
        const char* stamp = "20000101-000000";
        std::remove("rh_t_20000101-000000.txt");
        std::remove("rh_t_20000101-000000-2.txt");
        std::string u1, u2;
        std::FILE* f1 = se_rt::open_output("rh_t_{time}.txt", stamp, u1);
        std::FILE* f2 = se_rt::open_output("rh_t_{time}.txt", stamp, u2);
        check(f1 && u1 == "rh_t_20000101-000000.txt", "{time} becomes the stamp");
        check(f2 && u2 == "rh_t_20000101-000000-2.txt", "a name already taken gets -2");
        if (f1) std::fclose(f1);
        if (f2) std::fclose(f2);
        std::string u3;
        std::FILE* f3 = se_rt::open_output("rh_plain.txt", stamp, u3);
        check(f3 && u3 == "rh_plain.txt", "a name without {time} is opened as it is");
        if (f3) std::fclose(f3);

        std::string pa, pb, la, dump;
        {
            Sim a, b;
            check(a.record_file("rh_{time}.csv") && a.log_file("rh_{time}.log"), "a pattern is accepted");
            check(std::string(a.record_file()) == "rh_{time}.csv", "before init(), the pattern");
            check(b.record_file("rh_{time}.csv"), "the same pattern on a second copy");
            check(a.init() && b.init(), "both start");
            pa = a.record_file();
            pb = b.record_file();
            la = a.log_file();
            dump = a.dump_settings();
            a.finish();
            b.finish();
        }
        check(pa.size() == std::string("rh_00000000-000000.csv").size() && pa.rfind("rh_", 0) == 0,
              "after init(), the name opened");
        check(pa != pb, "two copies, two tables");
        check(la.size() == std::string("rh_00000000-000000.log").size(), "the log's name too");
        check(!lines_of(pa.c_str()).empty() && !lines_of(pb.c_str()).empty(), "both were written");
        check(has(dump, "file  = \"rh_{time}.csv\";"), "the dump keeps the pattern");
        std::remove(pa.c_str());
        std::remove(pb.c_str());
        std::remove(la.c_str());

        Sim c;
        check(!c.record_file("x{model}.csv"), "another placeholder is refused");
        check(has(c.error(), "`{model}` is not a placeholder"), "and error() names it");
        check(!c.log_file("x{.log"), "a lone brace is refused");
        check(has(c.error(), "log_file: a `{` with no `}` after it"), "and error() says so");
        check(has(refusal_text("record { file = \"a}b.csv\"; }"), "t:1: file: a `}` with no `{`"),
              "a source's file name is checked the same way");
    }

    if (failures == 0) std::printf("record_host: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
