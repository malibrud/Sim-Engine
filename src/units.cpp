// ─────────────────────────────────────────────────────────────────────────────
//  Units — implementation.  SPECIFICATION.md §4, Appendix A.
// ─────────────────────────────────────────────────────────────────────────────
#include "units.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace se {
namespace {

long gcd_l(long a, long b) {
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    while (b) {
        const long t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

constexpr double kPi = 3.14159265358979323846;

// The seven base dimensions, in the order of Unit::dim.
const char* const kBaseSym[kBaseDims] = {"m", "kg", "s", "A", "K", "mol", "cd"};

struct SymbolDef {
    const char* sym;
    signed char dim[kBaseDims];
    double scale;
    double offset;
};

// Appendix A. Dimensions are written out rather than derived so the table
// reads as a table; every row was checked against the SI brochure.
//                                   m  kg   s   A   K mol  cd
const SymbolDef kSymbols[] = {
    // Base
    {"m",    { 1,  0,  0,  0,  0,  0,  0}, 1.0, 0.0},
    {"kg",   { 0,  1,  0,  0,  0,  0,  0}, 1.0, 0.0},
    {"s",    { 0,  0,  1,  0,  0,  0,  0}, 1.0, 0.0},
    {"A",    { 0,  0,  0,  1,  0,  0,  0}, 1.0, 0.0},
    {"K",    { 0,  0,  0,  0,  1,  0,  0}, 1.0, 0.0},
    {"mol",  { 0,  0,  0,  0,  0,  1,  0}, 1.0, 0.0},
    {"cd",   { 0,  0,  0,  0,  0,  0,  1}, 1.0, 0.0},

    // Derived. `rad` and `sr` are dimensionless — see §4.5 for the cost.
    {"rad",  { 0,  0,  0,  0,  0,  0,  0}, 1.0, 0.0},
    {"sr",   { 0,  0,  0,  0,  0,  0,  0}, 1.0, 0.0},
    {"Hz",   { 0,  0, -1,  0,  0,  0,  0}, 1.0, 0.0},
    {"N",    { 1,  1, -2,  0,  0,  0,  0}, 1.0, 0.0},
    {"Pa",   {-1,  1, -2,  0,  0,  0,  0}, 1.0, 0.0},
    {"J",    { 2,  1, -2,  0,  0,  0,  0}, 1.0, 0.0},
    {"W",    { 2,  1, -3,  0,  0,  0,  0}, 1.0, 0.0},
    {"C",    { 0,  0,  1,  1,  0,  0,  0}, 1.0, 0.0},
    {"V",    { 2,  1, -3, -1,  0,  0,  0}, 1.0, 0.0},
    {"F",    {-2, -1,  4,  2,  0,  0,  0}, 1.0, 0.0},
    {"ohm",  { 2,  1, -3, -2,  0,  0,  0}, 1.0, 0.0},
    {"S",    {-2, -1,  3,  2,  0,  0,  0}, 1.0, 0.0},
    {"Wb",   { 2,  1, -2, -1,  0,  0,  0}, 1.0, 0.0},
    {"T",    { 0,  1, -2, -1,  0,  0,  0}, 1.0, 0.0},
    {"H",    { 2,  1, -2, -2,  0,  0,  0}, 1.0, 0.0},
    {"lm",   { 0,  0,  0,  0,  0,  0,  1}, 1.0, 0.0},
    {"lx",   {-2,  0,  0,  0,  0,  0,  1}, 1.0, 0.0},
    {"Bq",   { 0,  0, -1,  0,  0,  0,  0}, 1.0, 0.0},
    {"Gy",   { 2,  0, -2,  0,  0,  0,  0}, 1.0, 0.0},
    {"Sv",   { 2,  0, -2,  0,  0,  0,  0}, 1.0, 0.0},
    {"kat",  { 0,  0, -1,  0,  0,  1,  0}, 1.0, 0.0},

    // Accepted non-SI
    {"g",    { 0,  1,  0,  0,  0,  0,  0}, 1e-3, 0.0},
    {"deg",  { 0,  0,  0,  0,  0,  0,  0}, kPi / 180.0, 0.0},
    {"min",  { 0,  0,  1,  0,  0,  0,  0}, 60.0, 0.0},
    {"h",    { 0,  0,  1,  0,  0,  0,  0}, 3600.0, 0.0},
    {"d",    { 0,  0,  1,  0,  0,  0,  0}, 86400.0, 0.0},
    {"L",    { 3,  0,  0,  0,  0,  0,  0}, 1e-3, 0.0},
    {"t",    { 0,  1,  0,  0,  0,  0,  0}, 1000.0, 0.0},
    {"bar",  {-1,  1, -2,  0,  0,  0,  0}, 1e5, 0.0},
    {"atm",  {-1,  1, -2,  0,  0,  0,  0}, 101325.0, 0.0},
    // One revolution per minute. `rad` is dimensionless, so rpm is 1/s.
    {"rpm",  { 0,  0, -1,  0,  0,  0,  0}, 2.0 * kPi / 60.0, 0.0},
    {"%",    { 0,  0,  0,  0,  0,  0,  0}, 0.01, 0.0},
    {"degC", { 0,  0,  0,  0,  1,  0,  0}, 1.0, 273.15},
    {"degF", { 0,  0,  0,  0,  1,  0,  0}, 5.0 / 9.0, 459.67 * 5.0 / 9.0},
    {"in",   { 1,  0,  0,  0,  0,  0,  0}, 0.0254, 0.0},
    {"ft",   { 1,  0,  0,  0,  0,  0,  0}, 0.3048, 0.0},
    {"mi",   { 1,  0,  0,  0,  0,  0,  0}, 1609.344, 0.0},
    {"lb",   { 0,  1,  0,  0,  0,  0,  0}, 0.45359237, 0.0},
    {"hp",   { 2,  1, -3,  0,  0,  0,  0}, 745.6998715822702, 0.0},
    {"psi",  {-1,  1, -2,  0,  0,  0,  0}, 6894.757293168361, 0.0},
};

struct PrefixDef {
    const char* sym;
    double scale;
};

// Longest first, so `da` is deca- and never deci-atto-.
const PrefixDef kPrefixes[] = {
    {"da", 1e1},
    {"y", 1e-24}, {"z", 1e-21}, {"a", 1e-18}, {"f", 1e-15}, {"p", 1e-12},
    {"n", 1e-9},  {"u", 1e-6},  {"m", 1e-3},  {"c", 1e-2},  {"d", 1e-1},
    {"h", 1e2},   {"k", 1e3},   {"M", 1e6},   {"G", 1e9},   {"T", 1e12},
    {"P", 1e15},  {"E", 1e18},  {"Z", 1e21},  {"Y", 1e24},
};

Unit from_def(const SymbolDef& d, double prefix_scale) {
    Unit u;
    for (int i = 0; i < kBaseDims; ++i) u.dim[i] = rat(d.dim[i]);
    u.scale = d.scale * prefix_scale;
    u.offset = d.offset;
    u.has_offset = d.offset != 0.0;
    return u;
}

// Levenshtein, capped — only ever run on a symbol that already failed.
int edit_distance(std::string_view a, std::string_view b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const int sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({cur[j - 1] + 1, prev[j] + 1, sub});
        }
        prev = cur;
    }
    return prev[b.size()];
}

}  // namespace

