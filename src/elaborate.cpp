// ─────────────────────────────────────────────────────────────────────────────
//  Stage 4 — elaboration.  SPECIFICATION.md §7, §9.2, §13, §14.
// ─────────────────────────────────────────────────────────────────────────────
#include "elaborate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

#include "dump.hpp"
#include "parser.hpp"

namespace se {
namespace {

std::string sub(const std::string& path, const std::string& name) {
    return path.empty() ? name : path + "." + name;
}

// §6.4a — `w1[3]` -> `w1` and 3. The index rides on the last path segment
// (§13.4, §13.5), so every matcher downstream keeps working on one string
// and only the two sites that care about elements have to look inside it.
bool split_index(const std::string& seg, std::string& base, std::size_t& index) {
    if (seg.empty() || seg.back() != ']') return false;
    const std::size_t open = seg.find('[');
    if (open == std::string::npos) return false;
    const std::string digits = seg.substr(open + 1, seg.size() - open - 2);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos)
        return false;
    base = seg.substr(0, open);
    index = static_cast<std::size_t>(std::strtoul(digits.c_str(), nullptr, 10));
    return true;
}

std::string ident_of(const std::string& path) {
    std::string s = path;
    for (char& c : s)
        if (c == '.') c = '_';
    return s;
}

// A rational approximation with a small denominator, for `^` exponents (§7.2).
bool as_rational(double x, Rat& out) {
    for (long d = 1; d <= 12; ++d) {
        const double n = x * static_cast<double>(d);
        const double r = std::floor(n + 0.5);
        if (std::fabs(n - r) < 1e-9) {
            out = rat(static_cast<long>(r), d);
            return true;
        }
    }
    return false;
}

// Levenshtein, only ever run on a path that already failed to resolve.
int path_distance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({cur[j - 1] + 1, prev[j] + 1,
                               prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        prev = cur;
    }
    return prev[b.size()];
}

std::string join_file(const std::string& dir, const std::string& rel) {
    if (dir.empty()) return rel;
    if (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) return rel;
    if (rel.size() > 1 && rel[1] == ':') return rel;   // absolute on Windows
    return dir + "/" + rel;
}

const char* const kSolvers[] = {"euler", "rk2", "rk4"};
const char* const kLevels[] = {"trace", "debug", "info", "warn", "error"};

bool one_of(const std::string& s, const char* const* set, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (s == set[i]) return true;
    return false;
}

}  // namespace

Elaborator::Elaborator(Diagnostics& diag, Resolver& resolver)
    : diag_(diag), resolver_(resolver) {}

// ═════════════════════════════════════════════════════════════════════════════
//  The §7 expression language
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// §15.5 — a double as a C++ literal that reads back bit-identical. The settings
// program is generated source, so a lossy rendering here would be a silent
// numerical difference between what `sec` computed and what the binary runs.
std::string cpp_number(double v) {
    if (std::isinf(v)) return v < 0 ? "-HUGE_VAL" : "HUGE_VAL";
    std::ostringstream os;
    os << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    std::string s = os.str();
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

ExprCode ec_lit(std::string s) {
    ExprCode c;
    ExprTok t;
    t.kind = ExprTok::Kind::Text;
    t.text = std::move(s);
    c.push_back(std::move(t));
    return c;
}

ExprCode ec_num(double v) { return ec_lit(cpp_number(v)); }

ExprCode ec_param(std::string owner, std::string name) {
    ExprCode c;
    ExprTok t;
    t.kind = ExprTok::Kind::Param;
    t.owner = std::move(owner);
    t.text = std::move(name);
    c.push_back(std::move(t));
    return c;
}

// Whether the code can be used as an operand without parentheses. A single
// reference or a single literal can; anything already carrying an operator
// cannot, and rather than track precedence we parenthesise. The generated
// program is read by people, but it is not hand-maintained, so a few redundant
// parentheses cost less than a precedence bug would.
bool ec_atomic(const ExprCode& c) {
    if (c.size() != 1) return false;
    if (c[0].kind == ExprTok::Kind::Param) return true;
    const std::string& s = c[0].text;
    return !s.empty() && s.find_first_of("+-*/ ", 1) == std::string::npos;
}

ExprCode ec_atom(ExprCode c) {
    if (ec_atomic(c)) return c;
    ExprCode r = ec_lit("(");
    r.insert(r.end(), c.begin(), c.end());
    r.push_back(ec_lit(")")[0]);
    return r;
}

// a <op> b, each operand parenthesised only if it needs to be.
ExprCode ec_bin(const ExprCode& a, const char* op, const ExprCode& b) {
    ExprCode r = ec_atom(a);
    r.push_back(ec_lit(std::string(" ") + op + " ")[0]);
    ExprCode rb = ec_atom(b);
    r.insert(r.end(), rb.begin(), rb.end());
    return r;
}

// fn(a, b) — arguments are already delimited, so they need no parentheses.
ExprCode ec_call(const char* fn, const std::vector<ExprCode>& args) {
    ExprCode r = ec_lit(std::string(fn) + "(");
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) r.push_back(ec_lit(", ")[0]);
        r.insert(r.end(), args[i].begin(), args[i].end());
    }
    r.push_back(ec_lit(")")[0]);
    return r;
}

// The affine conversion `convert()` performs, applied to an expression instead
// of to a number (§4.2 units carry a scale and an offset). Emitted only when it
// would change the value, so an in-unit expression stays untouched — which is
// why the common case generates no conversion arithmetic at all.
ExprCode ec_convert(const ExprCode& c, const Unit& from, const Unit& to) {
    if (from.scale == to.scale && from.offset == to.offset) return c;
    ExprCode r = c;
    if (from.scale != 1.0) r = ec_bin(r, "*", ec_num(from.scale));
    if (from.offset != 0.0) r = ec_bin(r, "+", ec_num(from.offset));
    if (to.offset != 0.0) r = ec_bin(r, "-", ec_num(to.offset));
    if (to.scale != 1.0) r = ec_bin(r, "/", ec_num(to.scale));
    return r;
}

}  // namespace

bool Elaborator::eval(const ast::Expr* e, const Source& src, const Env& env, Value& out) {
    if (!e) return false;
    using K = ast::Expr::Kind;

    switch (e->kind) {
        case K::Number: {
            out.v = e->number;
            out.bare = (e->unit == nullptr);
            out.code = ec_num(e->number);
            out.constant = true;
            UnitEval ue(src, diag_);
            return ue.eval(e->unit.get(), out.unit);
        }

        case K::Param: {
            auto it = env.find(e->text);
            if (it == env.end()) {
                std::vector<Attachment> att;
                att.push_back(note("a setting expression may reference other settings "
                                   "OF THE SAME NODE, through `param.` (§6.2)"));
                if (!env.empty()) {
                    std::string have = "in scope here: ";
                    bool first = true;
                    for (const auto& kv : env) {
                        if (!first) have += ", ";
                        have += "`" + kv.first + "`";
                        first = false;
                    }
                    att.push_back(help(have));
                }
                diag_.error("SE0304", src, e->loc,
                            "no setting named `" + e->text + "` here", "unresolved",
                            std::move(att));
                return false;
            }
            // `cpp` and `constant` ride along from the environment: the binding
            // that put this name in scope already knows whether it is a literal
            // or an lvalue a configuration override can move (§6.2b).
            out = it->second;
            out.bare = false;
            return true;
        }

        case K::Name: {
            if (e->text == "pi") {
                out = Value{3.14159265358979323846, unit_one(), true};
                out.code = ec_num(out.v);
                return true;
            }
            if (e->text == "e") {
                out = Value{2.71828182845904523536, unit_one(), true};
                out.code = ec_num(out.v);
                return true;
            }
            if (e->text == "inf") {
                out = Value{HUGE_VAL, unit_one(), true};
                out.code = ec_lit("HUGE_VAL");
                return true;
            }
            // §10.4 — the node's own effective sample rate, resolved at
            // elaboration: its declared `rate` if it has one, the base step
            // otherwise. NOT runtime ambient context (§10.2) — these are
            // compile-time constants, which is why they carry none of the
            // "which fields are live when" baggage that closed `sim.*`.
            //
            // Both spellings ship because coefficient design wants a rate
            // (`K = 2 * sample_rate`) while anything expressed as a period
            // wants the reciprocal; deriving both from one number means they
            // cannot disagree.
            if (e->text == "sample_rate" || e->text == "time_step") {
                if (effective_rate_ <= 0.0) {
                    diag_.error("SE0413", src, e->loc,
                                "`" + e->text + "` has no value here",
                                "no sample rate in scope",
                                {note("it resolves to the node's `rate`, or the base "
                                      "step when the node declares none — neither is "
                                      "known outside a node's settings (§10.4)")});
                    return false;
                }
                Unit u;
                lookup_unit_symbol(e->text == "sample_rate" ? "Hz" : "s", u);
                out = Value{e->text == "sample_rate" ? effective_rate_
                                                     : 1.0 / effective_rate_,
                            u, false};
                // Both spellings come off one number, so they cannot disagree
                // after an override any more than they can before one.
                out.code = e->text == "sample_rate"
                               ? effective_rate_code_
                               : ec_bin(ec_num(1.0), "/", effective_rate_code_);
                out.constant = effective_rate_code_.size() == 1 &&
                               effective_rate_code_[0].kind == ExprTok::Kind::Text;
                return true;
            }
            // The grammar has no other way to spell a bare name here, so this
            // is where `mass = fl.mass` lands.
            diag_.error("SE0440", src, e->loc,
                        "`" + e->text + "` is not a constant or a setting of this node",
                        "unresolved name",
                        {note("flow-down goes strictly parent -> child: there are no "
                              "cross-instance references in a setting binding (§6.9.1)"),
                         help("the constants are `pi`, `e` and `inf`; other settings of "
                              "this node are reached as `param." + e->text + "`")});
            return false;
        }

        case K::Unary: {
            if (!eval(e->lhs.get(), src, env, out)) return false;
            if (e->op == '-') {
                out.v = -out.v;
                ExprCode neg = ec_lit("-");
                ExprCode a = ec_atom(out.code);
                neg.insert(neg.end(), a.begin(), a.end());
                out.code = std::move(neg);
            }
            return true;
        }

        case K::Binary: {
            Value a, b;
            if (!eval(e->lhs.get(), src, env, a)) return false;
            if (!eval(e->rhs.get(), src, env, b)) return false;

            if (e->op == '^') {
                if (!b.unit.dimensionless()) {
                    diag_.error("SE0412", src, e->rhs->loc,
                                "the exponent of `^` must be dimensionless",
                                "`(" + b.unit.str() + ")`");
                    return false;
                }
                // §6.2b — an exponent that a configuration override could move
                // would move the DIMENSION of the result with it, and dimensions
                // are shape. §7.2 already asks for a literal here; this is where
                // that requirement becomes load-bearing rather than stylistic.
                if (!b.constant) {
                    diag_.error("SE0412", src, e->rhs->loc,
                                "the exponent of `^` must not depend on a setting",
                                "this can change after the binary is built",
                                {note("`^` multiplies the exponent vector, so an "
                                      "overridable exponent would change the result's "
                                      "dimension at configuration (§6.2b)"),
                                 help("settings are values, not shape (§6.2) — write the "
                                      "exponent as a literal")});
                    return false;
                }
                const double exp = to_si(b.v, b.unit);
                Rat r;
                if (as_rational(exp, r)) {
                    // A value `v` in unit `u` is `v * u.scale` in SI, so
                    // raising it leaves the number alone and raises the unit.
                    out.unit = unit_pow(a.unit, r);
                    out.v = std::pow(a.v, exp);
                    out.code = ec_call("std::pow", {a.code, ec_num(exp)});
                } else if (a.unit.dimensionless()) {
                    out.unit = unit_one();
                    out.v = std::pow(to_si(a.v, a.unit), exp);
                    out.code = ec_call(
                        "std::pow", {ec_convert(a.code, a.unit, unit_one()), ec_num(exp)});
                } else {
                    diag_.error("SE0412", src, e->rhs->loc,
                                "a dimensioned base needs an integer or rational exponent",
                                "not a simple rational",
                                {note("`^` multiplies the exponent vector, so the result "
                                      "must still have rational dimensions (§7.2)")});
                    return false;
                }
                out.bare = a.bare && b.bare;
                out.constant = a.constant;   // b is constant, checked above
                break;
            }

            if (e->op == '*' || e->op == '/') {
                if (e->op == '/' && b.v == 0.0) {
                    diag_.error("SE0413", src, e->loc, "division by zero at elaboration",
                                "the divisor is zero",
                                {note("the expression language is total by rejection, "
                                      "not by producing NaN (§7.3)")});
                    return false;
                }
                out.v = (e->op == '*') ? a.v * b.v : a.v / b.v;
                out.unit = (e->op == '*') ? unit_mul(a.unit, b.unit)
                                          : unit_div(a.unit, b.unit);
                out.bare = a.bare && b.bare;
                out.code = ec_bin(a.code, e->op == '*' ? "*" : "/", b.code);
                out.constant = a.constant && b.constant;
                break;
            }

            // `+` and `-` require compatible dimensions and convert the right
            // operand to the left's unit. A bare literal on either side adopts
            // the other's unit, which is the same rule as at a declarative site.
            if (a.bare && !b.bare) a.unit = b.unit;
            if (b.bare && !a.bare) b.unit = a.unit;
            if (!compatible(a.unit, b.unit)) {
                diag_.error("SE0410", src, e->loc,
                            "incompatible units in `" + std::string(1, e->op) + "`",
                            "`(" + a.unit.str() + ")` and `(" + b.unit.str() + ")`",
                            {note("left is " + a.unit.dim_str()),
                             note("right is " + b.unit.dim_str())});
                return false;
            }
            const double rhs = convert(b.v, b.unit, a.unit);
            out.v = (e->op == '+') ? a.v + rhs : a.v - rhs;
            out.unit = a.unit;
            out.bare = a.bare && b.bare;
            // The conversion the line above performs on the number, performed on
            // the expression — the two operands may be in different units and
            // only the left one's is kept.
            out.code = ec_bin(a.code, e->op == '+' ? "+" : "-",
                              ec_convert(b.code, b.unit, a.unit));
            out.constant = a.constant && b.constant;
            break;
        }

        case K::Call:
            if (!eval_call(e, src, env, out)) return false;
            break;
    }

    if (!std::isfinite(out.v) && !(e->kind == K::Name && e->text == "inf")) {
        diag_.error("SE0413", src, e->loc, "elaboration produced a non-finite value",
                    "not finite",
                    {note("division by zero, a domain error, or an overflow (§7.3)")});
        return false;
    }
    return true;
}

