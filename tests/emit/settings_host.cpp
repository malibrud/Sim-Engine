// ─────────────────────────────────────────────────────────────────────────────
//  The settings loader in a compiled model (SPECIFICATION.md §14.1), against
//  the generated drivetrain.
//
//  What a host can rely on when it hands a model a §14 source at run time:
//
//    1. A value's unit is checked AND converted, with the same table `sec`
//       uses: centimetres land in metres and rpm in rad/s.
//    2. Every refusal names the origin, the line, the path and the reason, in
//       `sec`'s own words where `sec` has some.
//    3. A load is all or nothing, and a refusal stands: no later load gets
//       in, and init() will not run.
//    4. Loads layer with the host's own settings() edits, last writer wins.
//    5. dump_settings() reads back through load_settings() to the same text.
//    6. The same paths one key at a time: settings_list(), get(), set() in the
//       canonical unit or as text, and sim.duration among them.
//
//  Like host.cpp it #includes Drivetrain.cpp, so it can look at the members a
//  binding lands in. Each check builds its own model, and a model's run state
//  is its own, so a refused init() in one check cannot reach the next.
//
//  Built and run by tests/run.ps1. Exit status 0 means every check passed.
// ─────────────────────────────────────────────────────────────────────────────
#include "Drivetrain.cpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "settings_host: FAIL %s\n", what);
    ++failures;
}

bool has(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

bool near(double a, double b) { return std::fabs(a - b) <= 1e-12 * (std::fabs(b) + 1.0); }

using Impl = drivetrain::Sim::Impl;

// A source on a fresh model: the error, or "" if it was accepted.
std::string refusal(const char* text) {
    Impl m;
    return m.load_settings(text, "t") ? std::string() : std::string(m.error());
}

}  // namespace

