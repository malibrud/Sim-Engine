// ─────────────────────────────────────────────────────────────────────────────
//  Strings in settings, on wires, in vars and on the root (SPECIFICATION.md
//  §5.1b), against the generated Text model.
//
//  What a host can rely on:
//
//    1. A string setting reads back with get_text(), flows down to the leaf
//       that holds it, takes raw characters through set(path, text), and
//       round-trips through the dump, quotes, semicolons and line breaks
//       included.
//    2. Text that doesn't fit, a number at a string path, text at a numeric
//       path, and get()/set(double) on a string are refused, each saying why.
//    3. A root string input reaches the model, a literal drives an input, and
//       the root's string outputs hold what the tick produced.
//    4. Node code that writes past a capacity is cut at a whole character, and
//       logged once per destination.
//    5. The recording quotes every string cell, RFC 4180, so commas, quotes,
//       line breaks and the empty string survive.
//    6. A string channel is read live with read_text(), never with read().
//    7. An inputs file drives a string input: quoted cells, held between rows,
//       a capacity refused at init(), and a recorded run that replays.
//
//  Built and run by tests/run.ps1, from the emitted Text directory. Exit
//  status 0 means every check passed.
// ─────────────────────────────────────────────────────────────────────────────
#include "Text.cpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) return;
    std::fprintf(stderr, "text_host: FAIL %s\n", what.c_str());
    ++failures;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string str(const char* p) { return p ? std::string(p) : std::string("(null)"); }

void write_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

using Sim = text::Sim;

std::vector<std::string> drain(Sim& sim) {
    std::vector<std::string> log;
    Sim::LogLine line;
    while (sim.log_next(line)) log.push_back(line.text);
    return log;
}

int count(const std::vector<std::string>& log, const std::string& part) {
    int n = 0;
    for (const std::string& l : log)
        if (has(l, part)) ++n;
    return n;
}

}  // namespace