bool Elaborator::eval_call(const ast::Expr* e, const Source& src, const Env& env,
                           Value& out) {
    const std::string& f = e->text;
    std::vector<Value> a;
    for (const ast::ExprPtr& arg : e->args) {
        Value v;
        if (!eval(arg.get(), src, env, v)) return false;
        a.push_back(v);
    }

    auto arity = [&](std::size_t n) {
        if (a.size() == n) return true;
        diag_.error("SE0412", src, e->loc,
                    "`" + f + "` takes " + std::to_string(n) + " argument" +
                        (n == 1 ? "" : "s"),
                    "given " + std::to_string(a.size()));
        return false;
    };
    auto want_dimensionless = [&](std::size_t i) {
        if (a[i].unit.dimensionless()) return true;
        diag_.error("SE0412", src, e->args[i]->loc,
                    "`" + f + "` requires a dimensionless argument",
                    "`(" + a[i].unit.str() + ")`",
                    {note("`asin`/`acos`/`atan` take a ratio and RETURN an angle; "
                          "`log*` and `exp` are dimensionless in and out (§7.3)")});
        return false;
    };
    // §4.5 made angle a base dimension, so `sin` now takes an angle rather than
    // a bare ratio. A bare value is still accepted and read as radians — `pi/4`
    // is the canonical argument and carries no unit of its own.
    auto want_angle = [&](std::size_t i) {
        if (a[i].bare || compatible(a[i].unit, unit_rad())) return true;
        diag_.error("SE0412", src, e->args[i]->loc,
                    "`" + f + "` requires an angle",
                    "`(" + a[i].unit.str() + ")`",
                    {note("the argument is " + a[i].unit.dim_str()),
                     help("angle is a base dimension (§4.5) — write `(rad)`, or a "
                          "`(deg)` literal, which converts here")});
        return false;
    };
    // Two arguments that must agree; the right is converted to the left's unit.
    auto align = [&]() {
        if (a[0].bare && !a[1].bare) a[0].unit = a[1].unit;
        if (a[1].bare && !a[0].bare) a[1].unit = a[0].unit;
        if (!compatible(a[0].unit, a[1].unit)) {
            diag_.error("SE0412", src, e->loc,
                        "`" + f + "` requires compatible arguments",
                        "`(" + a[0].unit.str() + ")` and `(" + a[1].unit.str() + ")`",
                        {note("left is " + a[0].unit.dim_str()),
                         note("right is " + a[1].unit.dim_str())});
            return false;
        }
        a[1].v = convert(a[1].v, a[1].unit, a[0].unit);
        a[1].code = ec_convert(a[1].code, a[1].unit, a[0].unit);
        a[1].unit = a[0].unit;
        return true;
    };

    out.bare = true;
    out.constant = true;
    for (const Value& v : a) {
        out.bare = out.bare && v.bare;
        out.constant = out.constant && v.constant;
    }
    out.unit = unit_one();

    // An argument that a builtin consumes in SI — `sin` wants radians whatever
    // the caller wrote, `log` wants a pure ratio. Mirrors the `to_si` the value
    // path applies a few lines below each use.
    auto si = [&](std::size_t i) { return ec_convert(a[i].code, a[i].unit, unit_one()); };
    auto code1 = [&](const char* fn) { out.code = ec_call(fn, {a[0].code}); };
    auto code1_si = [&](const char* fn) { out.code = ec_call(fn, {si(0)}); };
    auto code2 = [&](const char* fn) { out.code = ec_call(fn, {a[0].code, a[1].code}); };

    if (f == "abs" || f == "floor" || f == "ceil" || f == "round") {
        if (!arity(1)) return false;
        out.unit = a[0].unit;
        out.v = f == "abs"     ? std::fabs(a[0].v)
                : f == "floor" ? std::floor(a[0].v)
                : f == "ceil"  ? std::ceil(a[0].v)
                               : std::floor(a[0].v + 0.5);
        if (f == "abs") code1("std::fabs");
        else if (f == "floor") code1("std::floor");
        else if (f == "ceil") code1("std::ceil");
        else out.code = ec_call("std::floor", {ec_bin(a[0].code, "+", ec_num(0.5))});
        return true;
    }
    if (f == "sign") {
        if (!arity(1)) return false;
        out.v = a[0].v > 0 ? 1.0 : (a[0].v < 0 ? -1.0 : 0.0);
        out.bare = true;
        // No `<cmath>` equivalent, so it emits as the conditional it is. This is
        // generated code, not §7 source: §7.4's ban on conditionals constrains
        // what a user may write, not what the emitter may produce for it.
        out.code = ec_atom(ec_bin(ec_bin(a[0].code, ">", ec_num(0.0)), "? 1.0 :",
                                  ec_atom(ec_bin(ec_bin(a[0].code, "<", ec_num(0.0)),
                                                 "? -1.0 :", ec_num(0.0)))));
        return true;
    }
    if (f == "min" || f == "max" || f == "mod" || f == "hypot") {
        if (!arity(2) || !align()) return false;
        out.unit = a[0].unit;
        out.v = f == "min"     ? std::min(a[0].v, a[1].v)
                : f == "max"   ? std::max(a[0].v, a[1].v)
                : f == "hypot" ? std::hypot(a[0].v, a[1].v)
                               : std::fmod(a[0].v, a[1].v);
        if (f == "mod" && a[1].v == 0.0) {
            diag_.error("SE0413", src, e->loc, "`mod` by zero at elaboration", "");
            return false;
        }
        code2(f == "min"     ? "std::min"
              : f == "max"   ? "std::max"
              : f == "hypot" ? "std::hypot"
                             : "std::fmod");
        return true;
    }
    if (f == "atan2") {
        if (!arity(2) || !align()) return false;
        // The arguments' shared unit cancels in the ratio; the result is an angle.
        out.v = std::atan2(a[0].v, a[1].v);
        out.unit = unit_rad();
        out.bare = false;
        code2("std::atan2");
        return true;
    }
    if (f == "sqrt" || f == "cbrt") {
        if (!arity(1)) return false;
        if (a[0].v < 0.0) {
            diag_.error("SE0413", src, e->loc, "`" + f + "` of a negative value", "");
            return false;
        }
        const Rat r = (f == "sqrt") ? rat(1, 2) : rat(1, 3);
        out.unit = unit_pow(a[0].unit, r);
        out.v = (f == "sqrt") ? std::sqrt(a[0].v) : std::cbrt(a[0].v);
        code1(f == "sqrt" ? "std::sqrt" : "std::cbrt");
        return true;
    }
    if (f == "pow") {
        if (!arity(2) || !want_dimensionless(1)) return false;
        // Same rule as `^`: the exponent decides the result's dimension, and a
        // dimension is shape, so it may not move at configuration (§6.2b).
        if (!a[1].constant) {
            diag_.error("SE0412", src, e->args[1]->loc,
                        "the exponent of `pow` must not depend on a setting",
                        "this can change after the binary is built",
                        {note("it multiplies the exponent vector, so an overridable "
                              "exponent would change the result's dimension (§6.2b)"),
                         help("settings are values, not shape (§6.2)")});
            return false;
        }
        const double exp = to_si(a[1].v, a[1].unit);
        Rat r;
        if (as_rational(exp, r)) {
            out.unit = unit_pow(a[0].unit, r);
            out.v = std::pow(a[0].v, exp);
            out.code = ec_call("std::pow", {a[0].code, ec_num(exp)});
        } else if (a[0].unit.dimensionless()) {
            out.v = std::pow(to_si(a[0].v, a[0].unit), exp);
            out.code = ec_call("std::pow", {si(0), ec_num(exp)});
        } else {
            diag_.error("SE0412", src, e->args[1]->loc,
                        "a dimensioned base needs an integer or rational exponent",
                        "not a simple rational");
            return false;
        }
        return true;
    }
    if (f == "exp" || f == "log" || f == "log2" || f == "log10") {
        if (!arity(1) || !want_dimensionless(0)) return false;
        const double x = to_si(a[0].v, a[0].unit);
        if (f != "exp" && x <= 0.0) {
            diag_.error("SE0413", src, e->loc, "`" + f + "` of a non-positive value", "");
            return false;
        }
        out.v = f == "exp"    ? std::exp(x)
                : f == "log"  ? std::log(x)
                : f == "log2" ? std::log2(x)
                              : std::log10(x);
        out.bare = true;
        code1_si(f == "exp" ? "std::exp" : f == "log" ? "std::log"
                          : f == "log2" ? "std::log2"
                                        : "std::log10");
        return true;
    }
    // Angle in, ratio out.
    if (f == "sin" || f == "cos" || f == "tan") {
        if (!arity(1) || !want_angle(0)) return false;
        const double x = to_si(a[0].v, a[0].unit);
        out.v = f == "sin" ? std::sin(x) : f == "cos" ? std::cos(x) : std::tan(x);
        out.bare = true;
        code1_si(f == "sin" ? "std::sin" : f == "cos" ? "std::cos" : "std::tan");
        return true;
    }
    // Ratio in, angle out. `to_si` is a no-op on a dimensionless argument, and
    // `std::asin` already returns radians, so the value needs no conversion —
    // only the unit is attached.
    if (f == "asin" || f == "acos" || f == "atan") {
        if (!arity(1) || !want_dimensionless(0)) return false;
        const double x = to_si(a[0].v, a[0].unit);
        if ((f == "asin" || f == "acos") && (x < -1.0 || x > 1.0)) {
            diag_.error("SE0413", src, e->loc, "`" + f + "` argument outside [-1, 1]", "");
            return false;
        }
        out.v = f == "asin" ? std::asin(x) : f == "acos" ? std::acos(x) : std::atan(x);
        out.unit = unit_rad();
        out.bare = false;
        code1_si(f == "asin" ? "std::asin" : f == "acos" ? "std::acos" : "std::atan");
        return true;
    }

    diag_.error("SE0304", src, e->loc, "unknown builtin function `" + f + "`",
                "not a builtin",
                {note("the set is fixed and small so that elaboration is guaranteed to "
                      "terminate (§7.3, §7.4)")});
    return false;
}

