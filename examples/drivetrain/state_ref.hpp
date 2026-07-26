// ─────────────────────────────────────────────────────────────────────────────
//  state_ref.hpp — FIXED ENGINE RUNTIME HEADER
//
//  Hand-written, ships with the engine, NEVER generated. It does not vary with
//  the model: the same header serves a 3-state model and a 30,000-state one.
//  The generator's only job is to emit `state_ref<rad/s> omega;` members and
//  bind them to slots in the constructor.
//
//  Purpose: a named, unit-typed view of one slot in the flat `double` state
//  vector. Units are semantic inside node bodies but stop at the vector
//  boundary, so an external solver (CVODE, odeint, a hand-rolled RK4) sees
//  nothing but `double*`.
//
//  Slot convention: the slot holds the value IN THE STATE'S DECLARED UNIT.
//  `omega (rad/s)` => the slot is rad/s. This is a contract with whoever
//  writes the external solver; the generator emits a slot map alongside the
//  model code.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <mp-units/framework.h>

namespace sim {

template <auto U>   // U is the declared unit reference, e.g. rad/s
class state_ref {
    double* slot_ = nullptr;

public:
    using quantity_type = mp_units::quantity<U, double>;

    state_ref() = default;
    explicit state_ref(double& slot) : slot_(&slot) {}

    // Bound once, in the generated Sim constructor.
    void bind(double& slot) { slot_ = &slot; }

    // Raw accessor — used by the interop trait below and by nothing else.
    double raw() const { return *slot_; }

    // Read: behaves as a quantity.
    operator quantity_type() const { return *slot_ * U; }

    // Write: unit-checked at the call site, stores the bare number.
    state_ref& operator=(quantity_type q) {
        *slot_ = q.numerical_value_in(U);
        return *this;
    }
};

}  // namespace sim

// ─────────────────────────────────────────────────────────────────────────────
//  The load-bearing part.
//
//  Without this specialization, `state.omega * param.radius` DOES NOT COMPILE.
//  mp-units' operator* is a template, and template argument deduction never
//  considers user-defined conversions — so the `operator quantity_type()`
//  above would simply never fire. The trait makes state_ref *deduce* as a
//  quantity, so every mp-units operator accepts it with no per-operator work.
//
//  CAVEAT — VERIFY BEFORE RELYING ON THIS. The exact spelling and semantics of
//  the interop customization point should be checked against the pinned
//  mp-units version; `quantity_like_traits` is the expected hook but has not
//  been confirmed against the library. If it does not behave as assumed, the
//  fallback is to hand-write the operator set in this same header — `* / + -`,
//  comparisons, unary minus, both argument orders, roughly 20 overloads. Still
//  written once, still fixed. Annoying, not structural.
// ─────────────────────────────────────────────────────────────────────────────
template <auto U>
struct mp_units::quantity_like_traits<sim::state_ref<U>> {
    static constexpr auto reference = U;
    using rep = double;
    static constexpr rep to_numerical_value(sim::state_ref<U> s) { return s.raw(); }
};
