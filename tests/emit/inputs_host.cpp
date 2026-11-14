// ─────────────────────────────────────────────────────────────────────────────
//  Root inputs driven from a CSV table (SPECIFICATION.md §13.8), against the
//  generated InDrive: one root input of each kind, each echoed to an output,
//  so what a tick saw is what outputs() holds after it.
//
//  What a host can rely on:
//
//    1. A double is interpolated between rows, and an int, a bool and a finite
//       input hold the last row. Two rows at one time make a step, the later
//       row applying at that time. Record fields are columns of their own, a
//       finite value may be written by name or by number, and header units are
//       converted, the time column's included.
//    2. The file's columns win over the host's writes; an input it does not
//       name stays the host's. Other columns are ignored, and one info line
//       names them.
//    3. The file's end ends the run when it comes before sim.duration, with
//       reason `completed` and one info line; tick_count() says so from init().
//    4. A table that cannot drive the model is refused at init(), naming the
//       file, the line and the column.
//    5. A row meets a tick within a thousandth of a step: a table sampled
//       faster than the model is interpolated, never snapped to a nearby row.
//    6. A settings file loaded with load_settings_file() finds its inputs file
//       beside itself; text and the key-value call use the working directory.
//       The dump carries the joined name and reads back.
//    7. `{time}` is refused in an inputs file's name.
//
//  Built and run by tests/run.ps1, from the emitted InDrive directory. Exit
//  status 0 means every check passed.
// ─────────────────────────────────────────────────────────────────────────────
#include "InDrive.cpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) return;
    std::fprintf(stderr, "inputs_host: FAIL %s\n", what.c_str());
    ++failures;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

bool near(double a, double b) { return std::fabs(a - b) <= 1e-9 * (1.0 + std::fabs(b)); }

void write_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

using Sim = indrive::Sim;
const double kPi = 3.14159265358979323846;

// What one tick saw, from the echoed outputs.
struct Seen {
    double u, speed, heading, h;
    int n, gear;
    bool on;
};

// Runs to the end, the host writing u = 99 and h = 3 before every tick, and
// returns what each tick saw. `log` gets every captured line.
std::vector<Seen> run(Sim& sim, std::vector<std::string>& log) {
    std::vector<Seen> seen;
    while (!sim.done()) {
        sim.inputs().u = 99.0;
        sim.inputs().h = 3.0;
        sim.tick();
        const Sim::Outputs& o = sim.outputs();
        seen.push_back({o.u_out, o.w_out.speed, o.w_out.heading, o.h_out, o.n_out,
                        static_cast<int>(o.gear_out), o.on_out});
    }
    Sim::LogLine line;
    while (sim.log_next(line)) log.push_back(line.text);
    return seen;
}

bool any_line(const std::vector<std::string>& log, const std::string& part) {
    for (const std::string& l : log)
        if (has(l, part)) return true;
    return false;
}

// A fresh Sim given `file`, whose init() must refuse with `want` in error().
void refused(const char* file, const std::string& text, const std::string& want) {
    write_file(file, text);
    Sim sim;
    sim.log_capture(true);
    check(sim.inputs_file(file), std::string("inputs_file accepts ") + file);
    const bool ok = sim.init();
    check(!ok, std::string(file) + ": init() refuses");
    check(has(sim.error(), want), std::string(file) + ": error names `" + want + "`, got: " + sim.error());
    sim.finish();
}

}  // namespace