bool Elaborator::coerce(const Value& v, const Type& target, const Source& src, Loc loc,
                        const char* what, double& out, ExprCode* out_code) {
    // §6.2c — a record target never reaches here: `destructure` splits it into
    // leaves first, and each leaf is a scalar. This stays as a backstop, so a
    // new caller that forgets to destructure fails loudly rather than emitting
    // a struct assignment from a `double`.
    if (target.is_record) {
        diag_.error("SE0426", src, loc,
                    std::string(what) + " is a record and cannot take a single value",
                    "expected a brace list",
                    {note("a record setting is written as a brace list, or as another "
                          "setting of the same record type (\xc2\xa7""6.2c)")});
        return false;
    }
    if (v.bare) {
        // §7.2 — a bare literal takes the unit of the site it initialises, so
        // there is nothing to convert on either the number or the expression.
        out = v.v;
        if (out_code) *out_code = v.code;
    } else {
        if (!compatible(v.unit, target.unit)) {
            diag_.error("SE0410", src, loc,
                        std::string("incompatible units for ") + what,
                        "`(" + v.unit.str() + ")` where `(" + target.unit.str() +
                            ")` is declared",
                        {note("value is " + v.unit.dim_str()),
                         note("target is " + target.unit.dim_str())});
            return false;
        }
        out = convert(v.v, v.unit, target.unit);
        if (out_code) *out_code = ec_convert(v.code, v.unit, target.unit);
    }
    if (target.scalar == "int") {
        const double r = std::floor(out + 0.5);
        if (std::fabs(out - r) > 1e-9) {
            diag_.error("SE0402", src, loc,
                        "lossy conversion to an `int` target",
                        "the value is " + std::to_string(out),
                        {note("a literal has no inherent type: it is converted to the "
                              "declared type of the site, and rejected if that is lossy "
                              "(§3.5)")});
            return false;
        }
        out = r;
        // The check above covers the elaborated value; an override reaching an
        // `int` target at configuration has not been through it, so the emitted
        // form rounds rather than letting C++ truncate toward zero. A constant
        // expression is replaced by its literal downstream and needs no rounding.
        if (out_code && !v.constant)
            *out_code = ec_call("std::floor", {ec_bin(*out_code, "+", ec_num(0.5))});
    }
    return true;
}

// §6.4a — an extent. Two things have to be true of it, and they settle at
// different times.
//
// The DIMENSION settles here: an extent counts slots, so it is dimensionless,
// and no override can make `[param.mass]` mean anything. That is SE0432, at
// elaboration, once.
//
// The VALUE does not. `[param.nbq]` is an ordinary §7 expression over ordinary
// overridable settings (§6.2b step 4), so the number elaboration computes is a
// default and not the size anything will be. It is checked here only when the
// expression is constant — nothing upstream can move it, so a bad value is
// bad for good — and otherwise carried into `Sim::configure()`, which rounds
// and rejects with the same rule against whatever a §14 source supplied.
bool Elaborator::extent_of(const ast::Expr* e, Loc loc, const Source& src, const Env& env,
                           const std::string& what, Extent& out) {
    Value v;
    if (!eval(e, src, env, v)) return false;

    double n = v.v;
    if (!v.bare) {
        if (!compatible(v.unit, unit_one())) {
            diag_.error("SE0432", src, loc,
                        "the extent of `" + what + "` must be dimensionless",
                        "`(" + v.unit.str() + ")` is " + v.unit.dim_str(),
                        {note("an extent counts slots; it is not a quantity (§6.4a)")});
            failed_ = true;
            return false;
        }
        n = convert(v.v, v.unit, unit_one());
    }

    out.present = true;
    out.dflt = n;
    if (loc.valid() && loc.length > 0 &&
        loc.offset + loc.length <= src.text().size())
        out.text = src.text().substr(loc.offset, loc.length);

    if (v.constant) {
        const double r = std::floor(n + 0.5);
        if (!(n >= 0.0) || std::fabs(n - r) > 1e-9) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%g", n);
            diag_.error("SE0432", src, loc,
                        "the extent of `" + what +
                            "` must be a non-negative integer",
                        std::string("this is ") + buf,
                        {note("zero is legal and yields no slots (§6.4a)")});
            failed_ = true;
            return false;
        }
        out.dflt = r;
        out.expr = ec_num(r);
    } else {
        out.expr = v.bare ? v.code : ec_convert(v.code, v.unit, unit_one());
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  The sim file
// ═════════════════════════════════════════════════════════════════════════════

bool Elaborator::time_value(const ast::Expr* e, const Source& src, const char* key,
                            double& out) {
    Value v;
    if (!eval(e, src, Env{}, v)) return false;
    Unit seconds;
    lookup_unit_symbol("s", seconds);
    if (v.bare) {
        diag_.error("SE0410", src, e->loc,
                    std::string("`") + key + ":` needs a unit",
                    "no unit given",
                    {help(std::string("write `") + key + ": 1 (ms);` — there is no "
                          "implicit time unit")});
        return false;
    }
    if (!compatible(v.unit, seconds)) {
        diag_.error("SE0410", src, e->loc,
                    std::string("`") + key + ":` must be a time",
                    "`(" + v.unit.str() + ")` is " + v.unit.dim_str());
        return false;
    }
    out = to_si(v.v, v.unit);
    return true;
}

bool Elaborator::load_settings_file(const std::string& path, const Source& from, Loc loc) {
    auto src = std::make_unique<Source>();
    std::string error;
    if (!Source::load(path, *src, error)) {
        diag_.error("SE0304", from, loc, "cannot read settings source `" + path + "`",
                    error);
        return false;
    }
    Parser parser(*src, diag_);
    auto ast = parser.parse_settings_file();
    for (const ast::Override& o : ast->overrides) {
        OverrideEntry e;
        e.path = o.path.str();
        e.value = o.value.get();
        e.src = src.get();
        e.loc = o.loc;
        overrides_.push_back(e);
    }
    settings_srcs_.push_back(std::move(src));
    settings_asts_.push_back(std::move(ast));
    return true;
}

bool Elaborator::read_sim_entries(const ast::SimFile& sim, const Source& src, Model& m) {
    bool have_step = false, have_duration = false, have_root = false, have_sync = false;
    bool have_window = false;
    Loc sync_loc, window_loc;

    for (const ast::SimEntry& e : sim.entries) {
        if (e.key == "step") have_step = time_value(e.value.get(), src, "step", m.step);
        else if (e.key == "duration")
            have_duration = time_value(e.value.get(), src, "duration", m.duration);
        else if (e.key == "sync") {
            have_sync = time_value(e.value.get(), src, "sync", m.sync);
            sync_loc = e.loc;
        } else if (e.key == "window") {
            have_window = time_value(e.value.get(), src, "window", m.window);
            window_loc = e.loc;
        } else if (e.key == "root")
            have_root = true;
        else if (e.key == "solver") {
            m.solver = e.word;
            if (!one_of(e.word, kSolvers, 3))
                diag_.error("SE0264", src, e.loc, "unknown solver `" + e.word + "`",
                            "not a solver",
                            {note("the solver is global, not per-node: `euler`, `rk2` "
                                  "or `rk4` (§9.5)")});
        } else if (e.key == "mode") {
            if (e.word == "realtime") m.realtime = true;
            else if (e.word != "batch")
                diag_.error("SE0264", src, e.loc, "unknown mode `" + e.word + "`",
                            "not a mode", {note("`batch` or `realtime` (§13.1)")});
        }
    }

    if (!have_root)
        diag_.error("SE0262", src, sim.loc, "`sim` has no `root:` entry", "required here",
                    {note("the `root` entry is what makes a model runnable (§13.1)")});
    if (!have_step)
        diag_.error("SE0262", src, sim.loc, "`sim` has no `step:` entry", "required here",
                    {note("one global fixed base step; every rate is an integer "
                          "decimation of it (§9.1)")});
    if (!have_duration)
        diag_.error("SE0262", src, sim.loc, "`sim` has no `duration:` entry",
                    "required here");

    if (m.realtime && !have_sync)
        diag_.error("SE0262", src, sim.loc, "`mode: realtime` requires a `sync:` entry",
                    "required here",
                    {note("the frame is the third integer quantity alongside the base "
                          "step and the rate decimations (§9.4)")});
    if (!m.realtime && have_sync)
        diag_.error("SE0263", src, sync_loc, "`sync:` requires `mode: realtime`",
                    "no frame to synchronise to");

    // §13.6 — `window` scales real-time instrumentation and nothing else, so in
    // a batch run it would be read and then ignored. Saying so beats leaving a
    // recorded `sim.utilization` column of zeros to be puzzled over.
    if (!m.realtime && have_window)
        diag_.error("SE0263", src, window_loc, "`window:` requires `mode: realtime`",
                    "no frames to average over",
                    {note("`window` sets the rolling window for `sim.utilization` and "
                          "`sim.overrun_pct`, which only exist in real time (§13.6)"),
                     note("a batch run has no frame boundaries, so both metrics would "
                          "record as zero")});

    // §9.4 — the same error class as a non-divisor rate.
    if (have_sync && have_step && m.step > 0.0) {
        const double ratio = m.sync / m.step;
        const double r = std::floor(ratio + 0.5);
        if (std::fabs(ratio - r) > 1e-9 || r < 1.0) {
            diag_.error("SE0451", src, sync_loc,
                        "`sync` is not an integer multiple of `step`",
                        "sync/step = " + std::to_string(ratio),
                        {note("the frame free-runs a whole number of base steps, then "
                              "spins to the boundary (§9.4)")});
        } else {
            m.sync_steps = static_cast<long>(r);
        }
    }

    // §13.6 — the window is a whole number of frames, and rounding to one is
    // honest. Rounding to ZERO is not: the metrics would then report the latest
    // frame rather than a rolling average, which is a different quantity
    // wearing the same name.
    if (have_sync && have_window && m.sync > 0.0) {
        const double frames = m.window / m.sync;
        if (frames < 0.5) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "window/sync = %g, which rounds to %ld frames",
                          frames, static_cast<long>(std::floor(frames + 0.5)));
            diag_.error("SE0264", src, window_loc,
                        "`window` is shorter than one `sync` frame", buf,
                        {note("the metrics are averaged over whole frames, so a window "
                              "this short would report the latest frame, not a rolling "
                              "average (§13.6)"),
                         note("the window also sets the resolution of `sim.overrun_pct`: "
                              "N frames means steps of 100/N percent"),
                         help("`window` must be at least `sync`")});
        }
    }
    // §10.4 — the base rate every node's `sample_rate` falls back to.
    base_rate_ = m.step > 0.0 ? 1.0 / m.step : 0.0;
    return have_root && have_step && have_duration;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Instantiation
