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
//    3. The block offset tables (§15.6) address the blocks they describe.
//    4. A Sim runs once, and a run's state is the model's own: a fresh Sim
//       runs after a finished one, and of two models side by side, stopping
//       one or breaking its contract leaves the other untouched.
//    5. Pacing is the host's (§9.4): tick() never waits, sync_realtime()
//       waits for the wall to reach time() counting from the first tick, a
//       late call is caught up, and realtime_stats() reports all of it.
//
//  3 is about Sim::Impl, which the public header leaves incomplete, so
//  this program #includes Drivetrain.cpp and is compiled on its own: the white
//  box and the public interface in one translation unit. The settings loader's
//  own checks are in settings_host.cpp.
//
//  It also drives the root boundary, which the example host does not: the sim
//  file has no syntax for supplying `axle_torque`, so a batch run leaves it at
//  zero and the wheel never turns.
//
//  Built and run by tests/run.ps1. Exit status 0 means every check passed.
// ─────────────────────────────────────────────────────────────────────────────
#include "Drivetrain.cpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "host: FAIL %s\n", what);
    ++failures;
}

using wall = std::chrono::steady_clock;

double seconds_since(wall::time_point t0) {
    return std::chrono::duration<double>(wall::now() - t0).count();
}

// The boundary the main section drives, for a whole run on a fresh Sim.
struct Outcome {
    bool          started = false;
    std::uint64_t ticks   = 0;
    double        speed   = 0.0;
    double        cut     = 0.0;
    int           code    = -1;
};

Outcome drive(drivetrain::Sim& s) {
    Outcome o;
    o.started = s.init();
    while (o.started && !s.done()) {
        s.inputs().axle_torque = 250.0;
        s.inputs().ground_speed = 22.0;
        s.tick();
    }
    o.ticks = s.tick_index();
    o.speed = s.outputs().ws.speed;
    o.cut   = s.outputs().torque_cut;
    o.code  = s.finish();
    return o;
}

}  // namespace