// ─── Rationals ───────────────────────────────────────────────────────────────

Rat rat(long n, long d) {
    if (d == 0) d = 1;
    if (d < 0) {
        n = -n;
        d = -d;
    }
    const long g = gcd_l(n, d);
    return Rat{n / g, d / g};
}

Rat rat_add(Rat a, Rat b) { return rat(a.n * b.d + b.n * a.d, a.d * b.d); }
Rat rat_sub(Rat a, Rat b) { return rat(a.n * b.d - b.n * a.d, a.d * b.d); }
Rat rat_mul(Rat a, Rat b) { return rat(a.n * b.n, a.d * b.d); }
Rat rat_neg(Rat a) { return Rat{-a.n, a.d}; }

std::string rat_str(Rat r) {
    if (r.d == 1) return std::to_string(r.n);
    return "(" + std::to_string(r.n) + "/" + std::to_string(r.d) + ")";
}

// ─── Unit algebra ────────────────────────────────────────────────────────────

bool Unit::dimensionless() const {
    for (int i = 0; i < kBaseDims; ++i)
        if (!dim[i].zero()) return false;
    return true;
}

std::string Unit::str() const {
    std::string num, den;
    int n_num = 0, n_den = 0;
    for (const UnitFactor& f : display) {
        if (f.exp.zero()) continue;
        const bool negative = f.exp.n < 0;
        std::string& side = negative ? den : num;
        int& count = negative ? n_den : n_num;
        if (count) side += '*';
        side += f.sym;
        const Rat e = negative ? rat_neg(f.exp) : f.exp;
        if (!(e == Rat{1, 1})) {
            side += '^';
            side += rat_str(e);
        }
        ++count;
    }
    if (n_num == 0 && n_den == 0) return "-";
    if (n_den == 0) return num;
    if (n_num == 0) num = "1";
    if (n_den > 1) return num + "/(" + den + ")";
    return num + "/" + den;
}