// ═════════════════════════════════════════════════════════════════════════════

// §13.3 — last writer wins. Every match is marked used, not just the winner,
// so a superseded duplicate is not then reported as having hit nothing.
OverrideEntry* Elaborator::find_override(const std::string& path) {
    OverrideEntry* found = nullptr;
    for (OverrideEntry& e : overrides_) {
        if (e.path != path) continue;
        e.used = true;
        found = &e;
    }
    return found;
}

void Elaborator::apply_overrides(const NodeInfo* def, const std::string& path,
                                 std::map<std::string, Pin>& pinned) {
    for (const auto& si : def->settings) {
        // §6.2c — the leaves this setting carries. A record is addressed one
        // leaf at a time and never as a whole, which is the rule §13.4 already
        // states for an array state and for the same reason: §7 has no
        // aggregate value, and §14.1's runtime loader reads a number.
        std::vector<Leafling> leaves;
        destructure(bound_type(si.type), nullptr, si.name, si.loc, *def->file->src,
                    "setting `" + si.name + "`", leaves);

        if (si.type.is_record) {
            if (OverrideEntry* whole = find_override(sub(path, si.name))) {
                std::string one;
                for (const Leafling& lf : leaves) {
                    if (!one.empty()) one += ", ";
                    one += "`" + sub(path, lf.key) + "`";
                }
                diag_.error("SE0460", *whole->src, whole->loc,
                            "override `" + whole->path + "` names a record, not a value",
                            "a record has no single value",
                            {note("address one field at a time: " + one),
                             note("the same rule an array state follows (\xc2\xa7""13.4)")});
                failed_ = true;
            }
        }

        for (const Leafling& lf : leaves) {
            OverrideEntry* o = find_override(sub(path, lf.key));
            if (!o) continue;
            Value v;
            if (!eval(o->value, *o->src, Env{}, v)) continue;
            double d = 0.0;
            if (coerce(v, lf.type, *o->src, o->loc,
                       ("setting `" + o->path + "`").c_str(), d)) {
                // §13.3 — an override detaches from the flow-down, so it emits
                // as a literal and the binding that would otherwise drive this
                // setting is simply not run. Evaluated in an empty environment,
                // so there is nothing it could depend on anyway.
                pinned[lf.key] = Pin{d, ec_num(d), true};
            }
        }
    }
}

// §6.2c — split a value across a record's field tree.
//
// This is where a brace list is consumed. It is deliberately NOT part of `eval`:
// §7.2 says every value is a real number with a dimension, and that stays true
// because an aggregate never becomes a `Value`. It is matched against the
// declared shape here, at the declaration site, and what comes out the other
// side is one ordinary scalar expression per leaf.
bool Elaborator::destructure(const Type& target, const ast::Expr* value,
                             const std::string& key, Loc decl_loc, const Source& src,
                             const std::string& what, std::vector<Leafling>& out) {
    const bool agg = value && value->kind == ast::Expr::Kind::Aggregate;

    if (!target.is_record) {
        if (agg) {
            diag_.error("SE0426", src, value->loc,
                        "a brace list where " + what + " declares a scalar",
                        "`" + target.scalar + "(" + target.unit.str() + ")` is not a record",
                        {note("a brace list initialises a record, field by field "
                              "(\xc2\xa7""6.2c)")});
            return false;
        }
        out.push_back(
            Leafling{key, target, value, value ? value->loc : decl_loc, decl_loc});
        return true;
    }

    const RecordInfo* rec = target.record;
    if (!rec) return false;

    // A whole-record copy — `limit = param.accel_limit`. One spelling reaches
    // every leaf, so the per-leaf references are synthesised rather than
    // written out. Whether the source really has those leaves is settled by the
    // ordinary `param.*` lookup in `eval`, which reports SE0304 against the
    // dotted name and lists what IS in scope.
    if (value && value->kind == ast::Expr::Kind::Param) {
        bool ok = true;
        for (const Field& f : rec->fields) {
            auto e = std::make_unique<ast::Expr>();
            e->kind = ast::Expr::Kind::Param;
            e->text = value->text + "." + f.name;
            e->loc = value->loc;
            const ast::Expr* raw = e.get();
            synth_.push_back(std::move(e));
            ok = destructure(field_type(target, f), raw, key + "." + f.name, decl_loc, src,
                             what, out) && ok;
        }
        return ok;
    }

    if (value && !agg) {
        diag_.error("SE0426", src, value->loc,
                    "a value where " + what + " declares the record `" + rec->fq + "`",
                    "expected a brace list",
                    {note("\xc2\xa7""7 is a scalar language: a record is built field by "
                          "field, not computed (\xc2\xa7""6.2c)")});
        return false;
    }

    // Which expression initialises which field. A field with none is not an
    // error here: it falls through with a null expression, and whether that
    // means "keep the default" or "still required" is the caller's to decide.
    std::vector<const ast::Expr*> supplied(rec->fields.size(), nullptr);
    bool ok = true;

    if (agg && value->field_names.empty()) {
        // Positional: complete, and in the order the `type` declares.
        if (value->args.size() != rec->fields.size()) {
            std::string have;
            for (const Field& f : rec->fields) {
                if (!have.empty()) have += ", ";
                have += f.name;
            }
            diag_.error("SE0423", src, value->loc,
                        "`" + rec->fq + "` has " + std::to_string(rec->fields.size()) +
                            " field(s), but the brace list gives " +
                            std::to_string(value->args.size()),
                        "wrong number of values",
                        {note("fields, in order: " + have),
                         help("a positional brace list must be complete; name the fields "
                              "to give only some of them (\xc2\xa7""6.2c)")});
            ok = false;
        }
        for (std::size_t i = 0; i < value->args.size() && i < rec->fields.size(); ++i)
            supplied[i] = value->args[i].get();
    } else if (agg) {
        // Named: order-free, and may be partial.
        for (std::size_t i = 0; i < value->field_names.size() && i < value->args.size();
             ++i) {
            const std::string& fname = value->field_names[i];
            const Loc floc = i < value->field_locs.size() ? value->field_locs[i]
                                                          : value->loc;
            std::size_t idx = rec->fields.size();
            for (std::size_t j = 0; j < rec->fields.size(); ++j)
                if (rec->fields[j].name == fname) { idx = j; break; }
            if (idx == rec->fields.size()) {
                std::string have;
                for (const Field& f : rec->fields) {
                    if (!have.empty()) have += ", ";
                    have += "`" + f.name + "`";
                }
                diag_.error("SE0424", src, floc,
                            "`" + rec->fq + "` has no field `" + fname + "`",
                            "unknown field", {help("it declares " + have)});
                ok = false;
                continue;
            }
            if (supplied[idx]) {
                diag_.error("SE0425", src, floc,
                            "field `" + fname + "` is given twice", "already given");
                ok = false;
                continue;
            }
            supplied[idx] = value->args[i].get();
        }
    }

    for (std::size_t i = 0; i < rec->fields.size(); ++i)
        ok = destructure(field_type(target, rec->fields[i]), supplied[i],
                         key + "." + rec->fields[i].name, decl_loc, src, what, out) && ok;
    return ok;
}

bool Elaborator::settings_of(const NodeInfo* def,
                             const std::map<std::string, Pin>& pinned,
                             const Source& src, Loc loc, std::vector<SettingValue>& out) {
    const Source& def_src = *def->file->src;
    Env env;

    // §6.2c — every setting expanded to the scalars it actually carries, in
    // declaration order. A scalar setting yields one leaf keyed by its own name,
    // so the walk below is the walk it has always been; a record setting simply
    // contributes more than one node to it.
    bool shape_ok = true;
    std::vector<Leafling> leaves;
    for (const auto& si : def->settings) {
        const std::size_t before = leaves.size();
        if (!destructure(bound_type(si.type), si.default_value, si.name, si.loc, def_src,
                         "setting `" + si.name + "`", leaves)) {
            shape_ok = false;
            for (std::size_t i = before; i < leaves.size(); ++i) leaves[i].errored = true;
        }
    }
    // What each setting will be assigned in the configuration program: a pinned
    // one keeps the expression that pinned it, a derived one gets its coerced
    // default below. Collected here rather than in `out` because the topological
    // walk decides the order and this map preserves it by name.
    std::map<std::string, Pin> resolved;

    // Every setting in scope reads as its own `param` member, whatever produced
    // its value: at configuration the member is what a sibling expression sees,
    // so an override of this setting reaches everything derived from it.
    for (const Leafling& lf : leaves) {
        auto it = pinned.find(lf.key);
        if (it == pinned.end()) continue;
        resolved[lf.key] = it->second;
        // §6.2a — SUBSTITUTED, for the same reason the derived-default path
        // below coerces against `bound()`: a setting may be declared in a unit
        // parameter (`sigma (U)`), and a sibling expression that reads it must
        // see this instance's real unit, not the definition's `(U)`. Leaving it
        // unsubstituted made every setting derived from a pinned unit-parametric
        // setting come out dimensionless, which surfaced as a bogus `SE0410`
        // pointing INTO the library rather than at the binding.
        Value v{it->second.value, lf.type.unit, false};
        v.code = ec_param(settings_path_, lf.key);
        v.constant = false;
        env[lf.key] = std::move(v);
    }

    // Topological evaluation of the unpinned defaults (§6.2). The graph is
    // walked depth-first; a back edge is a cycle.
    enum class Mark { White, Grey, Black };
    std::map<std::string, Mark> mark;
    for (const Leafling& lf : leaves)
        mark[lf.key] = env.count(lf.key) ? Mark::Black : Mark::White;

    std::vector<std::string> stack;
    std::function<bool(const Leafling&)> visit = [&](const Leafling& s) -> bool {
        if (mark[s.key] == Mark::Black) return true;
        if (mark[s.key] == Mark::Grey) {
            std::string cycle;
            for (const std::string& n : stack) cycle += n + " -> ";
            cycle += s.key;
            diag_.error("SE0421", def_src, s.decl_loc,
                        "cycle among derived settings of `" + def->def->name + "`",
                        "`" + s.key + "` depends on itself",
                        {note("cycle: " + cycle),
                         note("settings are a dependency graph requiring a topological "
                              "sort, so a cycle has no value (§6.2)")});
            mark[s.key] = Mark::Black;
            return false;
        }
        mark[s.key] = Mark::Grey;
        stack.push_back(s.key);

        bool ok = true;
        if (s.errored) {
            // The shape error is the diagnostic; `unbound` would be a second
            // report of the same mistake.
            ok = false;
        } else if (!s.expr) {
            // §6.2 — no default means required, and every instantiation must
            // bind it. Checked statically, here.
            diag_.error("SE0420", src, loc,
                        "required setting `" + s.key + "` is not bound",
                        "instantiated here",
                        {note("declared with no default, so every instantiation must "
                              "bind it (§6.2)",
                              def->file->path + ":" +
                                  std::to_string(s.decl_loc.line))});
            ok = false;
        } else {
            // Resolve dependencies first.
            std::vector<const ast::Expr*> work{s.expr};
            while (!work.empty()) {
                const ast::Expr* e = work.back();
                work.pop_back();
                if (!e) continue;
                if (e->kind == ast::Expr::Kind::Param) {
                    // §6.2c — `param.offset.x` names a leaf, so the dotted
                    // text IS the graph key and no field walk is needed here.
                    for (const Leafling& other : leaves)
                        if (other.key == e->text && mark[other.key] != Mark::Black)
                            ok = visit(other) && ok;
                }
                work.push_back(e->lhs.get());
                work.push_back(e->rhs.get());
                for (const ast::ExprPtr& arg : e->args) work.push_back(arg.get());
            }
            Value v;
            if (ok && eval(s.expr, def_src, env, v)) {
                // §6.2a — a setting may be declared in a unit parameter
                // (`limit (U)`), so this must coerce against the instance's
                // substituted unit rather than the definition's `(U)`. §6.2c
                // put the substituted unit on the leaf, so it is already right.
                double d = 0.0;
                ExprCode code;
                if (coerce(v, s.type, def_src, s.loc,
                           ("setting `" + s.key + "`").c_str(), d, &code)) {
                    resolved[s.key] = Pin{d, std::move(code), v.constant};
                    Value bound_v{d, s.type.unit, false};
                    bound_v.code = ec_param(settings_path_, s.key);
                    bound_v.constant = false;
                    env[s.key] = std::move(bound_v);
                } else {
                    ok = false;
                }
            } else {
                ok = false;
            }
        }
        stack.pop_back();
        mark[s.key] = Mark::Black;
        return ok;
    };

    bool ok = shape_ok;

    // §10.4 — `sample_rate` and `time_step` are this node's EFFECTIVE values,
    // so the reserved `rate` setting has to be resolved before any other
    // setting can read them. Seeded with the base rate first, so that a `rate`
    // default which itself mentions `sample_rate` reads the base step rather
    // than an undefined value.
    effective_rate_ = base_rate_;
    effective_rate_code_ = ec_num(base_rate_);
    for (const Leafling& lf : leaves) {
        if (lf.key != "rate") continue;
        ok = visit(lf) && ok;
        auto it = env.find("rate");
        if (it != env.end() && it->second.v > 0.0) effective_rate_ = it->second.v;
        // Only now, so that a `rate` default which itself mentions `sample_rate`
        // reads the base step rather than referring to itself.
        effective_rate_code_ = ec_param(settings_path_, "rate");
        break;
    }

    for (const Leafling& lf : leaves) ok = visit(lf) && ok;

    slots_.clear();
    for (const Leafling& lf : leaves) {
        SettingValue sv;
        sv.name = lf.key;
        // §15.6 promises the manifest reader that `value` is already in `unit`.
        // §6.2c put the substituted unit on the leaf, which for a record field
        // means the field's own unit and not the record's arguments.
        sv.unit = lf.type.unit;
        sv.scalar = lf.type.scalar;
        auto it = env.find(lf.key);
        sv.value = it == env.end() ? 0.0 : it->second.v;

        // §6.2b — the same leaf as one assignment in the configuration program.
        // `leaves` is already in the order the topological walk above settled,
        // so appending here preserves it.
        SettingSlot slot;
        slot.path = sub(settings_path_, lf.key);
        slot.owner = settings_path_;
        slot.name = lf.key;
        // §6.2c — the declaration the leaf belongs to. A dotted `name` addresses
        // INTO a member; it cannot declare one, so a composite's parameter
        // struct needs the setting itself (§15.5).
        slot.decl = lf.key.substr(0, lf.key.find('.'));
        if (const NodeInfo::SettingInfo* si = def->setting(slot.decl))
            slot.decl_type = bound_type(si->type);
        else
            slot.decl_type = lf.type;
        slot.unit = sv.unit;
        slot.scalar = sv.scalar;
        slot.value = sv.value;
        slot.rate = (lf.key == "rate");
        auto r = resolved.find(lf.key);
        if (r != resolved.end()) {
            slot.expr = r->second.code;
            slot.constant = r->second.constant;
        } else {
            // A required setting nobody bound: already reported as SE0420, and
            // the program still needs a well-formed entry to stay emittable.
            slot.expr = ec_num(sv.value);
        }
        slots_.push_back(std::move(slot));

        out.push_back(std::move(sv));
    }
    return ok;
}