int main() {
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
    const wall::time_point run_began = wall::now();
    check(sim.settings().wheel_inertia == 0.95, "settings() start at the .sim's values");
    check(sim.init(), "init() starts the run");
    check(sim.time() == 0.0, "time starts at zero");
    check(!sim.load_settings("whl.radius = 0.3 (m);"), "a load after init() is refused (§6.2b)");
    check(std::string(sim.error()).find("after init()") != std::string::npos,
          "and error() says configuration is over");

    double cut_at_last_change = 0.0;
    std::uint64_t changes_total = 0, clock_slips = 0;
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
            cut_at_last_change = after;
        }
    }
    check(sim.finish() == 0, "a clean run finishes with 0");
    // Generous on purpose: the point is that tick() did not spin, not how fast
    // this machine is. A 5 s model that waited on the wall would take 5 s.
    check(seconds_since(run_began) < 2.5, "tick() never waits on the wall clock");
    {
        const drivetrain::Sim::RealtimeStats st = sim.realtime_stats();
        check(st.frames == 0 && st.overruns == 0 && st.utilization == 0.0 &&
                  st.overrun_pct == 0.0 && st.lag == 0.0,
              "a run that never called sync_realtime() reports no pacing");
    }

    check(clock_slips == 0, "time() is the instant the next tick() samples");
    check(changes_total > 0, "the traction controller actually did something");
    check(cut_at_last_change > 0.0, "a positive slip produced a positive torque cut");

    // With the boundary driven, the wheel integrates and slip is finite —
    // the failure the example host cannot avoid.
    check(std::isfinite(sim.outputs().ws.slip), "slip is finite once ground_speed is driven");
    check(sim.outputs().ws.speed > 0.0, "the wheel spun up under a positive axle torque");
    check(std::fabs(sim.time() - 5.0) < 1e-9, "the run reached its configured duration");

    // ── a Sim runs once, and runs share nothing ──────────────────────────────
    //  Every piece of run state -- clock, stop flag, contracts, frame, log --
    //  lives in the model's own se_rt::Run. So the run above leaves nothing
    //  behind, and two models side by side cannot see each other.
    check(!sim.init(), "a second init() on the same Sim is refused");
    check(std::string(sim.error()).find("runs once") != std::string::npos,
          "and error() says to construct a new one");

    {
        drivetrain::Sim a;
        const Outcome ra = drive(a);
        drivetrain::Sim b;
        const Outcome rb = drive(b);
        check(ra.started && rb.started, "a fresh Sim starts after another has finished");
        check(ra.ticks == a.tick_count() && rb.ticks == b.tick_count(),
              "and both run to their configured length");
        check(ra.code == 0 && rb.code == 0, "and both finish clean");
        check(ra.speed == rb.speed && ra.cut == rb.cut,
              "two runs of one model, one after the other, agree exactly");
        check(rb.speed == sim.outputs().ws.speed && rb.cut == sim.outputs().torque_cut,
              "and agree with the first run in this process");
    }

    {
        //  White box, so one model can be stopped and given a broken contract
        //  directly, as a body would, while the other is watched.
        auto p = std::make_unique<drivetrain::Sim::Impl>();
        auto q = std::make_unique<drivetrain::Sim::Impl>();
        check(p->start() && q->start(), "two models start side by side");

        const se_rt::Level before = q->se_run_.log.level_for("whl");
        const se_rt::Level other =
            before == se_rt::Level::Error ? se_rt::Level::Trace : se_rt::Level::Error;
        p->se_run_.log.set_path_level("whl", other);
        check(p->se_run_.log.level_for("whl") == other, "one model's log level moves");
        check(q->se_run_.log.level_for("whl") == before, "and the other's does not");

        for (drivetrain::Sim::Impl* m : {p.get(), q.get()}) {
            m->sig.in.axle_torque = 250.0;
            m->sig.in.ground_speed = 22.0;
        }
        for (int k = 0; k < 10; ++k) {
            p->tick();
            q->tick();
        }
        p->se_run_.control.stop("stopped by the host");
        p->se_run_.checks.fail(p->se_run_.log, "whl", "drivetrain.Wheel",
                               "a contract the host broke", "on purpose", 0.0, 0);
        p->tick();
        check(p->done(), "a stopped model is done at its next tick");
        check(!q->se_run_.control.halted(), "and the other model is still running");
        check(!q->se_run_.checks.any(), "and has no contract failures");

        while (!q->done()) q->tick();
        check(q->tick_index() == q->se_end_tick_,
              "the other model runs to its configured length");
        p->finish();
        q->finish();
        check(p->exit_code() == 1, "the model with a broken contract fails");
        check(q->exit_code() == 0, "and the other finishes clean");
    }

    // ── pacing is the host's (§9.4, §13.6) ───────────────────────────────────
    {
        const double step = drivetrain::Sim::step_seconds;
        drivetrain::Sim s;
        check(s.init(), "a Sim to pace starts");

        // The anchor is the first tick, not init(): this delay is not lateness.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const wall::time_point t0 = wall::now();
        for (int k = 0; k < 20; ++k) s.tick();
        s.sync_realtime();
        // The anchor is taken inside the first tick, after t0, so the wait
        // covers at least 20 steps (less a clock tick of rounding).
        check(seconds_since(t0) >= 20 * step - 1e-6,
              "20 ticks then sync_realtime() take 20 steps of wall time");
        drivetrain::Sim::RealtimeStats st = s.realtime_stats();
        check(st.frames == 1, "one sync_realtime() is one frame");
        check(st.overruns == 0 && st.lag == 0.0,
              "a delay between init() and the first tick() is not an overrun");
        s.sync_realtime();
        check(s.realtime_stats().frames == 1, "a call with no tick since the last does nothing");

        // A stall well past one frame: the next call finds the wall ahead.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (int k = 0; k < 10; ++k) s.tick();
        s.sync_realtime();
        st = s.realtime_stats();
        check(st.overruns == 1, "a call that finds the wall ahead is an overrun");
        check(st.lag > 0.05, "and reports how far ahead the wall is");
        check(st.overrun_pct > 0.0, "and shows in the overrun rate");

        // It catches up: frames run without waiting until the lag is gone.
        int calls = 0;
        while (s.realtime_stats().lag > 0.0 && !s.done() && calls < 400) {
            for (int k = 0; k < 10 && !s.done(); ++k) s.tick();
            s.sync_realtime();
            ++calls;
        }
        check(s.realtime_stats().lag == 0.0, "the model catches up with the wall");
        check(s.realtime_stats().overruns > 1,
              "and the calls that caught up were late, not waited on");
        check(std::fabs(seconds_since(t0) - s.time()) < 0.05,
              "once caught up, wall time since the first tick matches time()");
        check(s.finish() == 0, "a paced run finishes clean");
    }

    if (failures == 0) std::printf("host: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