int main() {
    // ── 1: settings, flow-down, raw text, the dump ───────────────────────────
    {
        Sim sim;
        check(str(sim.get_text("label")) == "from the sim", "the .sim's root block sets the label");
        check(str(sim.get_text("mid.label")) == "from the sim", "it flows into the composite");
        check(str(sim.get_text("mid.tag.label")) == "from the sim", "and on into the leaf");

        const char* raw = "say \"hi\"; ok\nnext\\line";
        check(sim.set("label", raw), "set() takes raw text at a string path: " + std::string(sim.error()));
        check(str(sim.get_text("label")) == raw, "and get_text() returns it as given");
        check(str(sim.get_text("mid.tag.label")) == raw, "and it flows down");

        bool listed = false;
        for (const Sim::SettingInfo& s : sim.settings_list())
            if (s.path == "label") {
                listed = true;
                check(s.type == "string", "settings_list types it `string`");
                check(s.text == raw, "settings_list carries its text");
                check(std::isnan(s.value), "and its value is NaN");
            }
        check(listed, "settings_list lists the label");

        const std::string dump = sim.dump_settings();
        check(has(dump, "label         = \"say \\\"hi\\\"; ok\\nnext\\\\line\";") ||
                  has(dump, "label = \"say \\\"hi\\\"; ok\\nnext\\\\line\";") ||
                  has(dump, "= \"say \\\"hi\\\"; ok\\nnext\\\\line\";"),
              "the dump writes it as a literal with section 3.6's escapes:\n" + dump);

        Sim back;
        check(back.load_settings(dump.c_str(), "dump"), "the dump loads: " + std::string(back.error()));
        check(str(back.get_text("label")) == raw, "and gives back the same text");
        check(back.dump_settings() == dump, "and dumps the same again");
    }

    // ── 2: refusals ──────────────────────────────────────────────────────────
    {
        Sim sim;
        check(!sim.set("label", "0123456789012345678901234567890123456789"), "40 bytes into string(32)");
        check(has(sim.error(), "40 bytes, and it holds 32"), "says both lengths: " + std::string(sim.error()));
        check(!sim.set("label", 1.0) && has(sim.error(), "takes text"), "set(double) on a string");
        check(std::isnan(sim.get("label")) && has(sim.error(), "get_text() reads"), "get() on a string");
        check(sim.get_text("sim.duration") == nullptr && has(sim.error(), "not a string setting"),
              "get_text() on a number");
        check(sim.get_text("no.such") == nullptr, "get_text() on a path the model lacks");
        check(str(sim.get_text("label")) == "from the sim", "a refused call changes nothing");
    }
    {
        Sim sim;
        check(!sim.load_settings("label = 12;", "n"), "a number at a string path");
        check(has(sim.error(), "expected a string in double quotes"), "says so: " + std::string(sim.error()));
    }
    {
        Sim sim;
        check(!sim.load_settings("sim.duration = \"1\";", "t"), "a string at a numeric path");
        check(has(sim.error(), "a string where a number is expected"), "says so: " + std::string(sim.error()));
    }
    {
        Sim sim;
        check(!sim.load_settings("label = \"x\" (m);", "u"), "a string with a unit");
        check(has(sim.error(), "a string takes no unit"), "says so: " + std::string(sim.error()));
    }
    {
        Sim sim;
        check(!sim.load_settings("label = \"bad \\q\";", "e"), "an unknown escape");
        check(has(sim.error(), "escapes are"), "says which escapes there are: " + std::string(sim.error()));
    }

    // ── 3, 4: the root, a literal, a cut ─────────────────────────────────────
    {
        Sim sim;
        sim.log_capture(true);
        sim.record_clear();
        check(sim.init(), "init: " + std::string(sim.error()));
        sim.inputs().cmd = "go";
        sim.tick();
        const Sim::Outputs& o = sim.outputs();
        check(o.echo == "go", "the root input reaches the leaf and comes back: " + std::string(o.echo));
        check(o.name == "from the sim", "the setting reaches an output");
        check(o.said == "a, \"quoted\" literal", "a literal drives an input: " + std::string(o.said));
        check(o.status == "abcdefg", "11 bytes into 8 keep the 7 before a two-byte character: " +
                                         std::string(o.status));
        sim.inputs().cmd = "stop";
        while (!sim.done()) sim.tick();
        sim.finish();
        const std::vector<std::string> log = drain(sim);
        check(count(log, "cut.out.msg: 11 bytes into string(8), truncated") == 1,
              "the cut is logged once, though it happens every tick");
    }

    // ── 5, 6: the recording, and a live read ─────────────────────────────────
    {
        Sim sim;
        sim.log_capture(true);
        check(sim.set("label", "a,b \"c\"\nd"), "a label with a comma, quotes and a line break");
        check(sim.record_file("text_q.csv"), "record_file");
        const int seen = sim.channel_handle("mid.tag.var.seen");
        const int msg = sim.channel_handle("cut.out.msg");
        check(seen >= 0 && msg >= 0, "string channels take handles");
        check(sim.init(), "init: " + std::string(sim.error()));
        sim.inputs().cmd = "go";
        sim.tick();
        sim.inputs().cmd = "stop";
        sim.tick();
        check(str(sim.read_text(seen)) == "go;stop", "read_text: the var as the last tick sampled it, got " +
                                                        str(sim.read_text(seen)));
        check(str(sim.read_text(msg)) == "abcdefg", "read_text of an output");
        check(std::isnan(sim.read(seen)), "read() of a string is NaN");
        check(sim.read_text(-1) == nullptr && sim.read_text(99) == nullptr, "a bad handle reads nullptr");
        while (!sim.done()) sim.tick();
        sim.finish();
        drain(sim);
        // The file is written in text mode, so on Windows each line break,
        // a quoted cell's included, is CRLF. Compared without the CRs.
        std::string csv;
        for (char c : read_file("text_q.csv"))
            if (c != '\r') csv += c;
        check(has(csv, "time (s),echo [string],mid.tag.out.name [string],mid.tag.var.seen [string],"
                       "lit.out.name [string],cut.out.msg [string]\n"),
              "the header brackets every string column `string`:\n" + csv);
        check(has(csv, "0,\"go\",\"a,b \"\"c\"\"\nd\",\"go\",\"a, \"\"quoted\"\" literal\",\"abcdefg\"\n"),
              "the first row quotes every cell, doubling quotes and keeping the comma and the line "
              "break:\n" + csv);
        check(has(csv, "0.01,\"stop\",\"a,b \"\"c\"\"\nd\",\"go;stop\","), "the second row");
    }

    // ── 7: an inputs file drives a string input ──────────────────────────────
    {
        write_file("cmd_in.csv",
                   "time (s), cmd [string], gain\n"
                   "0,    \"a, \"\"b\"\"\", 1\n"
                   "0.02, go,           2\n"
                   "0.03, \"x\ny\",     2\n"
                   "0.04, \"\",         3\n");
        Sim sim;
        sim.log_capture(true);
        check(sim.inputs_file("cmd_in.csv"), "inputs_file");
        check(sim.record_file("cmd_rec.csv"), "record_file");
        check(sim.record("cmd") && sim.record("gain"), "record the root inputs, for the replay");
        check(sim.init(), "init: " + std::string(sim.error()));
        std::vector<std::string> echo;
        while (!sim.done()) {
            sim.tick();
            echo.push_back(std::string(sim.outputs().echo));
        }
        sim.finish();
        drain(sim);
        check(echo.size() == 5, "the table's end ends the run: 5 ticks, got " + std::to_string(echo.size()));
        if (echo.size() == 5) {
            check(echo[0] == "a, \"b\"", "a quoted cell with a comma and a doubled quote: " + echo[0]);
            check(echo[1] == "a, \"b\"", "a string holds between rows");
            check(echo[2] == "go", "a bare cell");
            check(echo[3] == "x\ny", "a quoted cell with a line break");
            check(echo[4].empty(), "\"\" is the empty string");
        }

        // The recording drives a second run exactly.
        Sim again;
        again.log_capture(true);
        check(again.inputs_file("cmd_rec.csv"), "inputs_file on the recording");
        check(again.record_file("cmd_rec2.csv"), "record_file");
        check(again.record("cmd") && again.record("gain"), "record");
        check(again.init(), "init on the recording: " + std::string(again.error()));
        while (!again.done()) again.tick();
        again.finish();
        drain(again);
        check(read_file("cmd_rec.csv") == read_file("cmd_rec2.csv"), "a recorded run replays exactly");
    }
    {
        auto refused = [](const char* file, const std::string& text, const std::string& want) {
            write_file(file, text);
            Sim sim;
            sim.log_capture(true);
            sim.inputs_file(file);
            check(!sim.init(), std::string(file) + ": init() refuses");
            check(has(sim.error(), want), std::string(file) + ": error names `" + want + "`, got: " +
                                              sim.error());
            sim.finish();
        };
        refused("cmd_long.csv", "time (s),cmd,gain\n0,01234567890123456,1\n",
                "column `cmd`: 17 bytes, and `cmd` holds 16");
        refused("cmd_unit.csv", "time (s),cmd [m],gain\n0,go,1\n", "a string input's bracket is [string]");
        refused("cmd_empty.csv", "time (s),cmd,gain\n0,,1\n", "the cell is empty");
        refused("cmd_open.csv", "time (s),cmd,gain\n0,\"never closed,1\n", "a quoted field is never closed");
        refused("cmd_after.csv", "time (s),cmd,gain\n0,\"go\"x,1\n", "after a quoted field's closing quote");
    }

    if (failures == 0) std::printf("text_host: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