std::vector<Unit> Elaborator::unit_args_of(const NodeInfo* child, const ast::Instance& inst,
                                           const Source& src) {
    const std::size_t want = child->unit_params.size();
    const std::size_t got = inst.unit_args.size();
    std::vector<Unit> out;

    if (want != got) {
        std::string names;
        for (const std::string& p : child->unit_params) {
            if (!names.empty()) names += ", ";
            names += p;
        }
        std::vector<Attachment> att;
        if (want == 0)
            att.push_back(note("`" + child->def->name + "` declares no `units` section"));
        else
            att.push_back(note("`" + child->def->name + "` declares " + std::to_string(want) +
                               " unit parameter" + (want == 1 ? "" : "s") + ": " + names));
        att.push_back(help("unit arguments are positional, after the definition — "
                           "`node x : " + child->def->name + " (m/s^2) { … };`"));
        diag_.error("SE0419", src, got ? inst.unit_args_loc : inst.loc,
                    "`" + child->def->name + "` takes " + std::to_string(want) +
                        " unit argument" + (want == 1 ? "" : "s") + ", given " +
                        std::to_string(got),
                    "wrong number of unit arguments", std::move(att));
        failed_ = true;
        return out;
    }

    for (const ast::UnitPtr& u : inst.unit_args) {
        // Evaluated with NO parameter scope: a unit argument is a concrete
        // unit, so `(U)` here would have to name a real Appendix A symbol.
        UnitEval ue(src, diag_);
        Unit bound_unit;
        if (!ue.eval(u.get(), bound_unit)) {
            failed_ = true;
            bound_unit = unit_one();
        } else if (bound_unit.nonradian_angle) {
            // Same reason as `SE0415`: this unit becomes a declared unit of
            // every port that mentions the parameter.
            diag_.error("SE0415", src, inst.unit_args_loc,
                        "`(" + bound_unit.str() + ")` may not be a unit argument",
                        "declare this in radians",
                        {note("a unit argument becomes the declared unit of every port "
                              "that names the parameter (§4.5, §6.2a)")});
            failed_ = true;
        }
        out.push_back(std::move(bound_unit));
    }
    return out;
}

void Elaborator::instantiate(const NodeInfo* def, const std::string& path,
                             std::map<std::string, Pin> pinned, const Source& src,
                             Loc loc, int depth, Model& m,
                             std::vector<Unit> unit_binding) {
    if (depth > 64) {
        diag_.error("SE0312", src, loc,
                    "recursive instantiation of `" + def->fq + "`",
                    "nesting is unbounded here",
                    {note("a composite is flattened at elaboration, so a node that "
                          "transitively contains itself has no finite runtime form "
                          "(§15.2)")});
        failed_ = true;
        return;
    }
    instances_[path] = def;

    // Swapped in for the duration of this node, and restored on the way out so
    // the recursion into children does not inherit the parent's binding.
    std::vector<Unit> saved_binding;
    saved_binding.swap(unit_binding_);
    unit_binding_ = std::move(unit_binding);
    struct RestoreBinding {
        std::vector<Unit>& live;
        std::vector<Unit>& saved;
        ~RestoreBinding() { live.swap(saved); }
    } restore_binding{unit_binding_, saved_binding};

    apply_overrides(def, path, pinned);
    std::vector<SettingValue> settings;
    settings_path_ = path;
    if (!settings_of(def, pinned, src, loc, settings)) failed_ = true;

    // §6.2b — this node's slice of the configuration program, appended in walk
    // order. Parents are instantiated before their children and a node's own
    // settings are topologically sorted within it, so appending as we go yields
    // a globally valid order with no second sort.
    for (SettingSlot& slot : slots_) m.settings.push_back(std::move(slot));
    slots_.clear();

    for (const SettingValue& s : settings) known_setting_paths_.push_back(sub(path, s.name));

    // A child binding is evaluated in the parent's scope, so `param.*` here
    // reads the PARENT's members — the second of the two owners an ExprTok can
    // name, and what makes an override of a composite setting flow downward.
    Env env;
    for (const SettingValue& s : settings) {
        Value v{s.value, s.unit, false};
        v.code = ec_param(path, s.name);
        v.constant = false;
        env[s.name] = std::move(v);
    }

    if (def->composite) {
        const Source& def_src = *def->file->src;

        // §6.2a — evaluated once per composite and shared with `collect_wires`,
        // which needs them to make endpoint units concrete. Computing them in
        // both places would report every arity error twice.
        std::map<std::string, std::vector<Unit>> child_bindings;
        for (const ast::Instance& inst : def->def->structure.instances) {
            auto it = def->children.find(inst.name);
            if (it == def->children.end()) continue;
            if (it->second->unit_params.empty() && inst.unit_args.empty()) continue;
            child_bindings[inst.name] = unit_args_of(it->second, inst, def_src);
        }

        collect_wires(def, path, m, child_bindings);
        for (const ast::Instance& inst : def->def->structure.instances) {
            auto child_it = def->children.find(inst.name);
            if (child_it == def->children.end()) continue;
            const NodeInfo* child = child_it->second;

            std::map<std::string, Pin> child_pinned;
            std::set<std::string> bound;
            for (const ast::Binding& b : inst.bindings) {
                const NodeInfo::SettingInfo* target = child->setting(b.name);
                if (!target) {
                    std::vector<Attachment> att;
                    std::string have;
                    for (const auto& s : child->settings) {
                        if (!have.empty()) have += ", ";
                        have += "`" + s.name + "`";
                    }
                    if (!have.empty()) att.push_back(help("`" + child->def->name +
                                                          "` declares " + have));
                    diag_.error("SE0441", def_src, b.loc,
                                "`" + child->def->name + "` has no setting `" + b.name +
                                    "`",
                                "unknown setting", std::move(att));
                    failed_ = true;
                    continue;
                }
                if (!bound.insert(b.name).second) {
                    diag_.error("SE0441", def_src, b.loc,
                                "setting `" + b.name + "` is bound twice", "already bound");
                    failed_ = true;
                    continue;
                }
                // §6.2a — the child's setting may be declared in one of the
                // child's unit parameters, so the binding is checked against
                // the CHILD's substitution, not this composite's.
                Type child_target = target->type;
                auto cb = child_bindings.find(inst.name);
                if (cb != child_bindings.end())
                    child_target = bind_type(target->type, cb->second);

                // §6.2c — a binding may name a record, and then it pins one leaf
                // per field. A scalar binding splits into exactly one leaf,
                // which is the path this has always taken.
                std::vector<Leafling> bl;
                if (!destructure(child_target, b.value.get(), b.name, b.loc, def_src,
                                 "setting `" + b.name + "`", bl)) {
                    failed_ = true;
                    // The binding was meant to bind these, and saying so was the
                    // whole error. Pin every leaf it covers so the child does not
                    // answer one bad brace list with an SE0420 per field; the
                    // values are inert, because the run is already refused.
                    std::vector<Leafling> all;
                    destructure(child_target, nullptr, b.name, b.loc, def_src,
                                "setting `" + b.name + "`", all);
                    for (const Leafling& lf : all)
                        child_pinned.emplace(lf.key, Pin{0.0, ec_num(0.0), true});
                    continue;
                }
                for (const Leafling& lf : bl) {
                    // A partial named list leaves a field unmentioned; that leaf
                    // simply is not pinned, and the child's own default runs.
                    if (!lf.expr) continue;
                    Value v;
                    if (!eval(lf.expr, def_src, env, v)) {
                        failed_ = true;
                        continue;
                    }
                    double d = 0.0;
                    ExprCode code;
                    if (coerce(v, lf.type, def_src, lf.loc,
                               ("setting `" + lf.key + "`").c_str(), d, &code))
                        child_pinned[lf.key] = Pin{d, std::move(code), v.constant};
                    else
                        failed_ = true;
                }
            }
            auto bit = child_bindings.find(inst.name);
            instantiate(child, sub(path, inst.name), child_pinned, def_src, inst.loc,
                        depth + 1, m,
                        bit == child_bindings.end() ? std::vector<Unit>{} : bit->second);
        }
        return;
    }

    // ─── A leaf ──────────────────────────────────────────────────────────────
    Leaf leaf;
    leaf.path = path;
    leaf.node = def;
    leaf.loc = loc;
    leaf.settings = std::move(settings);

    // §9.2 — the elaborated decimation. It is a DEFAULT, not the final word:
    // `rate` is an ordinary runtime setting, so the emitted model recomputes
    // this in resolve_settings() and re-runs the divisor check there. What is
    // checked here is what `sec` can see, which is the default.
    for (const SettingValue& s : leaf.settings) {
        if (s.name != "rate") continue;
        const double base_rate = m.step > 0.0 ? 1.0 / m.step : 0.0;
        const double ratio = s.value > 0.0 ? base_rate / s.value : 0.0;
        const double r = std::floor(ratio + 0.5);
        if (s.value <= 0.0 || std::fabs(ratio - r) > 1e-9 || r < 1.0) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%g", s.value);
            std::vector<Attachment> att;
            char note_buf[160];
            std::snprintf(note_buf, sizeof note_buf,
                          "%g / %g = %.4f; the nearest legal rates are %g Hz and %g Hz",
                          base_rate, s.value, ratio, base_rate / std::max(1.0, std::floor(ratio)),
                          base_rate / std::max(1.0, std::ceil(ratio)));
            att.push_back(note(note_buf));
            diag_.error("SE0450", src, loc,
                        std::string("rate ") + buf +
                            " Hz is not an integer divisor of the base rate",
                        "instantiated here", std::move(att));
            failed_ = true;
        } else {
            leaf.decimation = static_cast<long>(r);
        }
    }

    const Source& def_src = *def->file->src;
    for (const auto& st : def->states) {
        StateSlot slot;
        slot.name = st.name;
        slot.continuous = st.continuous;
        slot.unit = bound(st.type.unit);
        slot.der_unit = bound(st.der_unit);
        slot.scalar = st.type.scalar;
        if (st.extent) extent_of(st.extent, st.extent_loc, def_src, env, sub(path, st.name),
                                 slot.extent);
        if (st.initial) {
            Value v;
            if (eval(st.initial, def_src, env, v))
                coerce(v, st.type, def_src, st.initial->loc,
                       ("state `" + st.name + "`").c_str(), slot.initial);
        }
        // §13.4 — an IC override rides the SAME channel as a setting.
        known_setting_paths_.push_back(sub(path, st.name));
        if (OverrideEntry* o = find_override(sub(path, st.name))) {
            if (slot.extent.present) {
                // §6.4a — there is no path naming the whole array. §7 has
                // no aggregate value to assign to it, and the declared default
                // already covers the uniform case.
                diag_.error("SE0460", *o->src, o->loc,
                            "`" + o->path + "` is an array state and has no whole-array "
                            "value",
                            "an element index is required",
                            {note("an array state is addressed one element at a time, "
                                  "`" + o->path + "[0]` (§13.4)"),
                             note("the declared default already initialises every "
                                  "element (§6.4a)")});
                failed_ = true;
            } else {
                Value v;
                if (eval(o->value, *o->src, Env{}, v))
                    coerce(v, st.type, *o->src, o->loc,
                           ("initial condition `" + o->path + "`").c_str(), slot.initial);
            }
        }
        // §13.4 — `bw.w1[3] = 0 (-);`. The index cannot be checked here at
        // all: the extent settles at configuration, and the same source may be
        // valid against one `order` and invalid against another (§6.2b).
        if (slot.extent.present) {
            const std::string base = sub(path, st.name);
            for (OverrideEntry& o : overrides_) {
                std::string seg;
                std::size_t index = 0;
                if (!split_index(o.path, seg, index) || seg != base) continue;
                o.used = true;
                StateSlot::ElementIC ic;
                ic.index = index;
                Value v;
                if (!eval(o.value, *o.src, Env{}, v)) continue;
                if (!coerce(v, st.type, *o.src, o.loc,
                            ("initial condition `" + o.path + "`").c_str(), ic.value))
                    continue;
                slot.element_ic.push_back(ic);
                Model::ElementRef ref;
                ref.path = o.path;
                ref.continuous = slot.continuous;
                ref.index = index;
                m.element_refs.push_back(std::move(ref));
                element_sites_.push_back({m.element_refs.size() - 1, m.leaves.size(),
                                          leaf.states.size()});
            }
        }
        leaf.states.push_back(std::move(slot));
    }

    // §6.5 — an array var. Node-private storage, so nothing external ever
    // addresses it; all it needs is a size, which lands at configuration with
    // every other extent.
    for (const Field& v : def->vars) {
        if (!v.extent) continue;
        ArrayVar av;
        av.name = v.name;
        if (extent_of(v.extent, v.extent_loc, def_src, env, sub(path, v.name), av.extent))
            leaf.array_vars.push_back(std::move(av));
    }

    // §6.2a — the ports as this instance actually sees them. For a
    // non-parametric node `bound` is the identity, so this is a copy.
    for (const Field& f : def->inputs) {
        Field g = f;
        g.type = bound_type(f.type);
        leaf.ports_in.push_back(std::move(g));
    }
    for (const Field& f : def->outputs) {
        Field g = f;
        g.type = bound_type(f.type);
        leaf.ports_out.push_back(std::move(g));
    }

    leaf_by_path_[path] = m.leaves.size();
    for (const Field& f : def->outputs)
        leaf_outputs_[sub(path, f.name)] = {m.leaves.size(), f.name};
    m.leaves.push_back(std::move(leaf));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Wires
