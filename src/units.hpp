// ─────────────────────────────────────────────────────────────────────────────
//  Units.  SPECIFICATION.md §4 and Appendix A.
//
//  A unit carries three things, and the third is the one that is easy to
//  forget:
//
//    1. a DIMENSION — a rational exponent vector over the seven SI bases plus
//       ANGLE (§4.5), which is what compatibility is decided on (§4.2);
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

// m, kg, s, A, K, mol, cd, rad — in that order. `rad` is the angle base
// dimension of §4.5; it is last so the seven SI slots keep their indices.
constexpr int kBaseDims = 8;

// One factor of the display form: the symbol as written, and its exponent.
// `is_param` marks a unit-parameter placeholder rather than an Appendix A
// symbol, so binding can strip it without guessing from the spelling.
struct UnitFactor {
    std::string sym;
    Rat exp;
    bool is_param = false;
};

struct Unit {
    Rat dim[kBaseDims];
    // value_in_SI = value * scale + offset
    double scale = 1.0;
    double offset = 0.0;
    bool has_offset = false;
    // Any factor is an angle unit other than `rad`/`sr` — `deg`, `rev`, `rpm`,
    // or a prefixed form. Tracked structurally rather than inferred from the
    // scale, which cannot be separated from the non-angle factors' scales.
    // §4.5 permits these as literal suffixes and display units only; a declared
    // unit carrying one is `SE0415`.
    bool nonradian_angle = false;
    // §6.2a — exponents on the enclosing node's unit parameters, by parameter
    // index. Empty for every non-parametric unit, which is almost all of them,
    // so the existing paths are untouched. `(U*s)` is {1} with `s` in `dim`;
    // `(U/V)` is {1, -1}. A unit with any nonzero entry is not yet a number:
    // it becomes one at instantiation, via `unit_bind`.
    std::vector<Rat> param_exp;

    std::vector<UnitFactor> display;

    bool parametric() const;
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
Unit unit_rad();                                  // the angle base, spelled `rad`
// A bare reference to unit parameter `index`, spelled `name`.
Unit unit_param(std::size_t index, const std::string& name);
// Substitute concrete units for the parameters, yielding a unit with no
// `param_exp` left. `binding` is indexed the way `param_exp` is; a parameter
// referenced beyond its end is a caller bug, not a user error.
Unit unit_bind(const Unit& u, const std::vector<Unit>& binding);
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
    // The enclosing node's unit parameters, in declaration order — the indices
    // `Unit::param_exp` is keyed by. Empty for every non-parametric node.
    UnitEval(const Source& src, Diagnostics& diag, std::vector<std::string> params)
        : src_(src), diag_(diag), params_(std::move(params)) {}

    // A null expression is dimensionless (a `native` member, an untyped site).
    // Returns false having reported on `SE0414` / `SE0411`.
    bool eval(const ast::UnitExpr* u, Unit& out);

private:
    bool eval_inner(const ast::UnitExpr* u, Unit& out, bool compound);

    const Source& src_;
    Diagnostics& diag_;
    std::vector<std::string> params_;
};

}  // namespace se

#endif  // SE_UNITS_HPP
