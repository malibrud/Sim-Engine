// ─────────────────────────────────────────────────────────────────────────────
//  A hand-written host for the generated drivetrain model.
//
//  This file is the regression test for two properties that are otherwise easy
//  to break silently:
//
//    1. The model is a HEADER with no `main()` of its own, so a host program
//       can include it and supply its own. If the generator ever goes back to
//       emitting one translation unit with `int main()` in it, this file fails
//       to link.
//    2. `init()` / `done()` / `tick()` / `finish()` is a real seam. The batch
//       driver in Sim.main.cpp is written in terms of the same calls, so a host
//       cannot drift away from what the generated driver does.
//    3. A `rate` override reaches the SCHEDULE (§9.2). The tick guards read a
//       decimation member that resolve_settings() derives, so retuning a
//       compiled binary moves when a node samples and not only what it
//       computes. A decimation baked as a literal passes every other check in
//       this suite, which is why the case is here rather than nowhere.
//
//  It also drives the root boundary, which the batch driver cannot: the sim
//  file has no syntax for supplying `in.axle_torque`, so a batch run leaves it
//  at zero and the wheel never turns. That is what this exists to fix.
//
//  Built and run by tests/run.ps1. Exit status 0 means every check passed.
// ─────────────────────────────────────────────────────────────────────────────
#include "Sim.generated.hpp"

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
    //
    //  The pin index is spelled out here because §14.1's loader does not exist
    //  yet; when it does, this becomes load_settings("tc.rate = 500 (Hz);").
    {
        static drivetrain::Sim retuned;
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

    static drivetrain::Sim sim;

    // ── the block layout is self-describing (§15.6) ──────────────────────────
    std::size_t n_sig = 0;
    const se_rt::Slot* sigs = sim.signal_map(n_sig);
    check(n_sig == 6, "signal_map has one slot per scalar wire and boundary input");

    const auto* base = reinterpret_cast<const unsigned char*>(&sim.sig);
    for (std::size_t i = 0; i < n_sig; ++i) {
        check(sigs[i].offset + sigs[i].size <= sizeof(drivetrain::Sim::Signals),
              "every signal slot lies inside the block");
        (void)base;
    }
    // The offsets are real: writing through the table must be visible through
    // the named member, which is the whole promise an inspector relies on.
    for (std::size_t i = 0; i < n_sig; ++i) {
        if (std::string(sigs[i].path) != "in.ground_speed") continue;
        auto* p = reinterpret_cast<double*>(reinterpret_cast<unsigned char*>(&sim.sig) +
                                            sigs[i].offset);
        *p = 12.25;
        check(sim.sig.in.ground_speed == 12.25, "signal_map offsets address the block");
        check(std::string(sigs[i].unit) == "m/s", "signal_map carries the declared unit");
    }

    std::size_t n_dis = 0;
    const se_rt::Slot* discretes = sim.discrete_map(n_dis);
    check(n_dis == 1, "discrete_map lists tc.cut");
    check(std::string(discretes[0].path) == "tc.cut", "discrete slot is addressed by path");

    // ── drive the boundary and run ───────────────────────────────────────────
    //  Units are the manifest's [boundary.in]: N*m and m/s.
    const double axle_torque = 250.0;
    const double ground_speed = 22.0;

    double cut_at_last_sample = 0.0;
    std::uint64_t changes_off_sample = 0;
    std::uint64_t changes_total = 0;

    sim.init();
    check(sim.time() == 0.0, "time starts at zero");

    while (!sim.done()) {
        sim.sig.in.axle_torque = axle_torque;
        sim.sig.in.ground_speed = ground_speed;

        const std::uint64_t k = sim.tick_index();
        const double before = sim.dis.tc.cut;
        sim.tick();
        const double after = sim.dis.tc.cut;

        if (after != before) {
            ++changes_total;
            // tc runs at 200 Hz on a 1 kHz base, so its discrete state may only
            // move on ticks where the decimation divides the index. This is the
            // property the whole-block `dis = nxt` commit has to preserve.
            if (k % 5 != 0) ++changes_off_sample;
            cut_at_last_sample = after;
        }
    }
    sim.finish();

    check(changes_off_sample == 0, "the 200 Hz state only moves on its sample ticks");
    check(changes_total > 0, "the traction controller actually did something");
    check(cut_at_last_sample > 0.0, "a positive slip produced a positive torque cut");

    // With the boundary driven, the wheel integrates and slip is finite —
    // the failure the batch driver cannot avoid.
    check(std::isfinite(sim.sig.whl.ws.slip), "slip is finite once ground_speed is driven");
    check(sim.x[0] > 0.0, "the wheel spun up under a positive axle torque");
    check(std::fabs(sim.time() - 5.0) < 1e-9, "the run reached its configured duration");

    if (failures == 0) std::printf("host: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