// ═════════════════════════════════════════════════════════════════════════════

bool Elaborator::port_conversion(const Field& from, const Field& to, const Source& src,
                                 Loc loc, double& scale, double& offset,
                                 std::vector<FieldConv>& field_convs) {
    scale = 1.0;
    offset = 0.0;
    field_convs.clear();
    if (from.type.is_record != to.type.is_record) {
        diag_.error("SE0410", src, loc, "a record and a scalar cannot be wired together",
                    std::string(from.type.is_record ? "record -> scalar"
                                                    : "scalar -> record"));
        return false;
    }
    if (from.type.is_record) {
        // The two ends must be the same record: a wire carries the whole value,
        // and there is no structural conversion between two different shapes.
        if (from.type.record != to.type.record) {
            diag_.error("SE0410", src, loc, "record types do not match",
                        "`" + from.type.record->fq + "` -> `" + to.type.record->fq + "`",
                        {note("a wire carries the whole record (§5.3)")});
            return false;
        }
        // §5.2 binds a parametric record's units at the REFERENCE, so the two
        // ends may have bound them differently. Differing *scales* are what the
        // per-field walk below converts; differing *dimensions* are an error,
        // and it belongs on the argument rather than on each field it reaches —
        // one wrong argument to `Vec3` is one mistake, not three.
        bool args_ok = true;
        for (std::size_t i = 0; i < from.type.unit_args.size(); ++i) {
            const Unit& a = from.type.unit_args[i];
            const Unit& b = to.type.unit_args[i];
            if (compatible(a, b)) continue;
            const std::string& pname = from.type.record->unit_params[i];
            diag_.error("SE0410", src, loc, "incompatible unit arguments across a wire",
                        "`(" + a.str() + ")` -> `(" + b.str() + ")`",
                        {note("both ports are `" + from.type.record->fq + "`, but bound `" +
                              pname + "` differently"),
                         note("source is " + a.dim_str()),
                         note("destination is " + b.dim_str())});
            args_ok = false;
        }
        if (!args_ok) return false;

        // Compatible arguments, so every field derived from them is compatible
        // too and converts on its own terms — exactly as a scalar port does.
        // `field_type()` performs the substitution, including threading a nested
        // record's arguments down. The per-field error below is a backstop for a
        // field whose unit does not come from the arguments at all.
        bool ok = true;
        std::function<void(const Type&, const Type&, const std::string&)> walk =
            [&](const Type& a, const Type& b, const std::string& prefix) {
                for (const Field& f : a.record->fields) {
                    const Type af = field_type(a, f);
                    const Type bf = field_type(b, f);
                    const std::string name = prefix.empty() ? f.name : prefix + "." + f.name;
                    if (af.is_record) {
                        walk(af, bf, name);
                        continue;
                    }
                    if (!compatible(af.unit, bf.unit)) {
                        diag_.error(
                            "SE0410", src, loc, "incompatible units across a wire",
                            "`(" + af.unit.str() + ")` -> `(" + bf.unit.str() + ")`",
                            {note("field `" + name + "` of `" + a.record->fq + "`"),
                             note("source is " + af.unit.dim_str()),
                             note("destination is " + bf.unit.dim_str())});
                        ok = false;
                        continue;
                    }
                    FieldConv fc;
                    fc.scale = af.unit.scale / bf.unit.scale;
                    fc.offset = (af.unit.offset - bf.unit.offset) / bf.unit.scale;
                    field_convs.push_back(fc);
                }
            };
        walk(from.type, to.type, "");
        return ok;
    }
    if (!compatible(from.type.unit, to.type.unit)) {
        diag_.error("SE0410", src, loc, "incompatible units across a wire",
                    "`(" + from.type.unit.str() + ")` -> `(" + to.type.unit.str() + ")`",
                    {note("source is " + from.type.unit.dim_str()),
                     note("destination is " + to.type.unit.dim_str())});
        return false;
    }
    // The conversion folds to a constant and is applied where the value is
    // read into the consumer's `In` view, which is a copy anyway.
    scale = from.type.unit.scale / to.type.unit.scale;
    offset = (from.type.unit.offset - to.type.unit.offset) / to.type.unit.scale;
    return true;
}

