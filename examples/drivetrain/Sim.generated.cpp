// ─────────────────────────────────────────────────────────────────────────────
//  GENERATED — DO NOT EDIT.
//  Source: examples/drivetrain/{WheelState,Wheel,TractionController,Corner}.se
//  Root:   drivetrain.Corner  (instantiated once as `fl`)
//  Base step: 1 kHz
//
//  This file is illustrative — hand-written to match what the emitter would
//  produce, so the example teaches the lowering alongside the surface syntax.
//  The emitter is now real; run
//
//      sec --emit -I examples -o <dir> examples/drivetrain/Drivetrain.sim
//
//  for the actual output. It differs from this file in one visible way: model
//  paths are ROOT-RELATIVE (§13.2), as `Drivetrain.sim` says, so the members
//  are `whl` and `tc` rather than the `fl_` prefixed names below. The `fl.`
//  here is left over from an earlier scenario where `Corner` was nested inside
//  a vehicle, and is retained only so the prefixing rule stays visible.
//
//  What to notice:
//    1. `Corner` is ABSENT. Composites are elaboration-time scaffolding; their
//       settings fold to literals and they leave no runtime trace.
//    2. A wire is not its own variable — it coincides with the producer's `out`
//       storage. Fan-out is free, nothing is copied.
//    3. Units are CHECKED BY THE DSL AND ERASED. Every declaration below is a
//       plain `double` that is, by construction, in its declared unit — see the
//       trailing comment on each one, and Sim.units.txt for the full manifest.
//       Nothing here depends on a units library; this file compiles with a
//       stock C++20 compiler and no third-party packages.
//    4. Purity and feedthrough are enforced by the C++ type system, not by an
//       analyzer. See the notes on each method below.
// ─────────────────────────────────────────────────────────────────────────────

#include "state_ref.hpp"

#include <array>
#include <cstdint>

namespace drivetrain {

using sim::state_ref;

// ═════════════════════════════════════════════════════════════════════════════
//  type WheelState                                          [WheelState.se:10]
//
//  The user-defined record. Unchanged by every lowering choice — it is a plain
//  struct with one member per field, each carrying its declared unit as a
//  comment.
// ═════════════════════════════════════════════════════════════════════════════
struct WheelState {
    double speed;    // rad/s
    double torque;   // N*m
    double slip;     // -
};

// ═════════════════════════════════════════════════════════════════════════════
//  node Wheel                                                    [Wheel.se:4]
//
//  A leaf. Leaves survive as their own classes, so `Wheel` is compiled once no
//  matter how many corners the model instantiates.
// ═════════════════════════════════════════════════════════════════════════════
struct Wheel {
    struct Param { double inertia;      // kg*m^2
                   double radius; };    // m
    struct Var   { double inertia_inv; };   // 1/(kg*m^2)

    // `continuous omega` => a proxy bound to a slot in Sim::x, not real storage.
    struct State { state_ref omega; };   // rad/s
    struct Der   { state_ref omega; };   // rad/s^2 — bound into Sim::xd

    struct Out   { WheelState ws; };

    // FEEDTHROUGH IS THIS: one In view per method, holding exactly the inputs
    // that method's DSL signature listed. Reading an input you did not declare
    // is not a stale value — it is "no such member", a compile error.
    struct In_derivative { double drive_torque; };   // N*m
    struct In_output { double drive_torque;       // N*m
                       double ground_speed; };    // m/s

    Param param;
    State state;
    Var   var;

    // init() is the only mutating method here — non-const, writes state and var.
    void init() {
#line 24 "examples/drivetrain/Wheel.se"
        var.inertia_inv = 1.0 / param.inertia;
    }

    // PURITY IS THIS: `const` on the method. The body physically cannot write
    // state, var, or param. `der` is the sole out-param.
    void derivative(const In_derivative& in, Der& der) const {
#line 30 "examples/drivetrain/Wheel.se"
        der.omega = in.drive_torque * var.inertia_inv;
    }

