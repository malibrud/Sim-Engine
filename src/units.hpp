// ─────────────────────────────────────────────────────────────────────────────
//  Units.  SPECIFICATION.md §4 and Appendix A.
//
//  A unit carries three things, and the third is the one that is easy to
//  forget:
//
//    1. a DIMENSION — a rational exponent vector over the seven SI bases,
//       which is what compatibility is decided on (§4.2);
//    2. a SCALE and OFFSET to SI base, which is what conversion folds to a
//       compile-time constant;
//    3. a DISPLAY form — the factors as the author actually spelled them.
//
//  (3) exists because units are ERASED at the C++ boundary (I3), so the
//  manifest (§15.6) is load-bearing output rather than documentation. A
//  manifest that rendered `rad/s` from the dimension vector would print `1/s`
//  and throw away the one word a reader needs. Carrying the spelling also
//  makes the derived derivative unit come out right: `rad/s` divided by `s` is
//  `rad/s^2`, not `1/s^2`.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_UNITS_HPP
#define SE_UNITS_HPP

#include <string>
#include <string_view>
#include <vector>

#include "ast.hpp"
#include "diag.hpp"

namespace se {

// ─── Rational exponents ──────────────────────────────────────────────────────
//  §4.1 admits `m^(1/2)`, so an integer exponent vector is not enough.

struct Rat {
    long n = 0;
    long d = 1;

    bool zero() const { return n == 0; }
    bool operator==(const Rat& o) const { return n == o.n && d == o.d; }
    bool operator!=(const Rat& o) const { return !(*this == o); }
};

Rat rat(long n, long d = 1);          // normalising constructor
Rat rat_add(Rat a, Rat b);
Rat rat_sub(Rat a, Rat b);
Rat rat_mul(Rat a, Rat b);
Rat rat_neg(Rat a);
std::string rat_str(Rat r);           // "2", "-1", "(1/2)"

// ─── Units ───────────────────────────────────────────────────────────────────

constexpr int kBaseDims = 7;   // m, kg, s, A, K, mol, cd  — in that order

// One factor of the display form: the symbol as written, and its exponent.
struct UnitFactor {
    std::string sym;
    Rat exp;
};

struct Unit {
    Rat dim[kBaseDims];
    // value_in_SI = value * scale + offset
    double scale = 1.0;
    double offset = 0.0;
    bool has_offset = false;

    std::vector<UnitFactor> display;

    bool dimensionless() const;
    // The declared spelling: "rad/s", "kg*m^2", "1/(kg*m^2)", "-".
    std::string str() const;
    // Canonical SI rendering from the dimension vector, for `SE0410` — the one
    // place where showing what the author wrote would hide the disagreement.
    std::string dim_str() const;
};

bool compatible(const Unit& a, const Unit& b);

// Both directions of the affine map. `to_si` then `from_si` is how a value
// crosses between two compatible units.
double to_si(double v, const Unit& u);
double from_si(double v, const Unit& u);
// Convert `v` from `a` to `b`. The caller has already checked compatibility.
double convert(double v, const Unit& a, const Unit& b);

Unit unit_one();                                  // dimensionless, no factors
Unit unit_mul(const Unit& a, const Unit& b);
Unit unit_div(const Unit& a, const Unit& b);
Unit unit_pow(const Unit& a, Rat e);

// Appendix A. Exact match first, then longest prefix plus exact unit, so `min`
// is the minute and `Pa` is the pascal.
bool lookup_unit_symbol(std::string_view sym, Unit& out);
// Near matches for the `SE0414` diagnostic — `(sec)` should point at `(s)`.
std::vector<std::string> near_unit_symbols(std::string_view sym);

// ─── Evaluation of a parsed unit expression ──────────────────────────────────

class UnitEval {
public:
    UnitEval(const Source& src, Diagnostics& diag) : src_(src), diag_(diag) {}

    // A null expression is dimensionless (a `native` member, an untyped site).
    // Returns false having reported on `SE0414` / `SE0411`.
    bool eval(const ast::UnitExpr* u, Unit& out);

private:
    bool eval_inner(const ast::UnitExpr* u, Unit& out, bool compound);

    const Source& src_;
    Diagnostics& diag_;
};

}  // namespace se

#endif  // SE_UNITS_HPP