void Elaborator::collect_wires(const NodeInfo* def, const std::string& path, Model& m,
                               const std::map<std::string, std::vector<Unit>>& child_binding) {
    (void)m;
    const Source& src = *def->file->src;

    // §6.9.2 — the port a bare endpoint means. Null unless there is exactly
    // one, in which case `check_structure` has already said so (SE0314) and
    // this bails silently rather than reporting the same thing per instance.
    auto sole = [](const std::vector<Field>& ports) -> const Field* {
        return ports.size() == 1 ? &ports[0] : nullptr;
    };

    // Resolves one endpoint to its port and its addressable path. `as_source`
    // flips the polarity of a `self` endpoint, which is the whole content of
    // §6.9.2's rule about the boundary being inverted from the inside.
    auto endpoint = [&](const ast::Endpoint& e, bool as_source, Field& out_field,
                        std::string& key) -> bool {
        const Field* field = nullptr;
        if (e.is_self) {
            if (e.segs.empty()) {
                // Bare `self`, so the sole port on the side the arrow needs.
                const Field* only = sole(as_source ? def->inputs : def->outputs);
                if (!only) return false;
                out_field = *only;
                out_field.type.unit = bound(only->type.unit);
                key = sub(path, only->name);
                return true;
            }
            const std::string& n = e.segs[0];
            field = as_source ? def->input(n) : def->output(n);
            if (!field) {
                const Field* other = as_source ? def->output(n) : def->input(n);
                if (other)
                    diag_.error("SE0443", src, e.loc,
                                std::string("`self.") + n + "` is " +
                                    (as_source ? "an output" : "an input") +
                                    " and cannot be a wire " +
                                    (as_source ? "source" : "destination"),
                                "wrong direction",
                                {note("seen from the inside, the boundary's polarity is "
                                      "inverted: `self.<input>` is a source and "
                                      "`self.<output>` is a destination (§6.9.2)")});
                else
                    diag_.error("SE0304", src, e.loc,
                                "`" + def->def->name + "` has no port `" + n + "`",
                                "unresolved port");
                return false;
            }
            // A composite may not be unit-parametric (`SE0418`), so its own
            // binding is empty and this is a copy.
            out_field = *field;
            out_field.type.unit = bound(field->type.unit);
            key = sub(path, n);
            return true;
        }
        if (e.segs.empty()) return false;
        const std::string& inst = e.segs[0];
        auto it = def->children.find(inst);
        if (it == def->children.end()) {
            diag_.error("SE0304", src, e.loc,
                        "`" + def->def->name + "` has no child instance `" + inst + "`",
                        "unresolved instance");
            return false;
        }
        const NodeInfo* child = it->second;

        // The instance has to be resolved before the port can be, because a
        // bare endpoint's port name IS the child's arity.
        std::string port;
        if (e.segs.size() == 1) {
            const Field* only = sole(as_source ? child->outputs : child->inputs);
            if (!only) return false;
            port = only->name;
        } else {
            port = e.segs[1];
        }

        field = as_source ? child->output(port) : child->input(port);
        if (!field) {
            const Field* other = as_source ? child->input(port) : child->output(port);
            if (other)
                diag_.error("SE0443", src, e.loc,
                            "`" + inst + "." + port + "` is " +
                                (as_source ? "an input" : "an output") +
                                " and cannot be a wire " +
                                (as_source ? "source" : "destination"),
                            "wrong direction",
                            {note("direction is always source -> destination (§6.9.2)")});
            else
                diag_.error("SE0304", src, e.loc,
                            "`" + child->def->name + "` has no port `" + port + "`",
                            "unresolved port");
            return false;
        }
        out_field = *field;
        auto bit = child_binding.find(inst);
        if (bit != child_binding.end())
            out_field.type = bind_type(field->type, bit->second);
        key = sub(sub(path, inst), port);
        return true;
    };

    for (const ast::Wire& w : def->def->structure.wires) {
        Field sf;
        std::string skey;
        if (!endpoint(w.source, /*as_source=*/true, sf, skey)) {
            failed_ = true;
            continue;
        }
        for (const ast::Endpoint& d : w.dests) {
            Field df;
            std::string dkey;
            if (!endpoint(d, /*as_source=*/false, df, dkey)) {
                failed_ = true;
                continue;
            }
            Hop hop;
            hop.source = skey;
            hop.loc = d.loc;
            hop.src = &src;
            if (!port_conversion(sf, df, src, d.loc, hop.scale, hop.offset, hop.field_convs))
                failed_ = true;

            auto existing = producers_.find(dkey);
            if (existing != producers_.end()) {
                // §6.9.2 — fan-in is what would make the single-writer wire
                // lowering unsound, so it is rejected rather than summed.
                diag_.error("SE0442", src, d.loc,
                            "two sources drive `" + dkey + "`", "second source here",
                            {note("already driven by `" + existing->second.source + "`"),
                             note("fan-out is free, fan-in is forbidden: a wire "
                                  "coincides with the producer's storage (§15.3)"),
                             help("insert an explicit `Sum` node")});
                failed_ = true;
                continue;
            }
            producers_[dkey] = hop;
        }
    }
}

void Elaborator::resolve_inputs(Model& m) {
    std::set<std::string> root_inputs;
    for (const Field& f : m.root->inputs) root_inputs.insert(f.name);

    for (std::size_t i = 0; i < m.leaves.size(); ++i) {
        Leaf& leaf = m.leaves[i];
        for (const Field& port : leaf.node->inputs) {
            const std::string key = sub(leaf.path, port.name);

            std::vector<const Hop*> chain;
            std::string k = key;
            for (std::size_t guard = 0; guard <= producers_.size(); ++guard) {
                auto it = producers_.find(k);
                if (it == producers_.end()) break;
                chain.push_back(&it->second);
                k = it->second.source;
            }

            InputSource in;
            auto out_it = leaf_outputs_.find(k);
            if (out_it != leaf_outputs_.end()) {
                in.kind = InputSource::Kind::Leaf;
                in.producer = out_it->second.first;
                in.port = out_it->second.second;
            } else if (chain.empty() ? root_inputs.count(key) != 0
                                     : root_inputs.count(k) != 0) {
                in.kind = InputSource::Kind::Boundary;
                in.boundary = "sig.in." + (chain.empty() ? key : k);
            } else {
                // §6.3 — there is no implicit zero.
                std::vector<Attachment> att;
                if (!chain.empty())
                    att.push_back(note("the wire reaches `" + k +
                                       "`, which nothing drives"));
                att.push_back(note("an unconnected input is an error: there is no "
                                   "implicit zero (§6.3)"));
                diag_.error("SE0430", *leaf.node->file->src, port.loc,
                            "input `" + key + "` is not connected", "declared here", att);
                failed_ = true;
                continue;
            }

            double s = 1.0, o = 0.0;
            for (std::size_t j = chain.size(); j-- > 0;) {
                s = chain[j]->scale * s;
                o = chain[j]->scale * o + chain[j]->offset;
            }
            in.scale = s;
            in.offset = o;

            // A record's fields compose the same way, element-wise. Every hop
            // on one chain carries the same record type (port_conversion has
            // already rejected any that does not), so the vectors agree in
            // length; a hop that reported none simply leaves the identity.
            for (std::size_t j = chain.size(); j-- > 0;) {
                const std::vector<FieldConv>& hop = chain[j]->field_convs;
                if (hop.empty()) continue;
                if (in.field_convs.empty()) in.field_convs.resize(hop.size());
                for (std::size_t f = 0; f < hop.size() && f < in.field_convs.size(); ++f) {
                    in.field_convs[f].scale = hop[f].scale * in.field_convs[f].scale;
                    in.field_convs[f].offset =
                        hop[f].scale * in.field_convs[f].offset + hop[f].offset;
                }
            }
            leaf.inputs[port.name] = in;
        }
    }
}

