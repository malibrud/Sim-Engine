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
//
//  Like settings_host.cpp it #includes Drivetrain.cpp, so it can look at the
//  configuration a call lands in.
//
//  Built and run by tests/run.ps1. Exit status 0 means every check passed.
// ─────────────────────────────────────────────────────────────────────────────
#include "Drivetrain.cpp"

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

    if (failures == 0) std::printf("record_host: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