    void output(const In_output& in, Out& out) const {
#line 38 "examples/drivetrain/Wheel.se"
        auto surface_speed = state.omega * param.radius;
        out.ws.speed  = state.omega;
        out.ws.torque = in.drive_torque;
        out.ws.slip   = (surface_speed - in.ground_speed) / in.ground_speed;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
//  node TractionController                          [TractionController.se:4]
//
//  Registered discrete. Note there is no `rate` member: `rate` is an
//  elaboration-time constant, folded into the schedule's decimation test below.
//  It costs nothing at runtime.
// ═════════════════════════════════════════════════════════════════════════════
struct TractionController {
    struct Param { double slip_limit;   // -
                   double gain; };      // N*m

    // `discrete cut` => real storage in the node. It never reaches the solver.
    struct State { double cut; };   // N*m

    // The x+ = h(t,x,u) buffer. Separate storage is what makes `next.` a
    // SIMULTANEOUS update: every discrete state reads the old value and writes
    // the new one, so results do not depend on statement order in next().
    // Named `Store`, not `Next`: the same type is both the committed block and
    // the buffer, so naming it for the buffer would be wrong half the time.
    struct Store { double cut; };   // N*m

    struct Out   { double torque_cut; };   // N*m

    struct In_next   { WheelState ws; };
    struct In_output {};                    // empty => sort root => breaks loops

    Param param;
    State state;

    // const and PURE: it carries the transition equation and writes nothing
    // else. `var`, natives and logging would live in on_step(), which this node
    // does not need. The out-parameter shadows the method name, which is fine —
    // the body is verbatim, so `next.cut` resolves to the parameter (§8.1).
    void next(const In_next& in, Store& next) const {
#line 24 "examples/drivetrain/TractionController.se"
        auto excess = in.ws.slip - param.slip_limit;
        next.cut = excess > 0.0 ? param.gain * excess : 0.0;
    }

    void output(const In_output& in, Out& out) const {
#line 32 "examples/drivetrain/TractionController.se"
        out.torque_cut = state.cut;
    }

    // There is deliberately no per-node commit (§6.4): the real emitter holds
    // one `Discrete` block and one `nxt` buffer of the same shape for the whole
    // model, and the tick ends with a single `dis = nxt`. Simultaneity is a
    // property of the MODEL, not of each node in turn.
};

// ═════════════════════════════════════════════════════════════════════════════
//  Sim — the whole model
// ═════════════════════════════════════════════════════════════════════════════
struct Sim {
    // ── continuous state vector ──────────────────────────────────────────────
    //  Plain doubles — as is everything else in this file. This is also the
    //  external solver's whole ABI.
    //
    //  SLOT MAP  (value is in the state's declared unit)
    //      x[0] / xd[0]  =  fl.whl.omega   [rad/s]
    //
    //  Discrete states are NOT here — fl.tc.cut lives in its node.
    static constexpr std::size_t n_states = 1;
    std::array<double, n_states> x{};
    std::array<double, n_states> xd{};

    // ── leaf instances ───────────────────────────────────────────────────────
    //  Composite paths become name prefixes. Identifier spelling is entirely
    //  the generator's business — it names these with global knowledge at emit
    //  time, so uniqueness is guaranteed by construction. Users only ever write
    //  model paths (`fl.whl`), which are unambiguous by sibling scoping.
    Wheel              fl_whl;   // model path: fl.whl
    TractionController fl_tc;    // model path: fl.tc

    // ── wires ────────────────────────────────────────────────────────────────
    //  A wire IS the producer's out storage. `whl.ws --> tc.ws, self.ws` is two
    //  readers of fl_whl_out.ws — no copy, no separate wire variable.
    //
    //  This is also where ZOH comes from for free: fl_tc_out is written only on
    //  fl.tc's sample ticks, so on the other nine base ticks the held value is
    //  simply the variable still sitting there. No hold buffer exists.
    Wheel::Out              fl_whl_out;
    TractionController::Out fl_tc_out;

    // ── solver / update scratch ──────────────────────────────────────────────
    //  `der` is not node data: it is evaluated 4x per RK4 step at trial states
    //  the trajectory never visits, then discarded. Sim owns it.
    Wheel::Der               fl_whl_der;
    TractionController::Store fl_tc_next;

    // ── root boundary ────────────────────────────────────────────────────────
    double src_axle_torque{};    // N*m
    double src_ground_speed{};   // m/s

    Sim() {
        bind_states();
        elaborate();
    }

    // Bind the named state proxies to their slots. Done once.
    void bind_states() {
        fl_whl.state.omega.bind(x[0]);
        fl_whl_der.omega.bind(xd[0]);
    }

    // Setting flow-down, evaluated at elaboration. `Corner::wheel_inertia` does
    // not exist at runtime — the expression `param.wheel_inertia` folded to a
    // literal here. Composite settings evaporate entirely.
    //
    // Any unit CONVERSION the DSL had to apply folded into these literals too:
    // a setting declared (km/h) and fed a (m/s) expression arrives here as one
    // already-scaled number. This is why C++ never needs conversion machinery.
    void elaborate() {
        fl_whl.param.inertia   = 0.9;     // kg*m^2
        fl_whl.param.radius    = 0.31;    // m
        fl_tc.param.slip_limit = 0.10;    // -
        fl_tc.param.gain       = 400.0;   // N*m
        // fl.tc.rate = 200 Hz on a 1 kHz base => decimation 5, see next_tick.
    }

    // Declarative state defaults, including discrete ones. Writing through the
    // proxy lands in x[0]. Sim-file IC overrides would be applied right here —
    // the single override channel, which is what discrete states buy over vars.
    void apply_initial_conditions() {
        fl_whl.state.omega = 0.0;   // rad/s
        fl_tc.state.cut    = 0.0;   // N*m
    }

    void init() {
        apply_initial_conditions();
        fl_whl.init();          // elaboration order
        // fl_tc has no init(): its IC is declarative.
    }

    void final() {
        // reverse elaboration order, only for nodes whose init() completed.
    }

    // ── schedule ─────────────────────────────────────────────────────────────

    // Topological order over feedthrough edges only, across the nodes active
    // this tick. fl_tc has an empty In_output, so it is a sort root.
    void output_pass() {
        fl_tc.output({}, fl_tc_out);
        fl_whl.output({src_axle_torque, src_ground_speed}, fl_whl_out);
    }

    // The external solver's entry point: reads x, fills xd.
    //
    // Buffer ownership, stated plainly: Sim owns x. A solver that owns its own
    // vector (CVODE, odeint) copies in on entry and reads xd back out — one
    // memcpy of n_states doubles each way per evaluation. The proxies buy a
    // clean layout, not zero copying.
    void derivatives(double /*t*/) {
        output_pass();
        fl_whl.derivative({src_axle_torque}, fl_whl_der);
    }

    // x+ = h(t,x,u), once per accepted base step, after outputs have
    // propagated. `on_step()` — the mutating hook for `var`, natives and
    // logging — would run in its own pass immediately after this one; neither
    // node here needs it.
    void next_tick(std::uint64_t k) {
        output_pass();
        if (k % 5 == 0) {                                  // 200 Hz on 1 kHz base
            // The wire: one member read, no copy.
            fl_tc.next({fl_whl_out.ws}, fl_tc_next);
        }
    }

    // Both state kinds advance HERE, at the bottom of the tick — below the
    // recorder and below the termination exit — so every method above observed
    // the same t[k], x[k] and dis[k], and a run that stops is left holding a
    // consistent checkpoint (§9.6). The real emitter does one whole-model
    // `dis = nxt`; this file keeps a per-node buffer only because it predates
    // the discrete block.
    void advance(double t, double h) {
        fl_tc.state.cut = fl_tc_next.cut;                  // stands in for dis = nxt
        solver_step(t, h);                                 // rk4 over `x`, §9.5
    }
};

}  // namespace drivetrain