int main() {
    // ── 1, 2: interpolation, holds, steps, units, who wins ───────────────────
    write_file("in_a.csv",
               "time (s), u [m/s], n [-], on, gear [indrive.InDrive#Gear], w.speed [km/h], "
               "w.heading [deg], extra [m]\n"
               "0,   0,  1, 0, PARK, 36, 0,   5\n"
               "0.5, 5,  2, 1, 2,    72, 90,  6\n"
               "0.5, 10, 3, 0, HIGH, 72, 90,  6\n"
               "1.0, 20, 4, 1, LOW,  0,  180, 7\n");
    {
        Sim sim;
        sim.log_capture(true);
        check(sim.inputs_file("in_a.csv"), "inputs_file(in_a.csv)");
        check(std::string(sim.inputs_file()) == "in_a.csv", "inputs_file() reads back");
        check(sim.init(), std::string("in_a: init() runs: ") + sim.error());
        check(sim.tick_count() == 100, "in_a: ends with sim.duration, at tick 100");
        std::vector<std::string> log;
        const std::vector<Seen> seen = run(sim, log);
        check(seen.size() == 101, "in_a: 101 ticks, 0 through 100");
        for (std::size_t k = 0; k < seen.size(); ++k) {
            const double t = static_cast<double>(k) * 0.01;
            const Seen& s = seen[k];
            double u, speed, heading;
            int n, gear;
            bool on;
            if (k < 50) {          // between rows 0 and 1; the ints hold row 0
                u = 10.0 * t;
                speed = 10.0 + 20.0 * t;
                heading = kPi / 2 * (t / 0.5);
                n = 1, gear = 0, on = false;
            } else if (k < 100) {  // the step at 0.5 s took row 2; rows 2 to 3
                const double f = (t - 0.5) / 0.5;
                u = 10.0 + 10.0 * f;
                speed = 20.0 - 20.0 * f;
                heading = kPi / 2 + kPi / 2 * f;
                n = 3, gear = 5, on = false;
            } else {               // on the last row
                u = 20.0, speed = 0.0, heading = kPi;
                n = 4, gear = 2, on = true;
            }
            const std::string at = "in_a tick " + std::to_string(k) + ": ";
            check(near(s.u, u), at + "u " + std::to_string(s.u) + " want " + std::to_string(u));
            check(near(s.speed, speed), at + "w.speed, converted from km/h");
            check(near(s.heading, heading), at + "w.heading, converted from deg");
            check(s.n == n, at + "n holds");
            check(s.gear == gear, at + "gear holds, by name and by number");
            check(s.on == on, at + "on holds");
            check(s.h == 3.0, at + "h is the host's");
        }
        check(any_line(log, "in_a.csv: not root inputs, so ignored: extra"), "in_a: the ignored column is named");
        check(any_line(log, "in_a.csv drives u, n, on, gear, w.speed, w.heading; the host drives h"),
              "in_a: the driven inputs and the host's are named");
        check(!any_line(log, "where in_a.csv ends"), "in_a: sim.duration ended the run, not the file");
        check(sim.finish() == 0, "in_a: a clean run");
    }

    // ── 3: the file's end ends the run; sim.duration shorter wins ────────────
    write_file("in_b.csv", "time [ms],u [m/s]\n0,1\n250,2\n");
    {
        Sim sim;
        sim.log_capture(true);
        check(sim.inputs_file("in_b.csv"), "inputs_file(in_b.csv)");
        check(sim.tick_count() == 100, "in_b: before init(), tick_count() is sim.duration's");
        check(sim.init(), std::string("in_b: init() runs: ") + sim.error());
        check(sim.tick_count() == 25, "in_b: after init(), the file's 250 ms");
        std::vector<std::string> log;
        const std::vector<Seen> seen = run(sim, log);
        check(seen.size() == 26, "in_b: 26 ticks, 0 through 25");
        check(!seen.empty() && near(seen.back().u, 2.0), "in_b: the last tick is on the last row");
        check(!seen.empty() && near(seen[10].u, 1.4), "in_b: 100 ms, interpolated from a ms time column");
        check(sim.finish() == 0, "in_b: a clean run");
        Sim::LogLine line;
        while (sim.log_next(line)) log.push_back(line.text);
        check(any_line(log, "the run ended at 0.25 s, where in_b.csv ends"), "in_b: the closing line");
        check(any_line(log, "run completed"), "in_b: reason completed");
    }
    {
        Sim sim;
        sim.log_capture(true);
        check(sim.load_settings("sim.duration = 0.1 (s);\ninputs { file = \"in_b.csv\"; }\n", "short"),
              std::string("short: loads: ") + sim.error());
        check(sim.init(), "short: init() runs");
        check(sim.tick_count() == 10, "short: sim.duration is the shorter, and wins");
        std::vector<std::string> log;
        run(sim, log);
        sim.finish();
        Sim::LogLine line;
        while (sim.log_next(line)) log.push_back(line.text);
        check(!any_line(log, "where in_b.csv ends"), "short: no closing line when sim.duration ended it");
    }

    // ── 4: refusals at init() ────────────────────────────────────────────────
    refused("bad_late.csv", "time (s),u [m/s]\n0.5,1\n1,2\n", "bad_late.csv: it starts at 0.5 s");
    refused("bad_nounit.csv", "time (s),u\n0,1\n", "column `u`: `u` needs a unit, as [m/s]");
    refused("bad_unit.csv", "time (s),u [kg]\n0,1\n", "incompatible units: [kg] where [m/s]");
    refused("bad_back.csv", "time (s),u [m/s]\n0,1\n0.2,1\n0.1,1\n",
            "bad_back.csv:4: the time goes backwards, from 0.2 s to 0.1 s");
    refused("bad_int.csv", "time (s),n\n0,1.5\n", "bad_int.csv:2: column `n`: `1.5` is not a whole number");
    refused("bad_bool.csv", "time (s),on\n0,2\n", "`2` is not 0 or 1");
    refused("bad_enum.csv", "time (s),gear\n0,TOP\n", "`TOP` is not an enumerator of indrive.InDrive#Gear");
    refused("bad_enumv.csv", "time (s),gear\n0,3\n", "`3` is not an enumerator");
    refused("bad_empty.csv", "time (s),u [m/s],n\n0,,1\n", "bad_empty.csv:2: column `u [m/s]`: the cell is empty");
    refused("bad_count.csv", "time (s),u [m/s]\n0,1,2\n", "bad_count.csv:2: 3 fields where the header has 2");
    refused("bad_notime.csv", "u [m/s],time (s)\n1,0\n", "the first column is the time");
    refused("bad_tunit.csv", "time (m),u [m/s]\n0,1\n", "the first column is the time");
    refused("bad_fin.csv", "time (s),gear [m]\n0,PARK\n", "a finite input's bracket is its type");
    refused("bad_dup.csv", "time (s),u [m/s],u [km/h]\n0,1,1\n", "a second column for `u`");
    refused("bad_rows.csv", "time (s),u [m/s]\n", "it has a header and no rows");
    refused("bad_nan.csv", "time (s),u [m/s]\n0,abc\n", "`abc` is not a number");
    {
        Sim sim;
        sim.log_capture(true);
        check(sim.inputs_file("no_such_file.csv"), "inputs_file(no_such_file.csv)");
        check(!sim.init() && has(sim.error(), "no_such_file.csv: cannot read it"), "a missing file is refused");
        sim.finish();
    }

    // ── 1: the dialect ───────────────────────────────────────────────────────
    write_file("in_dialect.csv",
               "\xEF\xBB\xBF\"time (s)\",\"u [m/s]\"\r\n0,1\r\n\r\n  \r\n1, 3 \r\n");
    {
        Sim sim;
        sim.log_capture(true);
        sim.inputs_file("in_dialect.csv");
        check(sim.init(), std::string("dialect: init() runs: ") + sim.error());
        std::vector<std::string> log;
        const std::vector<Seen> seen = run(sim, log);
        check(seen.size() == 101 && near(seen[50].u, 2.0), "dialect: BOM, CRLF, quotes, blank lines");
        sim.finish();
    }

    // ── 5: a dense table is interpolated, never snapped ──────────────────────
    {
        // 1 kHz, offset 0.4 ms from the model's 10 ms ticks, u = t: any
        // snapping to a row within half a step would read up to 5 ms off.
        std::string t = "time (s),u [m/s]\n0,0\n";
        for (int j = 0; j <= 1000; ++j) {
            char row[64];
            const double tt = 0.0004 + j * 0.001;
            std::snprintf(row, sizeof row, "%.9g,%.9g\n", tt, tt);
            t += row;
        }
        write_file("in_dense.csv", t);
        Sim sim;
        sim.log_capture(true);
        sim.inputs_file("in_dense.csv");
        check(sim.init(), "dense: init() runs");
        std::vector<std::string> log;
        const std::vector<Seen> seen = run(sim, log);
        bool all = seen.size() == 101;
        for (std::size_t k = 0; all && k < seen.size(); ++k) all = near(seen[k].u, k * 0.01);
        check(all, "dense: every tick reads u = t, interpolated");
        sim.finish();
    }
    {
        // A row a hair off its tick, as text can read back, counts as on it.
        write_file("in_tol.csv", "time (s),n\n0,1\n0.0099999999,7\n0.02,9\n");
        Sim sim;
        sim.log_capture(true);
        sim.inputs_file("in_tol.csv");
        check(sim.init(), "tol: init() runs");
        std::vector<std::string> log;
        const std::vector<Seen> seen = run(sim, log);
        check(seen.size() == 3 && seen[1].n == 7 && seen[2].n == 9,
              "tol: a row within a thousandth of a step is on the tick");
        sim.finish();
    }

    // ── 6: where a relative name is found; the dump ──────────────────────────
    std::filesystem::create_directories("inp_sub");
    write_file("inp_sub/run.settings", "// a recipe that carries its data\ninputs { file = \"in_b.csv\"; }\n");
    write_file("inp_sub/in_b.csv", "time (s),u [m/s]\n0,5\n0.1,6\n");
    write_file("inp_sub/abs.settings", "inputs { file = \"C:/elsewhere/x.csv\"; }\n");
    {
        Sim sim;
        sim.log_capture(true);
        check(sim.load_settings_file("inp_sub/run.settings"),
              std::string("load_settings_file(inp_sub/run.settings): ") + sim.error());
        check(std::string(sim.inputs_file()) == "inp_sub/in_b.csv", "a settings file's inputs file is beside it");
        const std::string dump = sim.dump_settings();
        check(has(dump, "inputs {\n    file = \"inp_sub/in_b.csv\";\n}\n"), "the dump carries the joined name");
        Sim again;
        check(again.load_settings(dump.c_str(), "dump"), "the dump loads back");
        check(std::string(again.inputs_file()) == "inp_sub/in_b.csv", "and names the same file");
        check(sim.init(), std::string("beside: init() runs: ") + sim.error());
        check(sim.tick_count() == 10, "beside: it read inp_sub/in_b.csv, not ./in_b.csv");
        std::vector<std::string> log;
        const std::vector<Seen> seen = run(sim, log);
        check(!seen.empty() && near(seen[0].u, 5.0), "beside: the values are inp_sub/in_b.csv's");
        sim.finish();
    }
    {
        Sim sim;
        check(sim.load_settings_file("inp_sub/abs.settings"), "abs.settings loads");
        check(std::string(sim.inputs_file()) == "C:/elsewhere/x.csv", "an absolute name is never joined");
        Sim text;
        check(text.load_settings("inputs { file = \"in_b.csv\"; }", "text"), "a text section loads");
        check(std::string(text.inputs_file()) == "in_b.csv", "text is relative to the working directory");
        check(text.load_settings("inputs { file = \"\"; }", "text"), "file = \"\" loads");
        check(std::string(text.inputs_file()).empty(), "file = \"\" means none");
        Sim missing;
        missing.log_capture(true);
        check(!missing.load_settings_file("inp_sub/none.settings"), "a missing settings file is refused");
        check(has(missing.error(), "cannot read inp_sub/none.settings"), "and says so");
        check(!missing.init(), "and the refusal stands");
        missing.finish();
    }
    {
        Sim sim;
        const std::string dump = sim.dump_settings();
        check(has(dump, "// the inputs (section 13.8)\ninputs {\n    file = \"\";\n}\n"),
              "a dump with no inputs file says file = \"\"");
    }

    // ── 7: `{time}` is refused ───────────────────────────────────────────────
    {
        Sim sim;
        check(!sim.inputs_file("run-{time}.csv"), "inputs_file refuses {time}");
        check(has(sim.error(), "`{time}` names a file the run writes"), "and says why");
        check(!sim.inputs_file("run-{date}.csv") && has(sim.error(), "with no placeholder"),
              "inputs_file refuses any other brace");
        Sim text;
        check(!text.load_settings("inputs { file = \"x-{time}.csv\"; }", "t"), "a section refuses {time}");
        check(has(text.error(), "t:1: file: `{time}` names a file the run writes"), "with its origin and line");
        Sim key;
        check(!key.load_settings("inputs { name = \"x.csv\"; }", "k") &&
                  has(key.error(), "not an `inputs` key"),
              "an unknown inputs key is refused");
    }

    if (failures == 0) std::printf("inputs_host: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