std::string Unit::dim_str() const {
    Unit canonical;
    for (int i = 0; i < kBaseDims; ++i) {
        canonical.dim[i] = dim[i];
        if (!dim[i].zero()) canonical.display.push_back({kBaseSym[i], dim[i]});
    }
    return canonical.str();
}

bool compatible(const Unit& a, const Unit& b) {
    for (int i = 0; i < kBaseDims; ++i)
        if (a.dim[i] != b.dim[i]) return false;
    return true;
}

double to_si(double v, const Unit& u) { return v * u.scale + u.offset; }
double from_si(double v, const Unit& u) { return (v - u.offset) / u.scale; }
double convert(double v, const Unit& a, const Unit& b) {
    if (a.scale == b.scale && a.offset == b.offset) return v;
    return from_si(to_si(v, a), b);
}

Unit unit_one() { return Unit{}; }

namespace {

void merge_factor(std::vector<UnitFactor>& into, const UnitFactor& f) {
    for (UnitFactor& g : into) {
        if (g.sym == f.sym) {
            g.exp = rat_add(g.exp, f.exp);
            return;
        }
    }
    into.push_back(f);
}

void drop_zero_factors(std::vector<UnitFactor>& v) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const UnitFactor& f) { return f.exp.zero(); }),
            v.end());
}

}  // namespace

Unit unit_mul(const Unit& a, const Unit& b) {
    Unit r;
    for (int i = 0; i < kBaseDims; ++i) r.dim[i] = rat_add(a.dim[i], b.dim[i]);
    r.scale = a.scale * b.scale;
    r.display = a.display;
    for (const UnitFactor& f : b.display) merge_factor(r.display, f);
    drop_zero_factors(r.display);
    return r;
}

Unit unit_div(const Unit& a, const Unit& b) {
    Unit r;
    for (int i = 0; i < kBaseDims; ++i) r.dim[i] = rat_sub(a.dim[i], b.dim[i]);
    r.scale = a.scale / b.scale;
    r.display = a.display;
    for (const UnitFactor& f : b.display) merge_factor(r.display, {f.sym, rat_neg(f.exp)});
    drop_zero_factors(r.display);
    return r;
}

Unit unit_pow(const Unit& a, Rat e) {
    Unit r;
    for (int i = 0; i < kBaseDims; ++i) r.dim[i] = rat_mul(a.dim[i], e);
    r.scale = std::pow(a.scale, static_cast<double>(e.n) / static_cast<double>(e.d));
    for (const UnitFactor& f : a.display) r.display.push_back({f.sym, rat_mul(f.exp, e)});
    drop_zero_factors(r.display);
    return r;
}