void Elaborator::resolve_boundary(const NodeInfo* root, Model& m) {
    for (const Field& f : root->inputs) {
        // The root's inputs become the fields of `Sim::Signals::In`, so the
        // host writes `sim.sig.in.axle_torque` and the manifest addresses the
        // same thing as `in.axle_torque`.
        BoundaryIn b;
        b.ident = f.name;
        b.path = "in." + f.name;
        b.type = f.type;
        m.boundary_in.push_back(std::move(b));
    }

    // A root output is named in the manifest by the leaf that actually
    // produces it, because that is the storage the host would read.
    for (const Field& f : root->outputs) {
        std::string k = f.name;
        for (std::size_t guard = 0; guard <= producers_.size(); ++guard) {
            auto it = producers_.find(k);
            if (it == producers_.end()) break;
            k = it->second.source;
        }
        auto out_it = leaf_outputs_.find(k);
        if (out_it == leaf_outputs_.end()) continue;   // undriven; simply unread
        const Leaf& leaf = m.leaves[out_it->second.first];
        // §6.2a — the substituted port, so the manifest reports the unit the
        // host actually receives rather than the definition's `(U)`.
        const Field* port = nullptr;
        for (const Field& lf : leaf.ports_out)
            if (lf.name == out_it->second.second) { port = &lf; break; }
        if (!port) continue;

        // Records are listed field by field: the manifest is read by a human
        // wiring up a host, and `ws` alone would not tell them anything.
        std::function<void(const std::string&, const std::string&, const Type&)> expand =
            [&](const std::string& path, const std::string& expr, const Type& t) {
                if (!t.is_record) {
                    BoundaryOut b;
                    b.path = path;
                    b.expr = expr;
                    b.type = t;
                    m.boundary_out.push_back(std::move(b));
                    return;
                }
                for (const Field& sf : t.record->fields)
                    expand(path + "." + sf.name, expr + "." + sf.name, field_type(t, sf));
            };
        expand(sub(leaf.path, port->name), "sig." + leaf.ident + "." + port->name,
               port->type);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Recording and logging
// ═════════════════════════════════════════════════════════════════════════════

void Elaborator::resolve_record(const ast::RecordBlock& block, const Source& src,
                                Model& m) {
    m.record.present = true;
    m.record.file = block.has_file ? block.file : "run.csv";
    m.record.every = block.has_every ? block.every : 1;
    if (m.record.every < 1) m.record.every = 1;

    for (const ast::Path& p : block.signals) {
        const std::string path = p.str();

        // §13.6 — the engine's own metrics are addressed in the same scheme.
        if (p.segs.size() == 2 && p.segs[0] == "sim") {
            RecordedSignal s;
            s.path = path;
            Unit pct;
            lookup_unit_symbol("%", pct);
            s.unit = pct;
            if (p.segs[1] == "utilization") s.expr = "se_rt::realtime().utilization()";
            else if (p.segs[1] == "overrun_pct") s.expr = "se_rt::realtime().overrun_pct()";
            else {
                diag_.error("SE0304", src, p.loc, "unknown engine metric `" + path + "`",
                            "not a metric",
                            {note("`sim.utilization` and `sim.overrun_pct` (§13.6)")});
                failed_ = true;
                continue;
            }
            m.record.signals.push_back(std::move(s));
            continue;
        }

        // Longest instance-path prefix wins, so `whl.out.ws.slip` splits at
        // `whl` and the rest is an accessor path.
        std::size_t best = 0;
        std::string leaf_path;
        for (std::size_t n = p.segs.size(); n-- > 0;) {
            std::string candidate;
            for (std::size_t i = 0; i < n; ++i) candidate = sub(candidate, p.segs[i]);
            if (leaf_by_path_.count(candidate)) {
                leaf_path = candidate;
                best = n;
                break;
            }
        }
        if (leaf_path.empty() && !leaf_by_path_.count("")) {
            diag_.error("SE0304", src, p.loc, "no node at `" + path + "`",
                        "unresolved signal path",
                        {note("signals are dotted paths from the root (§13.2)")});
            failed_ = true;
            continue;
        }
        const std::size_t leaf_index = leaf_by_path_[leaf_path];
        const Leaf& leaf = m.leaves[leaf_index];
        std::vector<std::string> rest(p.segs.begin() + best, p.segs.end());
        // §13.5 — `bw.state.w1[3]`. The index comes off the last segment
        // before anything resolves a name, so every lookup below sees `w1`.
        bool has_index = false;
        std::size_t elem_index = 0;
        if (!rest.empty()) {
            std::string stem;
            if (split_index(rest.back(), stem, elem_index)) {
                rest.back() = stem;
                has_index = true;
            }
        }
        if (rest.empty()) {
            diag_.error("SE0304", src, p.loc, "`" + path + "` names a node, not a signal",
                        "not a signal");
            failed_ = true;
            continue;
        }

        std::string kind;
        if (rest[0] == "out" || rest[0] == "state" || rest[0] == "var") {
            kind = rest[0];
            rest.erase(rest.begin());
            if (rest.empty()) {
                diag_.error("SE0304", src, p.loc, "`" + path + "` names no member",
                            "incomplete path");
                failed_ = true;
                continue;
            }
        } else {
            // §13.5 — a bare name is legal when unambiguous, and loud when not.
            const bool is_state = leaf.node->state(rest[0]) != nullptr;
            const bool is_out = leaf.node->output(rest[0]) != nullptr;
            if (is_state && is_out) {
                diag_.error("SE0461", src, p.loc,
                            "`" + path + "` is ambiguous", "both a state and an output",
                            {note("`" + leaf.path + ".state." + rest[0] + "` names the state"),
                             note("`" + leaf.path + ".out." + rest[0] + "` names the output"),
                             note("a node may legitimately have both (§6.11)")});
                failed_ = true;
                continue;
            }
            kind = is_state ? "state" : (is_out ? "out" : "var");
        }

        RecordedSignal s;
        s.path = path;
        const std::string base = leaf.ident;

        if (kind == "state") {
            const NodeInfo::StateInfo* st = leaf.node->state(rest[0]);
            if (!st || rest.size() != 1) {
                diag_.error("SE0304", src, p.loc, "`" + path + "` names no state",
                            "unresolved");
                failed_ = true;
                continue;
            }
            // Read the block, not the node's view of it. A continuous state is
            // a slot in `x` — the solver's ABI — and a discrete one is a field
            // of `dis`; the node holds only proxies into both (§15.5).
            const StateSlot* ss = nullptr;
            std::size_t state_index = 0;
            for (std::size_t i = 0; i < leaf.states.size(); ++i)
                if (leaf.states[i].name == rest[0]) {
                    ss = &leaf.states[i];
                    state_index = i;
                }
            if (ss && ss->extent.present != has_index) {
                // Neither direction can be waved through: an array state has no
                // single value to record, and a scalar one has no element 0 to
                // name that would still be true if it grew one.
                diag_.error("SE0304", src, p.loc,
                            ss->extent.present
                                ? "`" + path + "` is an array state and needs an index"
                                : "`" + path + "` is not an array state",
                            ss->extent.present ? "an element index is required"
                                               : "unexpected index",
                            {note(ss->extent.present
                                      ? "an element of an array state is recorded by "
                                        "index, `" + path + "[0]` (§13.5)"
                                      : "only an array-shaped state is indexed "
                                        "(§6.4a)")});
                failed_ = true;
                continue;
            }
            if (st->continuous) {
                if (!m.dynamic_states) {
                    s.expr = "x[" + std::to_string(ss ? ss->slot : 0) + "]";
                } else {
                    s.expr = "x[se_x_off_[" + std::to_string(ss ? ss->off : 0) + "]" +
                             (has_index ? " + " + std::to_string(elem_index) : "") + "]";
                }
            } else if (ss && ss->extent.present) {
                s.expr = "dis_arr[se_da_off_[" + std::to_string(ss->off) + "] + " +
                         std::to_string(elem_index) + "]";
            } else {
                s.expr = "dis." + base + "." + rest[0];
            }
            if (has_index && ss) {
                Model::ElementRef ref;
                ref.path = path;
                ref.continuous = ss->continuous;
                ref.index = elem_index;
                m.element_refs.push_back(std::move(ref));
                element_sites_.push_back({m.element_refs.size() - 1, leaf_index,
                                          state_index});
            }
            // §6.2a — the slot's unit is substituted; the node's is still `(U)`.
            s.unit = ss ? ss->unit : st->type.unit;
        } else {
            if (has_index) {
                // §6.4a — only a state is array-shaped in the recordable
                // surface. A port may not carry an extent at all (SE0234), and
                // an array var is node-private storage with no path (§6.5).
                diag_.error("SE0304", src, p.loc,
                            "`" + path + "` is not an array state", "unexpected index",
                            {note("a port may not be array-shaped (SE0234), and an "
                                  "array var is node-private (§6.5)")});
                failed_ = true;
                continue;
            }
            const Field* f = nullptr;
            if (kind == "out")
                for (const Field& lf : leaf.ports_out)
                    if (lf.name == rest[0]) f = &lf;
            if (!f)
                for (const Field& v : leaf.node->vars)
                    if (kind == "var" && v.name == rest[0]) f = &v;
            if (!f) {
                diag_.error("SE0304", src, p.loc,
                            "`" + path + "` names no " + kind + " of `" +
                                leaf.node->def->name + "`",
                            "unresolved");
                failed_ = true;
                continue;
            }
            // An output lives in the signal block; a var is deliberately NOT a
            // block member — §6.5 makes it private per-instance storage outside
            // the override channel — so it is still read through the node.
            s.expr = kind == "out" ? "sig." + base + "." + rest[0]
                                   : base + ".var." + rest[0];
            Type t = f->type;
            bool ok = true;
            for (std::size_t i = 1; i < rest.size(); ++i) {
                if (!t.is_record) {
                    ok = false;
                    break;
                }
                const Field* sf = nullptr;
                for (const Field& c : t.record->fields)
                    if (c.name == rest[i]) sf = &c;
                if (!sf) {
                    ok = false;
                    break;
                }
                s.expr += "." + rest[i];
                t = field_type(t, *sf);
            }
            if (!ok || t.is_record) {
                diag_.error("SE0304", src, p.loc,
                            "`" + path + "` does not name a scalar signal",
                            t.is_record ? "a record cannot be a CSV column" : "no such field");
                failed_ = true;
                continue;
            }
            s.unit = t.unit;
        }
        m.record.signals.push_back(std::move(s));
    }
}

void Elaborator::resolve_log(const ast::LogBlock& block, const Source& src, Model& m) {
    m.log.present = true;
    m.log.file = block.has_file ? block.file : "";
    if (block.has_level) {
        m.log.level = block.level;
        if (!one_of(block.level, kLevels, 5))
            diag_.error("SE0264", src, block.loc, "unknown log level `" + block.level + "`",
                        "not a level",
                        {note("`trace`, `debug`, `info`, `warn`, `error` (§13.7)")});
    }
    for (const auto& kv : block.levels) {
        if (!one_of(kv.second, kLevels, 5)) {
            diag_.error("SE0264", src, kv.first.loc,
                        "unknown log level `" + kv.second + "`", "not a level");
            continue;
        }
        m.log.levels.push_back({kv.first.str(), kv.second});
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Driver
// ═════════════════════════════════════════════════════════════════════════════

bool Elaborator::run(const ast::SimFile& sim, const Source& sim_src,
                     const std::string& sim_dir, Model& m) {
    m.name = sim.name;

    const int before = diag_.error_count();
    read_sim_entries(sim, sim_src, m);

    const ast::SimEntry* root_entry = nullptr;
    const ast::SimEntry* record_entry = nullptr;
    const ast::SimEntry* log_entry = nullptr;
    for (const ast::SimEntry& e : sim.entries) {
        if (e.key == "root") root_entry = &e;
        else if (e.key == "record") record_entry = &e;
        else if (e.key == "log") log_entry = &e;
        else if (e.key == "settings")
            load_settings_file(join_file(sim_dir, e.text), sim_src, e.loc);
    }
    if (!root_entry) return false;

    m.root = resolver_.resolve_root(root_entry->root.str(), sim_src, root_entry->loc);
    if (!m.root) return false;
    m.program = &resolver_.program();
    m.root_namespace = m.root->file->package;

    // §13.3 — the sim file's root block is the third writer; the `.settings`
    // source is the fourth, and is applied inside instantiate().
    std::map<std::string, Pin> pinned;
    std::set<std::string> bound;
    for (const ast::Binding& b : root_entry->bindings) {
        const NodeInfo::SettingInfo* target = m.root->setting(b.name);
        if (!target) {
            diag_.error("SE0441", sim_src, b.loc,
                        "`" + m.root->def->name + "` has no setting `" + b.name + "`",
                        "unknown setting");
            continue;
        }
        if (!bound.insert(b.name).second) {
            diag_.error("SE0441", sim_src, b.loc,
                        "setting `" + b.name + "` is bound twice", "already bound");
            continue;
        }
        // §6.2c — the root block binds a record the same way a `structure`
        // binding does. The root has no unit arguments to substitute, so the
        // declared type is already concrete.
        std::vector<Leafling> bl;
        if (!destructure(target->type, b.value.get(), b.name, b.loc, sim_src,
                         "setting `" + b.name + "`", bl)) {
            std::vector<Leafling> all;
            destructure(target->type, nullptr, b.name, b.loc, sim_src,
                        "setting `" + b.name + "`", all);
            for (const Leafling& lf : all)
                pinned.emplace(lf.key, Pin{0.0, ec_num(0.0), true});
            continue;
        }
        for (const Leafling& lf : bl) {
            if (!lf.expr) continue;
            Value v;
            if (!eval(lf.expr, sim_src, Env{}, v)) continue;
            double d = 0.0;
            ExprCode code;
            if (coerce(v, lf.type, sim_src, lf.loc, ("setting `" + lf.key + "`").c_str(),
                       d, &code))
                // The root block is evaluated in an empty environment — there is
                // no enclosing node whose settings it could read — so it always
                // folds.
                pinned[lf.key] = Pin{d, std::move(code), v.constant};
        }
    }

    instantiate(m.root, "", pinned, sim_src, root_entry->loc, 0, m);
    if (m.leaves.empty()) {
        diag_.error("SE0511", sim_src, root_entry->loc,
                    "the model has no leaf nodes", "nothing to run",
                    {note("a composite leaves no runtime trace at all (§15.2)")});
        return false;
    }

    // §I5 — generated identifiers are entirely the generator's business, so
    // uniqueness is guaranteed here by construction rather than by a language
    // rule the user has to know about.
    std::map<std::string, int> used;
    for (Leaf& leaf : m.leaves) {
        // The root itself may be a leaf, in which case its model path is empty
        // — §13.2 addresses from the root without naming it.
        std::string base = leaf.path.empty() ? "root" : ident_of(leaf.path);
        int& n = used[base];
        leaf.ident = n == 0 ? base : base + "_" + std::to_string(n);
        ++n;
    }

    resolve_inputs(m);
    resolve_boundary(m.root, m);

    // §6.4a — one array state anywhere in the model switches the WHOLE
    // model to the dynamic lowering (§15.5a). It has to be model-wide: `x` is
    // one block, and a block cannot be half `constexpr`.
    for (const Leaf& leaf : m.leaves)
        for (const StateSlot& st : leaf.states)
            if (st.extent.present) m.dynamic_states = true;

    if (!m.dynamic_states) {
        std::size_t slot = 0;
        for (Leaf& leaf : m.leaves)
            for (StateSlot& st : leaf.states)
                if (st.continuous) st.slot = slot++;
        m.n_states = slot;
    } else {
        // Positions are a configuration result now, so what is assigned here is
        // only each state's ROW in the offset table the generated `configure()`
        // fills. A scalar state gets a row too, with an extent of one: two code
        // paths for "where does this state live" would be two chances to
        // disagree.
        std::size_t ci = 0, di = 0;
        for (Leaf& leaf : m.leaves)
            for (StateSlot& st : leaf.states) {
                if (st.continuous) st.off = ci++;
                else if (st.extent.present) st.off = di++;
            }
        m.n_cont_slots = ci;
        m.n_disc_arr_slots = di;
        m.n_states = 0;   // not known until §6.2b step 4
    }

    if (record_entry) resolve_record(record_entry->record, sim_src, m);
    if (log_entry) resolve_log(log_entry->log, sim_src, m);

    // Every element reference the model made, now that the offset table it
    // indexes into exists.
    for (const ElementSite& site : element_sites_)
        m.element_refs[site.ref].off = m.leaves[site.leaf].states[site.state].off;

    // Every override must have landed on something. A typo here is otherwise
    // completely silent, which is the worst possible failure for the layer
    // that parameter sweeps generate.
    for (const OverrideEntry& o : overrides_) {
        if (o.used) continue;
        std::vector<Attachment> att;
        auto inst = instances_.find(o.path);
        if (inst != instances_.end()) {
            att.push_back(note("`" + o.path + "` names a node, not a setting"));
        } else {
            const std::size_t dot = o.path.rfind('.');
            const std::string prefix = dot == std::string::npos ? "" : o.path.substr(0, dot);
            const std::string name = dot == std::string::npos ? o.path : o.path.substr(dot + 1);
            auto owner = instances_.find(prefix);
            if (owner != instances_.end() && owner->second->output(name))
                att.push_back(note("`" + o.path +
                                   "` names an output, and outputs are computed (§13.3)"));
        }
        // A typo here is otherwise completely silent, so spend the budget on
        // pointing at what was probably meant.
        std::string best;
        int best_d = 4;
        for (const std::string& known : known_setting_paths_) {
            const int d = path_distance(o.path, known);
            if (d < best_d) {
                best_d = d;
                best = known;
            }
        }
        if (!best.empty()) att.push_back(help("did you mean `" + best + "`?"));
        att.push_back(note("overrides address a setting or a state by a dotted path from "
                           "the root (§13.2, §13.4)"));
        diag_.error("SE0460", *o.src, o.loc, "override `" + o.path + "` matches nothing",
                    "no such setting or state", std::move(att));
        failed_ = true;
    }

    return !failed_ && diag_.error_count() == before;
}

}  // namespace se