int main() {
    // ── 1. converted to the canonical unit, and pinned ───────────────────────
    {
        static Impl m;
        check(m.load_settings("whl.radius = 31.5 (cm);\nwhl.omega = 60 (rpm);", "conv"),
              "a compatible unit is accepted");
        check(near(m.whl.param.radius, 0.315), "31.5 cm lands as 0.315 m");
        check(m.se_pin_[2], "a bound setting is pinned");
        check(near(m.se_ic_.whl_omega, 2.0 * 3.14159265358979323846),
              "60 rpm lands as 2 pi rad/s in the initial condition");
        check(m.load_settings("whl.radius = 0.25;", "bare"), "no unit means the canonical one");
        check(m.whl.param.radius == 0.25, "a bare number is taken as written");
    }

    // ── 2. every refusal says where and why ──────────────────────────────────
    {
        const std::string u = refusal("wheel_inertia = 3 (m);");
        check(has(u, "t:1: wheel_inertia: incompatible units: (m) where (kg*m^2) is declared"),
              "a wrong dimension is refused in SE0410's words");
        check(has(refusal("whl.radius = 2 * 0.1;"), "not an expression"),
              "an expression is refused");
        check(has(refusal("whl.radius = pi;"), "not a name or an expression"),
              "a section-7 constant is refused");
        check(has(refusal("whl.nope = 1;"), "whl.nope: not a setting or a state"),
              "an unknown path is refused by name");
        check(has(refusal("whl.ws.slip = 1;"), "an output is computed"),
              "an output is refused");
        check(has(refusal("whl = 1;"), "set one leaf at a time, as `whl."),
              "an instance is refused, pointing at a leaf");
        check(has(refusal("whl.omega[0] = 1;"), "not an array state"),
              "an index on a scalar is refused");
        check(has(refusal("whl.radius = 1 (furlong);"), "unknown unit symbol `furlong`"),
              "an unknown unit symbol is refused");
        check(has(refusal("whl.radius = 1 (degC*m);"), "offset unit `degC` in a compound unit"),
              "an offset unit in a compound is refused, as SE0411 refuses it");
        check(has(refusal("whl.radius = 1 (m)"), "expected `;`"), "a missing `;` is refused");
        check(has(refusal("// a comment\n/* and a\n block */\nwhl.radius = 1 (s);"),
                  "t:4: whl.radius"),
              "the line count survives both kinds of comment");
        check(refusal("tc.gain = 1 (N*m*rad);").empty(),
              "angle absorbs into a positive length power (section 4.5), as it does in sec");
    }

    // ── 3. all or nothing, and a refusal stands ──────────────────────────────
    {
        static Impl m;
        check(!m.load_settings("whl.radius = 0.5 (m);\nwhl.nope = 1;", "two"),
              "one bad line refuses the source");
        check(has(m.error(), "two:2: whl.nope"), "the refusal names the second line");
        check(m.whl.param.radius == 0.315 && !m.se_pin_[2],
              "the good line before it was not applied");
        check(!m.load_settings("whl.radius = 0.4 (m);", "later"),
              "a later, valid source is refused while the first refusal stands");
        check(has(m.error(), "two:2:"), "and error() still names the first");
    }

    // ── 4. layering with settings() ──────────────────────────────────────────
    {
        static Impl m;
        m.se_settings_.wheel_inertia = 2.0;   // what sim.settings().wheel_inertia = 2 does
        check(m.load_settings("whl.radius = 0.3 (m);", "layer"), "a load beside a host edit");
        check(m.se_cfg_root.wheel_inertia == 2.0 && m.se_pin_[0],
              "the host's earlier edit was taken first, not lost");
        check(m.load_settings("wheel_inertia = 3 (kg*m^2);", "later"), "a later writer");
        check(m.se_settings_.wheel_inertia == 3.0, "settings() shows the later writer");
    }

    // ── 5. the dump round-trips ──────────────────────────────────────────────
    {
        static Impl a, b;
        check(a.load_settings("whl.radius = 30 (cm);\ntc.slip_limit = 0.1;\nwhl.omega = 7;", "a"),
              "the source to dump");
        const std::string d1 = a.dump_settings();
        check(has(d1, "\nwhl.radius    = 0.3 (m);\n"), "the dump writes canonical units");
        check(has(d1, "\ntc.slip_limit = 0.1;\n"), "a dimensionless value carries no unit");
        check(has(d1, "// initial conditions\nwhl.omega     = 7 (rad/s);\n"),
              "initial conditions follow the settings");
        check(b.load_settings(d1.c_str(), "dump"), "a dump loads");
        check(std::string(b.dump_settings()) == d1, "and dumps back to the same text");
    }

    // ── 6. one key at a time ─────────────────────────────────────────────────
    {
        drivetrain::Sim sim;
        const std::vector<drivetrain::Sim::SettingInfo> list = sim.settings_list();
        auto row = [&](const char* path) -> const drivetrain::Sim::SettingInfo* {
            for (const auto& s : list)
                if (s.path == path) return &s;
            return nullptr;
        };
        const auto* radius = row("whl.radius");
        const auto* omega = row("whl.omega");
        const auto* length = row("sim.duration");
        check(radius && radius->kind == drivetrain::Sim::SettingKind::Setting && radius->unit == "m",
              "a setting is listed with its kind and canonical unit");
        check(omega && omega->kind == drivetrain::Sim::SettingKind::Initial, "so is an initial condition");
        check(length && length->kind == drivetrain::Sim::SettingKind::Run && length->unit == "s" &&
                  near(length->value, 5.0),
              "and the run's length, at the .sim's value");
        check(list.back().path == "sim.duration", "the run's length comes last");

        check(sim.set("whl.radius", 0.3) && near(sim.get("whl.radius"), 0.3),
              "set() in the canonical unit, and get() reads it back");
        check(sim.set("whl.radius", "31.5 (cm)") && near(sim.get("whl.radius"), 0.315),
              "set() with a unit converts it");
        check(!sim.set("whl.radius", "1 (s)"), "a wrong unit is refused");
        check(std::string(sim.error()) == "whl.radius: incompatible units: (s) where (m) is declared",
              "and error() names the path and the reason, with no origin or line");
        check(near(sim.get("whl.radius"), 0.315), "and changed nothing");
        check(!sim.set("whl.radius", "1; tc.slip_limit = 9"), "one value only");
        check(!sim.set("whl.nope", 1.0), "a path the model does not have is refused");
        check(std::isnan(sim.get("whl.nope")) && has(sim.error(), "whl.nope: not a setting"),
              "and get() of one is NaN, with the reason");
        check(!sim.set("sim.duration", "1.0005 (s)"), "a length that is not a whole number of steps");
        check(has(sim.error(), "not a whole number of base steps"), "is refused, saying so");
        check(sim.set("sim.duration", 2.0) && sim.tick_count() == 2000,
              "the run's length moves tick_count()");
        check(has(sim.dump_settings(), "\nsim.duration") && has(sim.dump_settings(), "= 2 (s);\n"),
              "and the dump writes it");
        check(sim.init(), "a refused set() did not stop the run");
        std::uint64_t ticks = 0;
        while (!sim.done()) {
            sim.tick();
            ++ticks;
        }
        check(ticks == 2001 && near(sim.time(), 2.0), "the run is as long as sim.duration said");
        check(!sim.set("whl.radius", 0.2), "a set() after init() is refused");
        check(has(sim.error(), "set() comes before init()"), "and says configuration is over");
        sim.finish();
    }

    // ── the public interface, and a refused source stops the run ─────────────
    {
        drivetrain::Sim sim;
        check(!sim.load_settings("whl.radius = 1 (s);"), "the public call refuses too");
        check(has(sim.error(), "settings:1: whl.radius: incompatible units"),
              "the default origin is `settings`");
        check(!sim.init(), "init() will not run after a refused source");
        check(has(sim.error(), "whl.radius: incompatible units"),
              "and says it is the refused source that stopped it");
    }

    if (failures == 0) std::printf("settings_host: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
