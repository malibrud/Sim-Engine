// ─────────────────────────────────────────────────────────────────────────────
//  state_ref.hpp — FIXED ENGINE RUNTIME HEADER
//
//  Hand-written, ships with the engine, NEVER generated. It does not vary with
//  the model: the same header serves a 3-state model and a 30,000-state one.
//  The generator's only job is to emit `state_ref omega;` members and bind them
//  to slots in the constructor.
//
//  Purpose: a named view of one slot in the flat `double` state vector, so a
//  node body can write `state.omega` instead of `sim.x[7]`, while an external
//  solver (CVODE, odeint, a hand-rolled RK4) still sees nothing but `double*`.
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

namespace sim {

// Not a template, and not a `double&`. A reference member can be neither
// default-constructed nor rebound, but the generated State/Der structs must be
// default-constructible before Sim::bind_states() runs. Hence a pointer proxy.
class state_ref {
    double* slot_ = nullptr;

public:
    state_ref() = default;
    explicit state_ref(double& slot) : slot_(&slot) {}

    // Bound once, in the generated Sim constructor.
    void bind(double& slot) { slot_ = &slot; }

    // Read: behaves as a double, so every operator, every <cmath> function and
    // every third-party API accepts it with no interop work whatsoever.
    operator double() const { return *slot_; }

    // Write.
    state_ref& operator=(double v) { *slot_ = v; return *this; }
};

}  // namespace sim