// ─── Symbol lookup ───────────────────────────────────────────────────────────

bool lookup_unit_symbol(std::string_view sym, Unit& out) {
    for (const SymbolDef& d : kSymbols) {
        if (sym == d.sym) {
            out = from_def(d, 1.0);
            out.display.push_back({std::string(sym), rat(1)});
            return true;
        }
    }
    // Longest prefix plus an exact unit. `kg` never reaches here (exact match
    // above), which is what makes `mg` a milligram (§ Appendix A).
    for (const PrefixDef& p : kPrefixes) {
        const std::size_t n = std::char_traits<char>::length(p.sym);
        if (sym.size() <= n || sym.compare(0, n, p.sym) != 0) continue;
        const std::string_view rest = sym.substr(n);
        for (const SymbolDef& d : kSymbols) {
            if (rest != d.sym) continue;
            // A prefixed offset unit (`mdegC`) is meaningless; reject by
            // omission rather than by producing a nonsense affine map.
            if (d.offset != 0.0) continue;
            out = from_def(d, p.scale);
            out.display.push_back({std::string(sym), rat(1)});
            return true;
        }
    }
    return false;
}

std::vector<std::string> near_unit_symbols(std::string_view sym) {
    std::vector<std::pair<int, std::string>> scored;
    for (const SymbolDef& d : kSymbols) {
        const int dist = edit_distance(sym, d.sym);
        if (dist <= 2) scored.push_back({dist, d.sym});
    }
    std::sort(scored.begin(), scored.end());
    std::vector<std::string> out;
    for (const auto& s : scored) {
        out.push_back(s.second);
        if (out.size() == 3) break;
    }
    return out;
}

// ─── Evaluation ──────────────────────────────────────────────────────────────

bool UnitEval::eval(const ast::UnitExpr* u, Unit& out) {
    return eval_inner(u, out, /*compound=*/false);
}

bool UnitEval::eval_inner(const ast::UnitExpr* u, Unit& out, bool compound) {
    if (!u) {
        out = unit_one();
        return true;
    }
    using K = ast::UnitExpr::Kind;
    switch (u->kind) {
        case K::Dimensionless:
        case K::One:
            out = unit_one();
            return true;

        case K::Symbol: {
            if (!lookup_unit_symbol(u->symbol, out)) {
                std::vector<Attachment> att;
                const std::vector<std::string> near = near_unit_symbols(u->symbol);
                if (!near.empty()) {
                    std::string s = "did you mean ";
                    for (std::size_t i = 0; i < near.size(); ++i) {
                        if (i) s += (i + 1 == near.size()) ? " or " : ", ";
                        s += "`(" + near[i] + ")`";
                    }
                    att.push_back(help(s + "?"));
                }
                diag_.error("SE0414", src_, u->loc,
                            "unknown unit symbol `" + u->symbol + "`",
                            "not a unit in Appendix A", std::move(att));
                out = unit_one();
                return false;
            }
            if (out.has_offset && compound) {
                diag_.error(
                    "SE0411", src_, u->loc,
                    "offset unit `" + u->symbol + "` in a compound unit expression",
                    "only legal on its own",
                    {note("`(" + u->symbol +
                          "/s)` is ambiguous between a temperature and a "
                          "temperature difference"),
                     help("use `(K/s)` for a rate of change")});
                return false;
            }
            return true;
        }

        case K::Mul:
        case K::Div: {
            Unit a, b;
            const bool ok_a = eval_inner(u->lhs.get(), a, /*compound=*/true);
            const bool ok_b = eval_inner(u->rhs.get(), b, /*compound=*/true);
            out = (u->kind == K::Mul) ? unit_mul(a, b) : unit_div(a, b);
            return ok_a && ok_b;
        }

        case K::Pow: {
            Unit base;
            const bool ok = eval_inner(u->base.get(), base, /*compound=*/true);
            out = unit_pow(base, rat(u->exp_num, u->exp_den));
            return ok;
        }
    }
    out = unit_one();
    return true;
}

}  // namespace se
