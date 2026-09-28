// ─────────────────────────────────────────────────────────────────────────────
//  A hand-written host for the generated drivetrain model.
//
//  This file is the regression test for properties that are otherwise easy to
//  break silently:
//
//    1. The public interface is a real seam. `settings()` / `init()` /
//       `inputs()` / `tick()` / `outputs()` / `finish()` are what the emitted
//       main.cpp uses, so a host written against Drivetrain.hpp cannot drift
//       away from what the example host does.
//    2. The root outputs are the ports, read through their wires: `ws` and
//       `torque_cut` as Corner declares them, updated every tick.
//    3. A `rate` override reaches the SCHEDULE (§9.2). The tick guards read a
//       decimation member that resolve_settings() derives, so retuning a
//       compiled binary moves when a node samples and not only what it
//       computes. A decimation baked as a literal passes every other check in
//       this suite, which is why the case is here rather than nowhere.
//    4. The block offset tables (§15.6) address the blocks they describe.
//
//  3 and 4 are about Sim::Impl, which the public header leaves incomplete, so
//  this program #includes Drivetrain.cpp and is compiled on its own: the white
//  box and the public interface in one translation unit. There is no settings
//  loader in the public interface yet (§14.1), so the rate override reaches
//  into Impl for its pin.
//
//  It also drives the root boundary, which the example host does not: the sim
//  file has no syntax for supplying `axle_torque`, so a batch run leaves it at
//  zero and the wheel never turns.
//
//  Built and run by tests/run.ps1. Exit status 0 means every check passed.
// ─────────────────────────────────────────────────────────────────────────────
#include "Drivetrain.cpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "host: FAIL %s\n", what);
    ++failures;
}

}  // namespace

int main() {
    se_rt::logs().configure("", se_rt::Level::Warn);
    se_rt::realtime().configure(0.0, 0.0);   // batch: do not spin on frames

    // ── §9.2 — a `rate` override moves the SCHEDULE, not only the values ─────
    //  This runs FIRST and never calls finish(), because §10.2's control is a
    //  process global with first-writer-wins: once one run completes, no second
    //  run can start in the same process.
    {
        static drivetrain::Sim::Impl retuned;
        check(retuned.tc_decim == 5, "tc elaborates to 200 Hz on a 1 kHz base");

        retuned.tc.param.rate = 500.0;
        retuned.se_pin_[3] = true;
        retuned.resolve_settings();
        check(retuned.config_error().empty(), "500 Hz is an integer divisor of 1 kHz");
        check(retuned.tc_decim == 2, "the decimation followed the rate override");

        std::uint64_t off_sample = 0, total = 0;
        retuned.init();
        for (int i = 0; i < 400 && !retuned.done(); ++i) {
            retuned.sig.in.axle_torque = 250.0;
            retuned.sig.in.ground_speed = 22.0;
            const std::uint64_t k = retuned.tick_index();
            const double before = retuned.dis.tc.cut;
            retuned.tick();
            if (retuned.dis.tc.cut != before) {
                ++total;
                if (k % 2 != 0) ++off_sample;
            }
        }
        check(total > 0, "the retuned controller actually did something");
        check(off_sample == 0, "the retuned state moves on the NEW sample ticks");

        // SE0450's runtime counterpart. Checked without init(), so the failed
        // configuration does not poison the global control for the run below.
        retuned.tc.param.rate = 60.0;
        retuned.resolve_settings();
        check(!retuned.config_error().empty(), "a non-divisor rate is refused");
        check(retuned.tc_decim == 1, "a refused rate does not leave a bogus divisor");
    }

    // ── the block layout is self-describing (§15.6) ──────────────────────────
    {
        static drivetrain::Sim::Impl blocks;
        std::size_t n_sig = 0;
        const se_rt::Slot* sigs = blocks.signal_map(n_sig);
        check(n_sig == 6, "signal_map has one slot per scalar wire and boundary input");
        for (std::size_t i = 0; i < n_sig; ++i)
            check(sigs[i].offset + sigs[i].size <= sizeof(drivetrain::Sim::Impl::Signals),
                  "every signal slot lies inside the block");
        // The offsets are real: writing through the table must be visible
        // through the named member, which is the whole promise an inspector
        // relies on.
        for (std::size_t i = 0; i < n_sig; ++i) {
            if (std::string(sigs[i].path) != "in.ground_speed") continue;
            auto* p = reinterpret_cast<double*>(reinterpret_cast<unsigned char*>(&blocks.sig) +
                                                sigs[i].offset);
            *p = 12.25;
            check(blocks.sig.in.ground_speed == 12.25, "signal_map offsets address the block");
            check(std::string(sigs[i].unit) == "m/s", "signal_map carries the declared unit");
        }

        std::size_t n_dis = 0;
        const se_rt::Slot* discretes = blocks.discrete_map(n_dis);
        check(n_dis == 1, "discrete_map lists tc.cut");
        check(std::string(discretes[0].path) == "tc.cut", "discrete slot is addressed by path");
    }

    // ── the public interface, exactly as a host sees it ──────────────────────
    //  Units are the header's: axle_torque in N*m, ground_speed in m/s.
    drivetrain::Sim sim;
    check(sim.settings().wheel_inertia == 0.95, "settings() start at the .sim's values");
    check(sim.init(), "init() starts the run");
    check(sim.time() == 0.0, "time starts at zero");

    double cut_at_last_change = 0.0;
    std::uint64_t changes_off_sample = 0, changes_total = 0, clock_slips = 0;
    while (!sim.done()) {
        const std::uint64_t k = sim.tick_index();
        if (sim.time() != static_cast<double>(k) * drivetrain::Sim::step_seconds) ++clock_slips;

        sim.inputs().axle_torque = 250.0;
        sim.inputs().ground_speed = 22.0;

        const double before = sim.outputs().torque_cut;
        sim.tick();
        const double after = sim.outputs().torque_cut;
        if (after != before) {
            ++changes_total;
            // tc runs at 200 Hz on a 1 kHz base, so its output may only move on
            // ticks where the decimation divides the index: the zero-order hold
            // the whole-block `dis = nxt` commit has to preserve.
            if (k % 5 != 0) ++changes_off_sample;
            cut_at_last_change = after;
        }
    }
    check(sim.finish() == 0, "a clean run finishes with 0");

    check(clock_slips == 0, "time() is the instant the next tick() samples");
    check(changes_off_sample == 0, "the 200 Hz output only moves on its sample ticks");
    check(changes_total > 0, "the traction controller actually did something");
    check(cut_at_last_change > 0.0, "a positive slip produced a positive torque cut");

    // With the boundary driven, the wheel integrates and slip is finite —
    // the failure the example host cannot avoid.
    check(std::isfinite(sim.outputs().ws.slip), "slip is finite once ground_speed is driven");
    check(sim.outputs().ws.speed > 0.0, "the wheel spun up under a positive axle torque");
    check(std::fabs(sim.time() - 5.0) < 1e-9, "the run reached its configured duration");

    if (failures == 0) std::printf("host: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
