#include "parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "dump.hpp"   // `unit_to_string`, for a literal endpoint's spelling (§6.9.2)

namespace se {

namespace {

// ─── did-you-mean support ────────────────────────────────────────────────────

int edit_distance(const std::string& a, const std::string& b) {
    const std::size_t n = a.size(), m = b.size();
    std::vector<int> prev(m + 1), cur(m + 1);
    for (std::size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= m; ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = cur;
    }
    return prev[m];
}

// Closest candidate within `max` edits, or empty.
std::string closest(const std::string& word, const std::vector<const char*>& candidates,
                    int max = 3) {
    std::string best;
    int best_d = max + 1;
    for (const char* c : candidates) {
        const int d = edit_distance(word, std::string(c));
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    return best_d <= max ? best : std::string();
}

const std::vector<const char*>& section_names() {
    static const std::vector<const char*> v = {
        "settings", "inputs", "outputs", "states", "vars",
        "native",   "build",  "structure", "declarations", "units", "tests"};
    return v;
}

const std::vector<const char*>& lifecycle_names() {
    static const std::vector<const char*> v = {"init",   "output",  "derivative",
                                              "next",   "transition", "adjust",
                                              "on_step", "final"};
    return v;
}

const std::vector<const char*>& scalar_types() {
    static const std::vector<const char*> v = {"double", "float", "int", "bool"};
    return v;
}

const std::vector<const char*>& build_primitives() {
    static const std::vector<const char*> v = {"link", "include_dir", "define", "cflag"};
    return v;
}

const std::vector<const char*>& platform_atoms() {
    static const std::vector<const char*> v = {"windows", "linux", "macos"};
    return v;
}

const std::vector<const char*>& sim_keys() {
    static const std::vector<const char*> v = {"root",   "step",   "solver", "duration",
                                               "mode",   "sync",   "window", "settings",
                                               "record", "log"};
    return v;
}

bool contains(const std::vector<const char*>& v, const std::string& s) {
    for (const char* c : v)
        if (s == c) return true;
    return false;
}

}  // namespace

// ─── Construction and the token stream ───────────────────────────────────────

Parser::Parser(const Source& src, Diagnostics& diag)
    : src_(src), diag_(diag), lexer_(src, diag) {}

void Parser::advance() {
    if (peeked_) {
        tok_ = *peeked_;
        peeked_.reset();
    } else {
        tok_ = lexer_.next();
    }
}

const Token& Parser::peek() {
    if (!peeked_) peeked_ = lexer_.next();
    return *peeked_;
}

bool Parser::accept(Tok k) {
    if (tok_.kind != k) return false;
    advance();
    return true;
}

bool Parser::swallow_invalid() {
    if (tok_.kind != Tok::Invalid) return false;
    advance();
    return true;
}

bool Parser::expect(Tok k, const char* code, const char* context) {
    if (tok_.kind == k) {
        advance();
        return true;
    }
    if (swallow_invalid()) return false;
    std::string msg = std::string("expected ") + describe(k);
    if (context) msg += std::string(" ") + context;
    msg += std::string(", found ") + describe(tok_.kind);
    err(code, tok_.loc, std::move(msg), std::string("expected ") + describe(k));
    return false;
}

void Parser::enter_raw() {
    peeked_.reset();
    lexer_.seek(tok_.loc);
}

void Parser::resync() {
    peeked_.reset();
    tok_ = lexer_.next();
}

void Parser::err(const char* code, Loc loc, std::string msg, std::string caret,
                 std::vector<Attachment> att) {
    diag_.error(code, src_, loc, std::move(msg), std::move(caret), std::move(att));
}

void Parser::warn(const char* code, Loc loc, std::string msg, std::string caret,
                  std::vector<Attachment> att) {
    diag_.warning(code, src_, loc, std::move(msg), std::move(caret), std::move(att));
}

// ─── Recovery (§16.3) ────────────────────────────────────────────────────────

void Parser::recover_in_list(Tok closer) {
    int depth = 0;
    while (!at_end()) {
        if (tok_.kind == Tok::LBrace) {
            ++depth;
        } else if (tok_.kind == Tok::RBrace) {
            if (depth == 0 && closer == Tok::RBrace) return;   // leave for the caller
            --depth;
        } else if (tok_.kind == Tok::Semi && depth == 0) {
            advance();
            return;
        }
        advance();
    }
}

void Parser::recover_in_node_body() {
    int depth = 0;
    while (!at_end()) {
        if (depth == 0) {
            switch (tok_.kind) {
                case Tok::RBrace:
                    return;
                case Tok::KwSettings:
                case Tok::KwInputs:
                case Tok::KwOutputs:
                case Tok::KwStates:
                case Tok::KwVars:
                case Tok::KwUnits:
                case Tok::KwNative:
                case Tok::KwDeclarations:
                case Tok::KwBuild:
                case Tok::KwStructure:
                case Tok::KwInit:
                case Tok::KwOutput:
                case Tok::KwDerivative:
                case Tok::KwNext:
                case Tok::KwTransition:
                case Tok::KwAdjust:
                case Tok::KwOnStep:
                case Tok::KwFinal:
                    return;
                default:
                    break;
            }
        }
        if (tok_.kind == Tok::LBrace) ++depth;
        else if (tok_.kind == Tok::RBrace) --depth;
        advance();
    }
}

void Parser::recover_to_definition() {
    int depth = 0;
    while (!at_end()) {
        if (depth == 0 && (tok_.kind == Tok::KwNode || tok_.kind == Tok::KwType ||
                           tok_.kind == Tok::KwFinite || tok_.kind == Tok::At))
            return;
        if (tok_.kind == Tok::LBrace) ++depth;
        else if (tok_.kind == Tok::RBrace && depth > 0) --depth;
        advance();
    }
}

// ─── Paths ───────────────────────────────────────────────────────────────────

bool Parser::parse_path(ast::Path& out, bool allow_keywords, bool allow_index) {
    const bool ok_first = allow_keywords ? tok_.is_word() : at(Tok::Ident);
    if (!ok_first) {
        std::vector<Attachment> att;
        if (tok_.is_keyword())
            att.push_back(note("`" + tok_.text + "` is a reserved word (Appendix B)"));
        err("SE0201", tok_.loc, std::string("expected a name, found ") + describe(tok_.kind),
            "expected a name", std::move(att));
        return false;
    }
    out.loc = tok_.loc;
    out.segs.push_back(tok_.text);
    advance();

    while (at(Tok::Dot)) {
        const Loc dot = tok_.loc;
        advance();
        if (at(Tok::Star)) {
            err("SE0210", tok_.loc, "wildcard import is not supported", "remove the `.*`",
                {note("every name's origin must be traceable from the `use` block (\xc2\xa7""2.4)"),
                 help("import the symbols you need (`use se.mech.Damper;`) or alias the "
                      "package (`use se.mech;`)")});
            advance();
            return false;
        }
        const bool ok = allow_keywords ? tok_.is_word() : at(Tok::Ident);
        if (!ok) {
            err("SE0201", tok_.loc,
                std::string("expected a name after `.`, found ") + describe(tok_.kind),
                "expected a name");
            (void)dot;
            return false;
        }
        out.segs.push_back(tok_.text);
        out.loc.length = tok_.loc.offset + tok_.loc.length - out.loc.offset;
        advance();
    }

    // §6.4a — `bw.w1[3]`. A literal index only: the extent is not known
    // until configuration, so there is nothing here for an expression to read,
    // and an index past the configured extent is a configuration failure rather
    // than a parse error (§13.4).
    if (allow_index && at(Tok::LBracket)) {
        advance();
        if (!at(Tok::Number) || tok_.text.find_first_not_of("0123456789") != std::string::npos) {
            err("SE0201", tok_.loc,
                std::string("expected a non-negative integer index, found ") +
                    describe(tok_.kind),
                "expected an index",
                {note("an array element is addressed by a literal index (§6.4a)")});
            return false;
        }
        const std::string index = tok_.text;
        advance();
        const Loc close = tok_.loc;
        if (!expect(Tok::RBracket, "SE0201", "to close the index")) return false;
        out.segs.back() += "[" + index + "]";
        out.loc.length = close.offset + close.length - out.loc.offset;
    }
    return true;
}

bool Parser::parse_qualified_name(ast::Path& out) { return parse_path(out, false); }

// ─── Units (§4.1) ────────────────────────────────────────────────────────────

ast::UnitPtr Parser::parse_unit() {
    if (!at(Tok::LParen)) {
        err("SE0205", tok_.loc, "expected a unit annotation", "expected `(`",
            {note("every scalar declaration carries a unit; `(-)` spells dimensionless "
                  "(\xc2\xa7""5.1)"),
             help("`name (unit): double;` for a scalar, `name: RecordType;` for a record")});
        return nullptr;
    }
    advance();
    ast::UnitPtr u = parse_unit_expr();
    if (!u) return nullptr;
    if (!expect(Tok::RParen, "SE0206", "to close the unit")) return nullptr;
    return u;
}

ast::UnitPtr Parser::parse_unit_expr() {
    // `(-)` — dimensionless.
    if (at(Tok::Minus) && peek().kind == Tok::RParen) {
        auto u = std::make_unique<ast::UnitExpr>();
        u->kind = ast::UnitExpr::Kind::Dimensionless;
        u->loc = tok_.loc;
        advance();
        return u;
    }

    ast::UnitPtr left = parse_unit_term();
    if (!left) return nullptr;
    while (at(Tok::Star) || at(Tok::Slash)) {
        const bool mul = at(Tok::Star);
        const Loc op = tok_.loc;
        advance();
        ast::UnitPtr right = parse_unit_term();
        if (!right) return nullptr;
        auto n = std::make_unique<ast::UnitExpr>();
        n->kind = mul ? ast::UnitExpr::Kind::Mul : ast::UnitExpr::Kind::Div;
        n->loc = op;
        n->lhs = std::move(left);
        n->rhs = std::move(right);
        left = std::move(n);
    }
    return left;
}

ast::UnitPtr Parser::parse_unit_term() {
    ast::UnitPtr base = parse_unit_factor();
    if (!base) return nullptr;
    if (!at(Tok::Caret)) return base;

    const Loc caret = tok_.loc;
    advance();
    long num = 1, den = 1;
    if (!parse_unit_exponent(num, den)) return nullptr;

    auto p = std::make_unique<ast::UnitExpr>();
    p->kind = ast::UnitExpr::Kind::Pow;
    p->loc = caret;
    p->base = std::move(base);
    p->exp_num = num;
    p->exp_den = den;
    return p;
}

ast::UnitPtr Parser::parse_unit_factor() {
    auto u = std::make_unique<ast::UnitExpr>();
    u->loc = tok_.loc;

    if (at(Tok::Number)) {
        if (tok_.text != "1") {
            err("SE0206", tok_.loc, "only the literal `1` may appear in a unit expression",
                "found `" + tok_.text + "`",
                {note("`1` is the identity unit, as in `(1/(kg*m^2))` (\xc2\xa7""4.1)")});
            advance();
            return nullptr;
        }
        u->kind = ast::UnitExpr::Kind::One;
        advance();
        return u;
    }
    if (at(Tok::Ident)) {
        u->kind = ast::UnitExpr::Kind::Symbol;
        u->symbol = tok_.text;
        advance();
        return u;
    }
    if (at(Tok::Percent)) {
        u->kind = ast::UnitExpr::Kind::Symbol;
        u->symbol = "%";
        advance();
        return u;
    }
    if (at(Tok::LParen)) {
        advance();
        ast::UnitPtr inner = parse_unit_expr();
        if (!inner) return nullptr;
        if (!expect(Tok::RParen, "SE0206", "to close the nested unit")) return nullptr;
        return inner;
    }

    err("SE0206", tok_.loc,
        std::string("malformed unit expression: expected a unit symbol, found ") +
            describe(tok_.kind),
        "expected a unit symbol",
        {note("a unit is `1`, a symbol, or a parenthesised unit expression, combined "
              "with `*`, `/` and `^` (\xc2\xa7""4.1)")});
    return nullptr;
}

bool Parser::parse_unit_exponent(long& num, long& den) {
    num = 1;
    den = 1;

    auto integer = [&](long& out) -> bool {
        bool neg = false;
        if (at(Tok::Minus)) {
            neg = true;
            advance();
        }
        if (!at(Tok::Number) || tok_.text.find('.') != std::string::npos ||
            tok_.text.find('e') != std::string::npos ||
            tok_.text.find('E') != std::string::npos) {
            err("SE0206", tok_.loc, "unit exponent must be an integer", "",
                {note("rational exponents are written `m^(1/2)` (\xc2\xa7""4.1)")});
            return false;
        }
        out = std::strtol(tok_.text.c_str(), nullptr, 10);
        if (neg) out = -out;
        advance();
        return true;
    };

    if (at(Tok::LParen)) {
        advance();
        if (!integer(num)) return false;
        if (at(Tok::Slash)) {
            advance();
            if (!integer(den)) return false;
            if (den == 0) {
                err("SE0206", tok_.loc, "unit exponent has a zero denominator");
                return false;
            }
        }
        return expect(Tok::RParen, "SE0206", "to close the exponent");
    }
    return integer(num);
}

// ─── Type references (§5.1) ──────────────────────────────────────────────────

// One production for every declaration, scalar or record: `name: Path(u1, …)`.
//
// A one-segment path naming a member of the closed scalar type set (§5.1) is a
// SCALAR reference and its argument list is its unit — exactly one argument for
// `double`, `float` and `int`, none for `bool`, which has no dimension to carry.
// Anything else is a record reference whose arguments bind that type's
// `units { }` parameters (§5.2).
//
// Scalars and records therefore differ in what the name resolves to and in
// nothing else. That is what lets "compatible units convert" hold at every wire
// in the language rather than at scalar wires only.
bool Parser::parse_type_ref(ast::TypeRef& out, const char* what) {
    out.loc = tok_.loc;

    if (!at(Tok::Colon)) {
        err("SE0205", tok_.loc, std::string("expected `:` after the ") + what,
            "expected `: type(unit)`",
            {note("every declaration is `name: Type(unit)` — one shape, whether the "
                  "type is scalar or a record (\xc2\xa7""5.1)")});
        return false;
    }
    advance();

    if (!parse_qualified_name(out.record)) return false;
    out.loc = out.record.loc;

    // The path is complete, so a `(` here can be nothing but the argument list;
    // this costs no lookahead, the same reasoning as an instance's arguments
    // (§6.9.1).
    std::vector<ast::UnitPtr> args;
    Loc args_loc = out.loc;
    if (at(Tok::LParen)) {
        args_loc = tok_.loc;
        advance();
        for (;;) {
            ast::UnitPtr u = parse_unit_expr();
            if (!u) return false;
            args.push_back(std::move(u));
            if (at(Tok::Comma)) {
                advance();
                continue;
            }
            break;
        }
        if (!expect(Tok::RParen, "SE0201", "to close the unit arguments")) return false;
    }

    const bool one_segment = out.record.segs.size() == 1;
    const std::string head = one_segment ? out.record.segs[0] : std::string();

    if (one_segment && contains(scalar_types(), head)) {
        out.is_scalar = true;
        out.scalar = head;
        // Arity is checked HERE, at stage 2, so that an omitted unit is still a
        // parse error rather than something resolution has to catch (§5.1).
        const std::size_t want = head == "bool" ? 0u : 1u;
        if (args.size() != want) {
            std::vector<Attachment> att;
            if (want == 0)
                att.push_back(note("`bool` takes no unit: a scaled or dimensioned truth "
                                   "value is meaningless (\xc2\xa7""5.1)"));
            else
                att.push_back(note("the unit is not optional; `(-)` spells dimensionless "
                                   "(\xc2\xa7""5.1)"));
            if (want == 1 && args.empty())
                att.push_back(help("write `" + std::string(what) + ": " + head +
                                   "(-)` if it really is dimensionless"));
            err("SE0205", args.empty() ? out.loc : args_loc,
                "`" + head + "` takes " +
                    (want == 0 ? "no unit argument" : "one unit argument") + ", found " +
                    std::to_string(args.size()),
                want == 0 ? "expected `" + head + "`" : "expected `" + head + "(unit)`",
                std::move(att));
            return false;
        }
        // `bool` has no argument to take the unit from, so it is dimensionless
        // by construction — the default `UnitExpr` kind.
        if (want == 1) {
            out.unit = std::move(args[0]);
        } else {
            out.unit = std::make_unique<ast::UnitExpr>();
            out.unit->loc = out.loc;
        }
        return true;
    }

    // A one-segment near-miss on the closed scalar set is a typo, not a record
    // nobody declared. Say so before resolution goes looking for a file.
    if (one_segment) {
        const std::string did = closest(head, scalar_types(), 2);
        if (!did.empty()) {
            err("SE0207", out.loc, "unknown scalar type `" + head + "`", "",
                {help("did you mean `" + did + "`?"),
                 note("the scalar type set is closed: `double`, `float`, `int`, "
                      "`bool` (\xc2\xa7""5.1)")});
            return false;
        }
    }

    out.is_scalar = false;
    out.unit_args = std::move(args);
    out.unit_args_loc = args_loc;
    return true;
}

// ─── Expressions (§7.1) ──────────────────────────────────────────────────────

ast::ExprPtr Parser::parse_expr() { return parse_additive(); }

ast::ExprPtr Parser::parse_additive() {
    ast::ExprPtr left = parse_multiplicative();
    if (!left) return nullptr;
    while (at(Tok::Plus) || at(Tok::Minus)) {
        const char op = at(Tok::Plus) ? '+' : '-';
        const Loc loc = tok_.loc;
        advance();
        ast::ExprPtr right = parse_multiplicative();
        if (!right) return nullptr;
        auto n = std::make_unique<ast::Expr>();
        n->kind = ast::Expr::Kind::Binary;
        n->op = op;
        n->loc = loc;
        n->lhs = std::move(left);
        n->rhs = std::move(right);
        left = std::move(n);
    }
    return left;
}

ast::ExprPtr Parser::parse_multiplicative() {
    ast::ExprPtr left = parse_unary();
    if (!left) return nullptr;
    while (at(Tok::Star) || at(Tok::Slash)) {
        const char op = at(Tok::Star) ? '*' : '/';
        const Loc loc = tok_.loc;
        advance();
        ast::ExprPtr right = parse_unary();
        if (!right) return nullptr;
        auto n = std::make_unique<ast::Expr>();
        n->kind = ast::Expr::Kind::Binary;
        n->op = op;
        n->loc = loc;
        n->lhs = std::move(left);
        n->rhs = std::move(right);
        left = std::move(n);
    }
    return left;
}

ast::ExprPtr Parser::parse_unary() {
    if (at(Tok::Plus) || at(Tok::Minus)) {
        const char op = at(Tok::Plus) ? '+' : '-';
        const Loc loc = tok_.loc;
        advance();
        ast::ExprPtr operand = parse_unary();
        if (!operand) return nullptr;
        auto n = std::make_unique<ast::Expr>();
        n->kind = ast::Expr::Kind::Unary;
        n->op = op;
        n->loc = loc;
        n->lhs = std::move(operand);
        return n;
    }
    return parse_power();
}

ast::ExprPtr Parser::parse_power() {
    ast::ExprPtr base = parse_primary();
    if (!base) return nullptr;
    if (!at(Tok::Caret)) return base;
    const Loc loc = tok_.loc;
    advance();
    // Right-associative, and binds tighter than unary minus: -2^2 is -4.
    ast::ExprPtr exp = parse_unary();
    if (!exp) return nullptr;
    auto n = std::make_unique<ast::Expr>();
    n->kind = ast::Expr::Kind::Binary;
    n->op = '^';
    n->loc = loc;
    n->lhs = std::move(base);
    n->rhs = std::move(exp);
    return n;
}

ast::ExprPtr Parser::parse_primary() {
    if (swallow_invalid()) return nullptr;

    auto e = std::make_unique<ast::Expr>();
    e->loc = tok_.loc;

    if (at(Tok::Number)) {
        e->kind = ast::Expr::Kind::Number;
        e->text = tok_.text;
        e->number = std::strtod(tok_.text.c_str(), nullptr);
        advance();
        // An optional unit suffix: `1500 (kg)`. There is no implicit
        // multiplication, so a `(` after a literal is always a unit.
        if (at(Tok::LParen)) {
            e->unit = parse_unit();
            if (!e->unit) return nullptr;
        }
        return e;
    }

    if (at(Tok::LParen)) {
        advance();
        ast::ExprPtr inner = parse_expr();
        if (!inner) return nullptr;
        if (!expect(Tok::RParen, "SE0201", "to close the expression")) return nullptr;
        return inner;
    }

    // §6.2c — a record brace list. It is NOT a value in the sense of §7.2:
    // nothing evaluates it, and no aggregate ever appears in the `Value` the
    // elaborator computes. It is destructured against the target's field tree at
    // the declaration site, and every leaf it yields is an ordinary scalar
    // expression. That is what lets a setting be a record while §7 stays scalar.
    //
    // The two spellings are told apart by one token of lookahead — `IDENT =` is
    // named, anything else is positional — and a list must be wholly one or the
    // other (SE0243). An empty `{}` is a positional list of nothing, which only a
    // fieldless record could accept; the arity check is what reports it.
    if (at(Tok::LBrace)) {
        e->kind = ast::Expr::Kind::Aggregate;
        advance();
        bool named = false;
        bool first = true;
        bool mixed = false;
        while (!at(Tok::RBrace) && !at_end()) {
            const bool this_named = at(Tok::Ident) && peek().kind == Tok::Equal;
            if (first) {
                named = this_named;
                first = false;
            } else if (this_named != named && !mixed) {
                err("SE0243", tok_.loc,
                    "a brace list mixes positional and named fields",
                    this_named ? "named, where the list began positional"
                               : "positional, where the list began named",
                    {note("a brace list is wholly positional or wholly named; the two "
                          "spellings desugar identically but may not be combined "
                          "(\xc2\xa7""6.2c)")});
                mixed = true;
            }

            Loc floc = tok_.loc;
            if (this_named) {
                e->field_names.push_back(tok_.text);
                advance();  // the field name
                advance();  // =
            }
            ast::ExprPtr v = parse_expr();
            if (!v) return nullptr;
            e->field_locs.push_back(floc);
            e->args.push_back(std::move(v));

            if (!accept(Tok::Comma)) break;
        }
        // Consume the closing brace even when the list was rejected, so the
        // token stream stays balanced and recovery resumes at the `;` rather
        // than mistaking this `}` for the end of the enclosing section.
        if (!expect(Tok::RBrace, "SE0201", "to close the brace list")) return nullptr;
        return mixed ? nullptr : std::move(e);
    }

    if (at(Tok::Ident)) {
        const std::string name = tok_.text;
        const Loc name_loc = tok_.loc;

        if (name == "param" && peek().kind == Tok::Dot) {
            advance();  // param
            advance();  // .
            if (!at(Tok::Ident)) {
                err("SE0201", tok_.loc,
                    std::string("expected a setting name after `param.`, found ") +
                        describe(tok_.kind),
                    "expected a setting name");
                return nullptr;
            }
            e->kind = ast::Expr::Kind::Param;
            e->text = tok_.text;
            e->loc = name_loc;
            e->loc.length = tok_.loc.offset + tok_.loc.length - name_loc.offset;
            advance();
            // §6.2c — further segments reach into a record setting:
            // `param.offset.x`. They accumulate into `text` as a dotted string, so
            // a leaf is an ordinary node in the §6.2 dependency sort and renders
            // straight into C++ member access at stage 6. Whether the segments name
            // real fields is the elaborator's job, not the parser's.
            while (at(Tok::Dot) && peek().kind == Tok::Ident) {
                advance();  // .
                e->text += "." + tok_.text;
                e->loc.length = tok_.loc.offset + tok_.loc.length - name_loc.offset;
                advance();
            }
            return e;
        }

        if (peek().kind == Tok::LParen) {
            advance();  // name
            advance();  // (
            e->kind = ast::Expr::Kind::Call;
            e->text = name;
            if (!at(Tok::RParen)) {
                for (;;) {
                    ast::ExprPtr arg = parse_expr();
                    if (!arg) return nullptr;
                    e->args.push_back(std::move(arg));
                    if (!accept(Tok::Comma)) break;
                }
            }
            if (!expect(Tok::RParen, "SE0201", "to close the argument list")) return nullptr;
            return e;
        }

        e->kind = ast::Expr::Kind::Name;
        e->text = name;
        advance();
        return e;
    }

    std::vector<Attachment> att;
    if (at(Tok::KwSelf) || tok_.is_keyword())
        att.push_back(note("`" + tok_.text + "` is a reserved word (Appendix B)"));
    att.push_back(note("the elaboration expression language has literals, `param.*`, "
                       "arithmetic and builtin math — no comparisons, no run-time "
                       "signals (\xc2\xa7""7.4)"));
    err("SE0201", tok_.loc, std::string("expected an expression, found ") + describe(tok_.kind),
        "expected an expression", std::move(att));
    return nullptr;
}

// ─── Setting bindings ────────────────────────────────────────────────────────

bool Parser::parse_bindings(std::vector<ast::Binding>& out) {
    if (!expect(Tok::LBrace, "SE0201", "to open the setting bindings")) return false;

    while (!at(Tok::RBrace) && !at_end()) {
        const std::uint32_t before = tok_.loc.offset;
        ast::Binding b;

        if (!at(Tok::Ident)) {
            std::vector<Attachment> att;
            if (tok_.is_keyword())
                att.push_back(note("`" + tok_.text + "` is a reserved word (Appendix B)"));
            err("SE0201", tok_.loc,
                std::string("expected a setting name, found ") + describe(tok_.kind),
                "expected a setting name", std::move(att));
            recover_in_list(Tok::RBrace);
        } else {
            b.name = tok_.text;
            b.loc = tok_.loc;
            advance();
            if (!expect(Tok::Equal, "SE0201", "after the setting name")) {
                recover_in_list(Tok::RBrace);
            } else if ((b.value = parse_expr()) == nullptr) {
                recover_in_list(Tok::RBrace);
            } else if (!expect(Tok::Semi, "SE0208", "after the binding")) {
                recover_in_list(Tok::RBrace);
            } else {
                out.push_back(std::move(b));
            }
        }

        if (give_up()) return false;
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close the setting bindings");
}

// ─── Model file (§2.5) ───────────────────────────────────────────────────────

std::unique_ptr<ast::ModelFile> Parser::parse_model_file() {
    auto file = std::make_unique<ast::ModelFile>();
    advance();

    if (at(Tok::KwPackage)) {
        advance();
        if (parse_path(file->package, false)) file->has_package = true;
        expect(Tok::Semi, "SE0208", "after the package path");
    } else {
        err("SE0202", tok_.loc, "missing `package` declaration",
            "expected `package` here",
            {note("every `.se` file begins with a `package` declaration, checked "
                  "against the file's directory (\xc2\xa7""2.2)")});
    }

    while (at(Tok::KwUse) && !give_up()) {
        ast::UseDecl u;
        u.loc = tok_.loc;
        advance();
        if (parse_path(u.path, false)) {
            file->uses.push_back(std::move(u));
            expect(Tok::Semi, "SE0208", "after the `use` path");
        } else {
            recover_in_list(Tok::End);
        }
    }

    while (!at_end() && !give_up()) {
        const std::uint32_t before = tok_.loc.offset;

        // §6.15 — annotations lead the definition they annotate, and the
        // definition begins at the first of them.
        std::vector<ast::Annotation> annots;
        if (at(Tok::At)) {
            if (!parse_annotations(annots)) {
                recover_to_definition();
                continue;
            }
            if (!at(Tok::KwType) && !at(Tok::KwFinite) && !at(Tok::KwNode)) {
                stray_annotations(annots,
                                  "expected a `node`, `type` or `finite` definition to follow");
                continue;
            }
        }
        const std::uint32_t first_line = annots.empty() ? 0 : annots.front().loc.line;

        if (at(Tok::KwType)) {
            ast::TypeDef t;
            ast::DiagramHints none;
            apply_annotations(annots, AnnotSite::Type, none);
            if (parse_type_def(t)) {
                t.start_line = first_line ? first_line : t.loc.line;
                file->types.push_back(std::move(t));
            } else {
                recover_to_definition();
            }
        } else if (at(Tok::KwFinite)) {
            ast::EnumDef e;
            ast::DiagramHints none;
            apply_annotations(annots, AnnotSite::Enum, none);
            if (parse_enum_def(e)) {
                e.start_line = first_line ? first_line : e.loc.line;
                file->enums.push_back(std::move(e));
            } else {
                recover_to_definition();
            }
        } else if (at(Tok::KwNode)) {
            ast::NodeDef n;
            apply_annotations(annots, AnnotSite::Definition, n.diagram);
            n.annotations = std::move(annots);
            if (parse_node_def(n)) {
                n.start_line = first_line ? first_line : n.loc.line;
                file->nodes.push_back(std::move(n));
            } else {
                recover_to_definition();
            }
        } else if (at(Tok::KwBuild)) {
            // A package manifest's file-level build block (§2.6).
            const Loc loc = tok_.loc;
            if (!mark_section(file->sec_build, "build", loc)) {
                recover_to_definition();
            } else {
                advance();
                if (!parse_build_list(file->build)) recover_to_definition();
            }
        } else if (at(Tok::KwUse)) {
            err("SE0203", tok_.loc, "`use` declaration after a definition",
                "move this above the definitions",
                {note("declaration order is `package`, then all `use`s, then the "
                      "definitions (\xc2\xa7""2.5)")});
            advance();
            ast::Path p;
            parse_path(p, false);
            accept(Tok::Semi);
        } else {
            std::vector<Attachment> att;
            att.push_back(note("a `.se` file contains `node`, `type` and `finite` "
                               "declarations (\xc2\xa7""2.3)"));
            if (at(Tok::KwSim))
                att.push_back(help("a `sim` declaration belongs in its own `.sim` file "
                                   "(\xc2\xa7""13)"));
            err("SE0204", tok_.loc,
                std::string("expected a definition (`node`, `type` or `finite`), found ") +
                    describe(tok_.kind),
                "expected `node`, `type` or `finite`", std::move(att));
            recover_to_definition();
        }

        if (tok_.loc.offset == before && !at_end()) advance();
    }
    file->doc_lines = lexer_.doc_lines();
    return file;
}

bool Parser::parse_type_def(ast::TypeDef& out) {
    out.loc = tok_.loc;
    advance();  // type
    if (!at(Tok::Ident)) {
        err("SE0201", tok_.loc, std::string("expected a type name, found ") + describe(tok_.kind),
            "expected a type name");
        return false;
    }
    out.name = tok_.text;
    out.loc = tok_.loc;
    advance();
    // A record body is a field list that may also carry one `units` section
    // (§5.2), so it shares the field loop rather than duplicating its recovery.
    return parse_field_list(out.fields, "record field", true, false, &out.sec_units,
                            &out.unit_params);
}

// §5.2a — `finite Name { A, B = 15, C }`. The values are settled here, so every
// later stage reads a finished list; the duplicate and empty checks belong to
// resolution (SE0317-19), which reports them against the whole declaration.
bool Parser::parse_enum_def(ast::EnumDef& out) {
    out.loc = tok_.loc;
    advance();  // finite
    if (!at(Tok::Ident)) {
        err("SE0201", tok_.loc, std::string("expected a type name, found ") + describe(tok_.kind),
            "expected a type name");
        return false;
    }
    out.name = tok_.text;
    out.loc = tok_.loc;
    advance();
    if (!expect(Tok::LBrace, "SE0201", "to open the enumerator list")) return false;

    long next = 0;
    while (!at(Tok::RBrace) && !at_end() && !give_up()) {
        if (!at(Tok::Ident)) {
            err("SE0201", tok_.loc,
                std::string("expected an enumerator name, found ") + describe(tok_.kind),
                "expected an enumerator name",
                {note("a finite type is a comma-separated list of names, each optionally "
                      "`= n` (\xc2\xa7""5.2a)")});
            return false;
        }
        ast::Enumerator en;
        en.name = tok_.text;
        en.loc = tok_.loc;
        advance();
        if (accept(Tok::Equal)) {
            const bool negate = accept(Tok::Minus);
            if (!at(Tok::Number) ||
                tok_.text.find_first_not_of("0123456789") != std::string::npos) {
                err("SE0201", tok_.loc,
                    "expected an integer value, found " +
                        (at(Tok::Number) ? "`" + tok_.text + "`" : describe(tok_.kind)),
                    "expected an integer",
                    {note("an enumerator's value is an integer literal, optionally "
                          "negative (\xc2\xa7""5.2a)")});
                return false;
            }
            const long v = std::strtol(tok_.text.c_str(), nullptr, 10);
            next = negate ? -v : v;
            en.explicit_value = true;
            advance();
        }
        en.value = next++;
        out.enumerators.push_back(std::move(en));
        if (!accept(Tok::Comma)) break;
    }
    return expect(Tok::RBrace, "SE0201", "to close the enumerator list");
}

bool Parser::mark_section(ast::SectionMark& m, const char* name, Loc loc) {
    if (m.present) {
        err("SE0220", loc, std::string("duplicate `") + name + "` section",
            "already declared",
            {note(std::string("`") + name + "` first appears on line " +
                  std::to_string(m.loc.line)),
             note("a section may appear at most once, so its contents are always "
                  "contiguous and reviewable in one place (\xc2\xa7""6)")});
        return false;
    }
    m.present = true;
    m.loc = loc;
    return true;
}

bool Parser::parse_node_def(ast::NodeDef& out) {
    advance();  // node
    if (!at(Tok::Ident)) {
        err("SE0201", tok_.loc, std::string("expected a node name, found ") + describe(tok_.kind),
            "expected a node name");
        return false;
    }
    out.name = tok_.text;
    out.loc = tok_.loc;
    advance();
    if (!expect(Tok::LBrace, "SE0201", "to open the node body")) return false;

    while (!at(Tok::RBrace) && !at_end() && !give_up()) {
        const std::uint32_t before = tok_.loc.offset;
        if (!parse_node_item(out)) recover_in_node_body();
        if (tok_.loc.offset == before && !at(Tok::RBrace) && !at_end()) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close the node body");
}

bool Parser::parse_node_item(ast::NodeDef& node) {
    const Loc loc = tok_.loc;

    switch (tok_.kind) {
        case Tok::KwSettings:
            if (!mark_section(node.sec_settings, "settings", loc)) return false;
            advance();
            return parse_setting_list(node.settings);

        case Tok::KwInputs:
            if (!mark_section(node.sec_inputs, "inputs", loc)) return false;
            advance();
            return parse_field_list(node.inputs, "input port", true, false);

        case Tok::KwOutputs:
            if (!mark_section(node.sec_outputs, "outputs", loc)) return false;
            advance();
            return parse_field_list(node.outputs, "output port", true, false);

        case Tok::KwVars:
            if (!mark_section(node.sec_vars, "vars", loc)) return false;
            advance();
            return parse_field_list(node.vars, "var", true, true);

        case Tok::KwUnits:
            if (!mark_section(node.sec_units, "units", loc)) return false;
            advance();
            return parse_unit_param_list(node.unit_params);

        case Tok::KwStates:
            if (!mark_section(node.sec_states, "states", loc)) return false;
            advance();
            return parse_state_list(node.states);

        case Tok::KwNative:
            if (!mark_section(node.sec_native, "native", loc)) return false;
            advance();
            return parse_native_list(node.natives);

        case Tok::KwBuild:
            if (!mark_section(node.sec_build, "build", loc)) return false;
            advance();
            return parse_build_list(node.build);

        case Tok::KwStructure:
            if (!mark_section(node.sec_structure, "structure", loc)) return false;
            advance();
            return parse_structure(node.structure);

        case Tok::KwDeclarations: {
            if (!mark_section(node.sec_declarations, "declarations", loc)) return false;
            advance();
            if (!at(Tok::LBrace)) {
                err("SE0201", tok_.loc,
                    std::string("expected `{` to open the `declarations` block, found ") +
                        describe(tok_.kind),
                    "expected `{`");
                return false;
            }
            enter_raw();
            const bool ok = lexer_.scan_brace_block(node.declarations);
            resync();
            return ok;
        }

        case Tok::KwInit:
        case Tok::KwOutput:
        case Tok::KwDerivative:
        case Tok::KwNext:
        case Tok::KwTransition:
        case Tok::KwAdjust:
        case Tok::KwOnStep:
        case Tok::KwFinal:
            return parse_method(node);

        case Tok::At: {
            // §6.15 — nothing inside a node body takes an annotation yet.
            std::vector<ast::Annotation> annots;
            if (!parse_annotations(annots)) return false;
            stray_annotations(annots, "nothing inside a node body takes one");
            return true;
        }

        case Tok::KwNode:
            err("SE0211", loc, "child instance outside a `structure` block",
                "`node` here declares a child instance",
                {note("a composite's children and wires live in `structure { ... }` "
                      "(\xc2\xa7""6.9)"),
                 help("wrap the instances and wires in `structure { ... }`")});
            return false;

        case Tok::KwSelf:
            err("SE0211", loc, "wire outside a `structure` block", "`self` here starts a wire",
                {note("a composite's children and wires live in `structure { ... }` "
                      "(\xc2\xa7""6.9)")});
            return false;

        case Tok::Ident: {
            // §11.4 — `template <class T> ...` is a helper, and a `<` is not a
            // token, so decide before lexing one: peek() would report it.
            if (!peeked_ && lexer_.peek_char() == '<') return parse_helper(node);
            const Tok after = peek().kind;
            // §6.13 — `tests` is contextual, not reserved: it is a section only
            // where a section can start.
            if (after == Tok::LBrace && tok_.text == "tests") {
                if (!mark_section(node.sec_tests, "tests", loc)) return false;
                advance();
                return parse_tests(node.tests);
            }
            if (after == Tok::LBrace) {
                // `state { ... }` — a mistyped section, not a C++ function.
                std::vector<Attachment> att;
                const std::string did = closest(tok_.text, section_names());
                if (!did.empty()) att.push_back(help("did you mean `" + did + "`?"));
                att.push_back(note("the sections are settings, inputs, outputs, states, "
                                   "vars, native, declarations, build, structure "
                                   "(\xc2\xa7""6)"));
                err("SE0221", loc, "unknown section `" + tok_.text + "`", "", std::move(att));
                return false;
            }
            if (after == Tok::Dot) {
                err("SE0211", loc, "wire outside a `structure` block", "this looks like a wire",
                    {note("a composite's children and wires live in `structure { ... }` "
                          "(\xc2\xa7""6.9)")});
                return false;
            }
            if (after == Tok::LParen) {
                // A bare `name(` at item position is a lifecycle typo far more often
                // than it is a legal C++ member definition.
                const std::string did = closest(tok_.text, lifecycle_names(), 2);
                if (!did.empty())
                    warn("SE0221", loc,
                         "`" + tok_.text + "` is not a lifecycle method; it will be copied "
                         "verbatim as a helper function",
                         "did you mean `" + did + "`?",
                         {note("the lifecycle methods are init, output, derivative, next, "
                               "transition, adjust, on_step, final (\xc2\xa7""8.1)")});
            }
            return parse_helper(node);
        }

        default:
            return parse_helper(node);
    }
}

// ─── Tests (§6.13) ───────────────────────────────────────────────────────────

bool Parser::parse_tests(ast::TestsSection& out) {
    if (!expect(Tok::LBrace, "SE0201", "to open the `tests` section")) return false;

    auto duplicate = [&](const char* what, Loc loc, Loc first) {
        err("SE0270", loc, std::string("duplicate `") + what + "` in `tests`",
            "already given",
            {note(std::string("`") + what + "` first appears on line " +
                  std::to_string(first.line)),
             note("the preamble is shared by every set; a set that needs a different "
                  "value names it as a column (\xc2\xa7""6.13)")});
    };

    while (!at(Tok::RBrace) && !at_end() && !give_up()) {
        const std::uint32_t before = tok_.loc.offset;
        const Loc loc = tok_.loc;

        if (at(Tok::Ident) && tok_.text == "step" && peek().kind == Tok::Colon) {
            if (out.has_step) {
                duplicate("step", loc, out.step_loc);
                recover_in_list(Tok::RBrace);
            } else {
                out.has_step = true;
                out.step_loc = loc;
                advance();
                advance();  // :
                if ((out.step = parse_expr()) == nullptr ||
                    !expect(Tok::Semi, "SE0208", "after `step`"))
                    recover_in_list(Tok::RBrace);
            }
        } else if (at(Tok::KwUnits)) {
            if (out.sec_units.present) {
                duplicate("units", loc, out.sec_units.loc);
                recover_in_list(Tok::RBrace);
            } else {
                out.sec_units.present = true;
                out.sec_units.loc = loc;
                advance();
                if (!expect(Tok::LBrace, "SE0201", "to open the unit bindings")) return false;
                while (!at(Tok::RBrace) && !at_end() && !give_up()) {
                    const std::uint32_t b2 = tok_.loc.offset;
                    ast::UnitBinding ub;
                    if (!at(Tok::Ident)) {
                        err("SE0201", tok_.loc,
                            std::string("expected a unit parameter name, found ") +
                                describe(tok_.kind),
                            "expected a unit parameter name");
                        recover_in_list(Tok::RBrace);
                    } else {
                        ub.name = tok_.text;
                        ub.loc = tok_.loc;
                        advance();
                        if (!expect(Tok::Equal, "SE0201", "after the unit parameter name")) {
                            recover_in_list(Tok::RBrace);
                        } else if ((ub.unit = at(Tok::LParen) ? parse_unit()
                                                              : parse_unit_expr()) == nullptr) {
                            recover_in_list(Tok::RBrace);
                        } else if (!expect(Tok::Semi, "SE0208", "after the unit binding")) {
                            recover_in_list(Tok::RBrace);
                        } else {
                            out.units.push_back(std::move(ub));
                        }
                    }
                    if (tok_.loc.offset == b2 && !at(Tok::RBrace)) advance();
                }
                if (!expect(Tok::RBrace, "SE0201", "to close the unit bindings")) return false;
            }
        } else if (at(Tok::KwSettings)) {
            if (out.sec_settings.present) {
                duplicate("settings", loc, out.sec_settings.loc);
                recover_in_list(Tok::RBrace);
            } else {
                out.sec_settings.present = true;
                out.sec_settings.loc = loc;
                advance();
                if (!parse_bindings(out.settings)) recover_in_list(Tok::RBrace);
            }
        } else if (at(Tok::Ident) && tok_.text == "set") {
            ast::TestSet set;
            if (parse_test_set(set)) {
                for (const ast::TestSet& s : out.sets) {
                    if (s.claim != set.claim) continue;
                    err("SE0270", set.claim_loc,
                        "duplicate set \"" + set.claim + "\" in `tests`", "already claimed",
                        {note("first claimed on line " + std::to_string(s.claim_loc.line)),
                         note("a claim names its set in every failure report, so two sets "
                              "with one claim could not be told apart (\xc2\xa7""6.13)")});
                    break;
                }
                out.sets.push_back(std::move(set));
            } else {
                recover_in_list(Tok::RBrace);
            }
        } else {
            err("SE0271", loc, std::string("unknown entry in `tests`: ") + describe(tok_.kind) +
                                   (tok_.kind == Tok::Ident ? " `" + tok_.text + "`" : ""),
                "not a `tests` entry",
                {note("a `tests` section holds `step:`, `units { }`, `settings { }` and "
                      "`set \"claim\", ... { rows }` (\xc2\xa7""6.13)")});
            recover_in_list(Tok::RBrace);
        }

        if (tok_.loc.offset == before && !at(Tok::RBrace) && !at_end()) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close the `tests` section");
}

bool Parser::parse_test_column(ast::TestColumn& out) {
    out.loc = tok_.loc;
    if (at(Tok::Ident) && tok_.text == "step" && peek().kind != Tok::Dot) {
        out.is_step = true;
        advance();
        return true;
    }
    static const std::vector<const char*> namespaces = {
        "param", "in", "state", "out", "der", "next", "transition", "adjust", "var"};
    if (!tok_.is_word() || !contains(namespaces, tok_.text)) {
        err("SE0201", tok_.loc,
            std::string("expected a column, found ") + describe(tok_.kind),
            "expected `step` or an accessor",
            {note("a column is `step` or one of param. in. state. out. der. next. "
                  "transition. adjust. var. (\xc2\xa7""6.13)")});
        return false;
    }
    out.ns = tok_.text;
    advance();
    if (!expect(Tok::Dot, "SE0201", "after the accessor namespace")) return false;
    do {
        if (!tok_.is_word()) {
            err("SE0201", tok_.loc,
                std::string("expected a member name, found ") + describe(tok_.kind),
                "expected a name");
            return false;
        }
        out.segs.push_back(tok_.text);
        out.loc.length = tok_.loc.offset + tok_.loc.length - out.loc.offset;
        advance();
        if (at(Tok::LBracket)) {
            advance();
            if (!at(Tok::Number) ||
                tok_.text.find_first_not_of("0123456789") != std::string::npos) {
                err("SE0201", tok_.loc,
                    std::string("expected a non-negative integer index, found ") +
                        describe(tok_.kind),
                    "expected an index");
                return false;
            }
            out.segs.back() += "[" + tok_.text + "]";
            advance();
            const Loc close = tok_.loc;
            if (!expect(Tok::RBracket, "SE0201", "to close the index")) return false;
            out.loc.length = close.offset + close.length - out.loc.offset;
        }
    } while (accept(Tok::Dot));
    return true;
}

bool Parser::parse_test_set(ast::TestSet& out) {
    out.loc = tok_.loc;
    advance();  // set
    if (!at(Tok::String)) {
        err("SE0201", tok_.loc, std::string("expected the set's claim, found ") +
                                    describe(tok_.kind),
            "expected a string",
            {note("a set is named by the claim its rows demonstrate (\xc2\xa7""6.13)")});
        return false;
    }
    out.claim = tok_.text;
    out.claim_loc = tok_.loc;
    advance();

    // The given columns run up to the operator, the expected columns up to
    // `within` or the opening brace.
    bool expected_side = false;
    for (;;) {
        if (!expect(Tok::Comma, "SE0201", expected_side ? nullptr : "after the claim"))
            return false;
        ast::TestColumn col;
        if (!parse_test_column(col)) return false;
        (expected_side ? out.expected : out.given).push_back(std::move(col));

        if (at(Tok::Comma)) continue;
        if (!expected_side && (at(Tok::Equal) || at(Tok::TildeEqual))) {
            out.approx = at(Tok::TildeEqual);
            out.op_loc = tok_.loc;
            advance();
            ast::TestColumn first;
            if (!parse_test_column(first)) return false;
            out.expected.push_back(std::move(first));
            expected_side = true;
            if (at(Tok::Comma)) continue;
        }
        break;
    }
    if (!expected_side) {
        err("SE0201", tok_.loc,
            std::string("expected `=` or `~=` in the set header, found ") + describe(tok_.kind),
            "expected `=` or `~=`",
            {note("the operator divides what is supplied from what is checked "
                  "(\xc2\xa7""6.13)")});
        return false;
    }

    Loc within_loc;
    if (at(Tok::Ident) && tok_.text == "within") {
        within_loc = tok_.loc;
        advance();
        if ((out.within = parse_expr()) == nullptr) return false;
    }
    if (out.approx && !out.within) {
        err("SE0272", out.op_loc, "`~=` with no `within`", "no tolerance given",
            {note("there is no default tolerance anywhere (\xc2\xa7""6.13)"),
             help("close the header with `within <tolerance>`")});
    } else if (!out.approx && out.within) {
        err("SE0272", within_loc, "`=` with a `within` clause", "`=` compares bitwise",
            {help("use `~=` for a toleranced comparison")});
    }

    if (!expect(Tok::LBrace, "SE0201", "to open the set's rows")) return false;
    const std::size_t arity = out.given.size() + out.expected.size();
    while (!at(Tok::RBrace) && !at_end() && !give_up()) {
        const std::uint32_t before = tok_.loc.offset;
        ast::TestRow row;
        if (parse_test_row(row)) {
            // An unlabelled row has already been reported (SE0274): one mistake,
            // one diagnostic.
            if (row.label_loc.length != 0 && row.cells.size() != arity) {
                err("SE0273", row.loc,
                    "row has " + std::to_string(row.cells.size()) + " value(s), but the set "
                        "header names " + std::to_string(arity) + " column(s)",
                    "wrong number of values",
                    {note("every row supplies every column; a case that wants a different "
                          "shape wants a different set (\xc2\xa7""6.13)")});
            }
            out.rows.push_back(std::move(row));
        } else {
            recover_in_list(Tok::RBrace);
        }
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close the set's rows");
}

bool Parser::parse_test_row(ast::TestRow& out) {
    out.loc = tok_.loc;
    for (;;) {
        if (at(Tok::String)) {
            out.label = tok_.text;
            out.label_loc = tok_.loc;
            advance();
            return expect(Tok::Semi, "SE0208", "after the row's label");
        }
        ast::ExprPtr cell = parse_expr();
        if (!cell) return false;
        out.loc.length = tok_.loc.offset - out.loc.offset;
        out.cells.push_back(std::move(cell));
        if (accept(Tok::Comma)) continue;
        if (at(Tok::Semi)) {
            err("SE0274", out.loc, "row has no label", "expected `, \"label\"` before `;`",
                {note("the label is the worked example's name, and what a failure report "
                      "leads with (\xc2\xa7""6.13)")});
            // The row is still well-formed up to here, so keep it: recovering
            // past the `;` just consumed would swallow the NEXT row.
            advance();
            return true;
        }
        return expect(Tok::Comma, "SE0201", "between a row's values");
    }
}

// ─── Declaration lists ───────────────────────────────────────────────────────

bool Parser::parse_field_list(std::vector<ast::FieldDecl>& out, const char* what,
                              bool allow_records, bool allow_extent,
                              ast::SectionMark* sec_units,
                              std::vector<ast::UnitParamDecl>* unit_params) {
    if (!expect(Tok::LBrace, "SE0201", "to open the section")) return false;

    while (!at(Tok::RBrace) && !at_end()) {
        const std::uint32_t before = tok_.loc.offset;
        ast::FieldDecl f;

        // Only a record body passes these; a port section has no `units` of its
        // own, since a node declares its parameters at node level (§6.2a).
        if (sec_units && at(Tok::KwUnits)) {
            const Loc uloc = tok_.loc;
            if (!mark_section(*sec_units, "units", uloc)) return false;
            advance();
            if (!parse_unit_param_list(*unit_params)) return false;
        } else if (!at(Tok::Ident)) {
            std::vector<Attachment> att;
            if (tok_.is_keyword())
                att.push_back(note("`" + tok_.text + "` is a reserved word (Appendix B)"));
            err("SE0201", tok_.loc,
                std::string("expected a ") + what + " name, found " + describe(tok_.kind),
                std::string("expected a ") + what + " name", std::move(att));
            recover_in_list(Tok::RBrace);
        } else {
            f.name = tok_.text;
            f.loc = tok_.loc;
            advance();
            if (!parse_extent(f.extent, f.extent_loc, what, allow_extent)) {
                recover_in_list(Tok::RBrace);
            } else if (!parse_type_ref(f.type, what)) {
                recover_in_list(Tok::RBrace);
            } else if (!allow_records && !f.type.is_scalar) {
                err("SE0231", f.type.loc, std::string("a ") + what + " may not be a record type");
                recover_in_list(Tok::RBrace);
            } else if (!expect(Tok::Semi, "SE0208", "after the declaration")) {
                recover_in_list(Tok::RBrace);
            } else {
                out.push_back(std::move(f));
            }
        }

        if (give_up()) return false;
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close the section");
}

// §6.4a — the extent on a state or a var: `w1 [param.nbq] (-): double`.
//
// It is parsed wherever one could be written, including the sites that forbid
// it, so that `x [3] (m): double;` on a port is rejected by SE0234 with the
// reason, rather than by a pile-up of punctuation errors that never mentions
// arrays. The expression itself is an ordinary §7 expression: it is not
// evaluated until configuration (§6.2b step 4), so `param.*` is legal here.
bool Parser::parse_extent(ast::ExprPtr& out, Loc& loc, const char* what, bool allowed) {
    if (!at(Tok::LBracket)) return true;
    Loc open = tok_.loc;
    advance();
    ast::ExprPtr e = parse_expr();
    if (!e) return false;
    const Loc close = tok_.loc;
    if (!expect(Tok::RBracket, "SE0201", "to close the extent")) return false;
    // The whole `[...]` span, so the manifest can print the extent AS WRITTEN
    // (§15.6): a reader of a dynamic model gets the expression, not a size,
    // because there is no size until configuration.
    open.length = close.offset + close.length - open.offset;
    if (!allowed) {
        const char* article =
            std::strchr("aeiou", what[0]) != nullptr ? "an " : "a ";
        // A setting is scalar for a different reason than a port is, so say
        // whichever one applies rather than a note that is true of neither.
        const bool setting = std::strcmp(what, "setting") == 0;
        err("SE0234", open, std::string(article) + what + " may not carry an extent",
            "arrays are not permitted here",
            {note(setting ? "a setting is a scalar: the flow-down expression language "
                            "is scalar (\xc2\xa7""5.2, \xc2\xa7""6.2)"
                          : "a port and a record field both live in the signal block, "
                            "whose offsets come from `offsetof` — a run-time-sized "
                            "member would destroy that (\xc2\xa7""15.5)"),
             note("only a state and a var may be array-shaped (\xc2\xa7""6.4a)"),
             help(setting ? "declare the COUNT as a setting and put the extent on the "
                            "state or var that reads it"
                          : "cross a wire as N ports or as a fixed record instead")});
        return false;
    }
    loc = open;
    out = std::move(e);
    return true;
}

// `units { U; V; }` — bare names, no type and no default. A unit parameter
// ranges over units rather than values, so there is nothing else to declare.
bool Parser::parse_unit_param_list(std::vector<ast::UnitParamDecl>& out) {
    if (!expect(Tok::LBrace, "SE0201", "to open `units`")) return false;

    while (!at(Tok::RBrace) && !at_end()) {
        const std::uint32_t before = tok_.loc.offset;

        if (!at(Tok::Ident)) {
            std::vector<Attachment> att;
            if (tok_.is_keyword())
                att.push_back(note("`" + tok_.text + "` is a reserved word (Appendix B)"));
            err("SE0201", tok_.loc,
                std::string("expected a unit parameter name, found ") + describe(tok_.kind),
                "expected a unit parameter name", std::move(att));
            recover_in_list(Tok::RBrace);
        } else {
            ast::UnitParamDecl p;
            p.name = tok_.text;
            p.loc = tok_.loc;
            advance();
            if (!expect(Tok::Semi, "SE0208", "after the unit parameter")) {
                recover_in_list(Tok::RBrace);
            } else {
                out.push_back(std::move(p));
            }
        }

        if (give_up()) return false;
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close `units`");
}

bool Parser::parse_setting_list(std::vector<ast::SettingDecl>& out) {
    if (!expect(Tok::LBrace, "SE0201", "to open `settings`")) return false;

    while (!at(Tok::RBrace) && !at_end()) {
        const std::uint32_t before = tok_.loc.offset;
        ast::SettingDecl s;
        bool ok = true;

        if (!at(Tok::Ident)) {
            std::vector<Attachment> att;
            if (tok_.is_keyword())
                att.push_back(note("`" + tok_.text + "` is a reserved word (Appendix B)"));
            err("SE0201", tok_.loc,
                std::string("expected a setting name, found ") + describe(tok_.kind),
                "expected a setting name", std::move(att));
            ok = false;
        } else {
            s.name = tok_.text;
            s.loc = tok_.loc;
            advance();
            ast::ExprPtr unused_extent;
            Loc unused_loc;
            if (!parse_extent(unused_extent, unused_loc, "setting", false)) {
                ok = false;
            } else if (!parse_type_ref(s.type, "setting")) {
                ok = false;
            } else if (at(Tok::Equal)) {
                advance();
                s.default_value = parse_expr();
                if (!s.default_value) ok = false;
            }
            if (ok && !expect(Tok::Semi, "SE0208", "after the setting")) ok = false;
        }

        if (ok) out.push_back(std::move(s));
        else recover_in_list(Tok::RBrace);

        if (give_up()) return false;
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close `settings`");
}

bool Parser::parse_state_list(std::vector<ast::StateDecl>& out) {
    if (!expect(Tok::LBrace, "SE0201", "to open `states`")) return false;

    while (!at(Tok::RBrace) && !at_end()) {
        const std::uint32_t before = tok_.loc.offset;
        ast::StateDecl s;
        bool ok = true;
        bool have_kind = false;

        if (at(Tok::KwContinuous) || at(Tok::KwDiscrete) || at(Tok::KwFinite)) {
            s.kind = at(Tok::KwContinuous) ? ast::StateKind::Continuous
                     : at(Tok::KwDiscrete) ? ast::StateKind::Discrete
                                           : ast::StateKind::Finite;
            s.kind_loc = tok_.loc;
            have_kind = true;
            advance();
        }

        if (!at(Tok::Ident)) {
            err("SE0201", tok_.loc,
                std::string("expected a state name, found ") + describe(tok_.kind),
                "expected a state name");
            ok = false;
        } else {
            if (!have_kind) {
                // §6.4: omitting the kind is an error, never a default.
                err("SE0230", tok_.loc,
                    "state `" + tok_.text +
                        "` must be declared `continuous`, `discrete` or `finite`",
                    "expected `continuous`, `discrete` or `finite` before the state name",
                    {note("there is deliberately no default state kind — the three differ in "
                          "write accessor (`der.`, `next.`, `transition.`), in schedule, and "
                          "in whether they enter the solver's state vector (\xc2\xa7""6.4)"),
                     help("prefix the declaration with `continuous`, `discrete` or "
                          "`finite`")});
                ok = false;
            }
            s.name = tok_.text;
            s.loc = tok_.loc;
            advance();
            if (!parse_extent(s.extent, s.extent_loc, "state", true)) {
                ok = false;
            } else if (!parse_type_ref(s.type, "state")) {
                ok = false;
            } else {
                // A named type here may be a record (SE0232) or an enum (legal,
                // §5.2a), and only resolution can tell which.
                if (at(Tok::Equal)) {
                    advance();
                    s.initial = parse_expr();
                    if (!s.initial) ok = false;
                }
                if (ok && !expect(Tok::Semi, "SE0208", "after the state")) ok = false;
            }
        }

        // A state with no kind is not recorded: downstream stages have no
        // representation for "unknown kind", and guessing would be worse.
        if (ok) out.push_back(std::move(s));
        else recover_in_list(Tok::RBrace);

        if (give_up()) return false;
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close `states`");
}

bool Parser::parse_native_list(std::vector<ast::NativeDecl>& out) {
    if (!expect(Tok::LBrace, "SE0201", "to open `native`")) return false;

    while (!at(Tok::RBrace) && !at_end()) {
        const std::uint32_t before = tok_.loc.offset;
        ast::NativeDecl n;
        bool ok = true;

        if (!at(Tok::Ident)) {
            err("SE0201", tok_.loc,
                std::string("expected a native member name, found ") + describe(tok_.kind),
                "expected a name");
            ok = false;
        } else {
            n.name = tok_.text;
            n.loc = tok_.loc;
            advance();

            if (at(Tok::LParen)) {
                err("SE0233", tok_.loc, "a `native` member may not carry a unit", "",
                    {note("natives are not quantities: no unit, not recordable, never in "
                          "the state vector, outside the override channel (\xc2\xa7""6.7)")});
                ok = false;
            } else if (!expect(Tok::Colon, "SE0201", "after the native member name")) {
                ok = false;
            } else {
                // §11.3: verbatim C++ type text, scanned to the `;`.
                enter_raw();
                Verbatim v;
                if (!lexer_.scan_to_semi(v)) {
                    resync();
                    ok = false;
                } else {
                    resync();
                    if (v.text.empty()) {
                        err("SE0201", v.open, "expected a C++ type after `:`", "",
                            {note("`native` type text is copied verbatim up to the `;` "
                                  "(\xc2\xa7""11.3)")});
                        ok = false;
                    }
                    n.cpp_type = std::move(v.text);
                    n.type_loc = v.open;
                    if (ok && !expect(Tok::Semi, "SE0208", "after the native type")) ok = false;
                }
            }
        }

        if (ok) out.push_back(std::move(n));
        else recover_in_list(Tok::RBrace);

        if (give_up()) return false;
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close `native`");
}

bool Parser::parse_build_list(std::vector<ast::BuildStmt>& out) {
    if (!expect(Tok::LBrace, "SE0201", "to open `build`")) return false;

    while (!at(Tok::RBrace) && !at_end()) {
        const std::uint32_t before = tok_.loc.offset;
        ast::BuildStmt b;
        bool ok = true;

        if (!tok_.is_word()) {
            err("SE0201", tok_.loc,
                std::string("expected a build primitive, found ") + describe(tok_.kind),
                "expected `link`, `include_dir`, `define` or `cflag`");
            ok = false;
        } else {
            b.primitive = tok_.text;
            b.loc = tok_.loc;
            if (!contains(build_primitives(), b.primitive)) {
                std::vector<Attachment> att;
                const std::string did = closest(b.primitive, build_primitives());
                if (!did.empty()) att.push_back(help("did you mean `" + did + "`?"));
                att.push_back(note("the vocabulary is concrete build primitives: `link`, "
                                   "`include_dir`, `define`, `cflag` (\xc2\xa7""12.1)"));
                err("SE0251", tok_.loc, "unknown build primitive `" + b.primitive + "`", "",
                    std::move(att));
                ok = false;
            }
            advance();

            if (ok && !at(Tok::String)) {
                if (!swallow_invalid())
                    err("SE0201", tok_.loc,
                        std::string("expected a quoted argument, found ") + describe(tok_.kind),
                        "expected a string");
                ok = false;
            } else if (ok) {
                b.argument = tok_.text;
                advance();

                if (at(Tok::Ident) && tok_.text == "when") {
                    advance();
                    if (!tok_.is_word()) {
                        err("SE0250", tok_.loc,
                            std::string("expected a platform atom after `when`, found ") +
                                describe(tok_.kind),
                            "expected a platform name");
                        ok = false;
                    } else {
                        b.when = tok_.text;
                        b.when_loc = tok_.loc;
                        if (!contains(platform_atoms(), b.when)) {
                            std::vector<Attachment> att;
                            const std::string did = closest(b.when, platform_atoms());
                            if (!did.empty()) att.push_back(help("did you mean `" + did + "`?"));
                            att.push_back(note("known atoms are windows, linux, macos; the "
                                               "vocabulary is not yet pinned "
                                               "(\xc2\xa7""12.2, Appendix C)"));
                            warn("SE0250", tok_.loc, "unknown platform atom `" + b.when + "`",
                                 "", std::move(att));
                        }
                        advance();
                    }
                }
                if (ok && !expect(Tok::Semi, "SE0208", "after the build declaration")) ok = false;
            }
        }

        if (ok) out.push_back(std::move(b));
        else recover_in_list(Tok::RBrace);

        if (give_up()) return false;
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close `build`");
}

// ─── Annotations (§6.15) ─────────────────────────────────────────────────────

// `@name` or `@name(key, key = value, ...)`, any number in a row. The current
// token is `@`. Only the syntax is checked here; what the keys mean is
// checked by `apply_annotations`, once the declaration they lead is known.
bool Parser::parse_annotations(std::vector<ast::Annotation>& out) {
    while (at(Tok::At)) {
        ast::Annotation a;
        a.loc = tok_.loc;
        advance();  // @
        if (!at(Tok::Ident)) {
            err("SE0201", tok_.loc,
                std::string("expected an annotation name after `@`, found ") +
                    describe(tok_.kind),
                "expected a name",
                {note("an annotation reads `@diagram(collapsed)` (\xc2\xa7""6.15)")});
            return false;
        }
        a.name = tok_.text;
        // The caret spans `@name`, so an unknown name is underlined whole.
        a.loc.length = tok_.loc.offset + tok_.loc.length - a.loc.offset;
        advance();

        if (accept(Tok::LParen)) {
            while (!at(Tok::RParen)) {
                if (!at(Tok::Ident)) {
                    err("SE0201", tok_.loc,
                        std::string("expected a key, found ") + describe(tok_.kind),
                        "expected a key",
                        {note("an argument is a key, or `key = value` (\xc2\xa7""6.15)")});
                    return false;
                }
                ast::AnnotationArg arg;
                arg.key = tok_.text;
                arg.loc = tok_.loc;
                advance();
                if (accept(Tok::Equal)) {
                    const bool neg = accept(Tok::Minus);
                    if (at(Tok::String) && !neg) {
                        arg.kind = ast::AnnotationArg::Kind::String;
                    } else if (at(Tok::Number)) {
                        arg.kind = ast::AnnotationArg::Kind::Number;
                    } else if (at(Tok::Ident) && !neg) {
                        arg.kind = ast::AnnotationArg::Kind::Ident;
                    } else {
                        err("SE0201", tok_.loc,
                            std::string("expected a value, found ") + describe(tok_.kind),
                            "expected a string, a number or a name",
                            {note("an annotation's values are literals, never expressions "
                                  "(\xc2\xa7""6.15)")});
                        return false;
                    }
                    arg.value = (neg ? "-" : "") + tok_.text;
                    advance();
                }
                a.args.push_back(std::move(arg));
                if (!accept(Tok::Comma)) break;
            }
            if (!expect(Tok::RParen, "SE0201", "to close the annotation")) return false;
        }
        out.push_back(std::move(a));
    }
    return true;
}

namespace {

// §6.15 — the `@diagram` keys. A new rendering preference is a row here, and
// nothing else in the parser changes.
struct DiagramKey {
    const char* name;
    bool definition, group, instance;   // where it may appear; a `type` takes none
};
const DiagramKey diagram_keys[] = {
    {"collapsed", true, true, true},
    {"expanded", false, false, true},
};

const std::vector<const char*>& diagram_key_names() {
    static std::vector<const char*> v;
    if (v.empty())
        for (const DiagramKey& k : diagram_keys) v.push_back(k.name);
    return v;
}

}  // namespace

// Check `in` against the key table for `site`, and record what it means in
// `out`. Strict: every mistake is an error, because a mistyped key that drew
// nothing would look exactly like a key that worked.
void Parser::apply_annotations(const std::vector<ast::Annotation>& in, AnnotSite site,
                               ast::DiagramHints& out) {
    const char* site_name = site == AnnotSite::Definition ? "a node definition"
                            : site == AnnotSite::Type     ? "a `type`"
                            : site == AnnotSite::Enum     ? "a `finite` type"
                            : site == AnnotSite::Group    ? "a group"
                                                          : "an instance";
    const ast::Annotation* first = nullptr;
    for (const ast::Annotation& a : in) {
        if (a.name != "diagram") {
            std::vector<Attachment> att;
            const std::string did = closest(a.name, {"diagram"}, 2);
            if (!did.empty()) att.push_back(help("did you mean `@" + did + "`?"));
            att.push_back(note("the only annotation is `@diagram` (\xc2\xa7""6.15)"));
            err("SE0280", a.loc, "unknown annotation `@" + a.name + "`", "", std::move(att));
            continue;
        }
        if (first) {
            err("SE0286", a.loc, "a second `@diagram` on one declaration", "",
                {note("first here", src_.path() + ":" + std::to_string(first->loc.line)),
                 help("put every key in one `@diagram(...)`, so a declaration's hints "
                      "are written in one place (\xc2\xa7""6.15)")});
            continue;
        }
        first = &a;
        if (a.args.empty())
            err("SE0283", a.loc, "`@diagram` names no keys", "",
                {note("the keys are `collapsed` and `expanded` (\xc2\xa7""6.15)")});

        std::vector<const ast::AnnotationArg*> seen;
        for (const ast::AnnotationArg& arg : a.args) {
            const DiagramKey* key = nullptr;
            for (const DiagramKey& k : diagram_keys)
                if (arg.key == k.name) key = &k;
            if (!key) {
                std::vector<Attachment> att;
                const std::string did = closest(arg.key, diagram_key_names(), 2);
                if (!did.empty()) att.push_back(help("did you mean `" + did + "`?"));
                att.push_back(note("the `@diagram` keys are `collapsed` and `expanded` "
                                   "(\xc2\xa7""6.15)"));
                err("SE0281", arg.loc, "unknown `@diagram` key `" + arg.key + "`", "",
                    std::move(att));
                continue;
            }
            const ast::AnnotationArg* dup = nullptr;
            for (const ast::AnnotationArg* p : seen)
                if (p->key == arg.key) dup = p;
            if (dup) {
                err("SE0284", arg.loc, "duplicate `@diagram` key `" + arg.key + "`",
                    "already given",
                    {note("first here", src_.path() + ":" + std::to_string(dup->loc.line))});
                continue;
            }
            seen.push_back(&arg);

            const bool allowed = site == AnnotSite::Definition ? key->definition
                                 : site == AnnotSite::Group    ? key->group
                                 : site == AnnotSite::Instance ? key->instance
                                                               : false;
            if (!allowed) {
                std::vector<Attachment> att;
                if (site == AnnotSite::Type)
                    att.push_back(note("a record type is never drawn on the topology diagram"));
                else if (site == AnnotSite::Enum)
                    att.push_back(note("a finite type is never drawn on the topology diagram"));
                else if (arg.key == "expanded")
                    att.push_back(note("everything is drawn expanded unless it says "
                                       "`collapsed`; `expanded` is for an instance, to "
                                       "override its definition's `collapsed`"));
                std::vector<std::string> where;
                if (key->definition) where.push_back("a node definition");
                if (key->group) where.push_back("a group");
                if (key->instance) where.push_back("an instance");
                std::string list;
                for (std::size_t k = 0; k < where.size(); ++k)
                    list += (k == 0 ? "" : k + 1 == where.size() ? " or " : ", ") + where[k];
                att.push_back(help("`" + arg.key + "` belongs on " + list));
                err("SE0282", arg.loc, "`" + arg.key + "` is not allowed on " + site_name, "",
                    std::move(att));
                continue;
            }
            if (arg.kind != ast::AnnotationArg::Kind::Flag) {
                err("SE0283", arg.loc, "`" + arg.key + "` is a flag and takes no value",
                    "remove the `= " + arg.value + "`");
                continue;
            }

            const auto want = arg.key == "collapsed" ? ast::DiagramHints::Collapse::Collapsed
                                                     : ast::DiagramHints::Collapse::Expanded;
            if (out.collapse != ast::DiagramHints::Collapse::Unset && out.collapse != want) {
                err("SE0285", arg.loc, "`collapsed` and `expanded` together", "",
                    {note("an instance starts either collapsed or expanded (\xc2\xa7""6.15)")});
                continue;
            }
            out.collapse = want;
            out.collapse_loc = arg.loc;
        }
    }
}

// Annotations followed by nothing they can lead. Reported once, at the first.
void Parser::stray_annotations(const std::vector<ast::Annotation>& in, const char* caret) {
    if (in.empty()) return;
    err("SE0287", in.front().loc, "this annotation annotates nothing", caret,
        {note("an annotation leads the declaration below it: a `node` definition, a "
              "`group`, or an instance in `structure` (\xc2\xa7""6.15)")});
}

// ─── Structure (§6.9) ────────────────────────────────────────────────────────

bool Parser::parse_structure(ast::Structure& out) {
    if (!expect(Tok::LBrace, "SE0201", "to open `structure`")) return false;
    if (!parse_structure_body(out, static_cast<std::size_t>(-1))) return false;
    return expect(Tok::RBrace, "SE0201", "to close `structure`");
}

// The statements of a `structure` block, or of a `group` inside one, up to but
// not including the closing `}`. `group` is the index of the enclosing group
// in `out.groups`, or npos at the top.
bool Parser::parse_structure_body(ast::Structure& out, std::size_t group) {
    while (!at(Tok::RBrace) && !at_end()) {
        const std::uint32_t before = tok_.loc.offset;

        // §6.15 — an instance or a group may be annotated; a wire may not, yet.
        std::vector<ast::Annotation> annots;
        if (at(Tok::At)) {
            if (!parse_annotations(annots)) {
                recover_in_list(Tok::RBrace);
                if (give_up()) return false;
                continue;
            }
            const bool is_group =
                at(Tok::Ident) && tok_.text == "group" && peek().kind == Tok::String;
            if (!at(Tok::KwNode) && !is_group) {
                stray_annotations(annots, "expected an instance or a group to follow");
                continue;
            }
        }

        if (at(Tok::KwNode)) {
            ast::Instance inst;
            inst.group = group;
            apply_annotations(annots, AnnotSite::Instance, inst.diagram);
            inst.annotations = std::move(annots);
            if (parse_instance(inst)) out.instances.push_back(std::move(inst));
            else recover_in_list(Tok::RBrace);
        } else if (at(Tok::Ident) && tok_.text == "group" && peek().kind == Tok::String) {
            // §6.9.3 — contextual: an identifier followed by a string literal is
            // never a wire, so `group` stays usable as an instance name.
            if (!parse_group(out, group, std::move(annots))) return false;
        } else if (at(Tok::KwSelf) || at(Tok::Ident) || at(Tok::Number) || at(Tok::Plus) ||
                   at(Tok::Minus)) {
            // A number, `+` or `-` starts a literal source (§6.9.2). Nothing
            // else in a `structure` block can, so no lookahead is needed.
            // One statement may be a chain, and so may yield several wires.
            std::vector<ast::Wire> ws;
            if (parse_wire_stmt(ws))
                for (ast::Wire& w : ws) out.wires.push_back(std::move(w));
            else recover_in_list(Tok::RBrace);
        } else {
            err("SE0211", tok_.loc,
                std::string("expected a child instance or a wire, found ") + describe(tok_.kind),
                "expected `node` or an endpoint",
                {note("`structure` holds `node name : Def { ... };` instances, "
                      "`a.b --> c.d;` wires (\xc2\xa7""6.9), and `group \"label\" { ... }` "
                      "blocks of either (\xc2\xa7""6.9.3); an instance or a group may "
                      "be preceded by `@diagram(...)` (\xc2\xa7""6.15)")});
            recover_in_list(Tok::RBrace);
        }

        if (give_up()) return false;
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return true;
}

// §6.9.3 — `group "label" { … }`. The current token is `group`, and the next
// is known to be a string literal.
bool Parser::parse_group(ast::Structure& out, std::size_t parent,
                         std::vector<ast::Annotation> annotations) {
    ast::Group g;
    g.loc = tok_.loc;
    g.parent = parent;
    g.start_line = annotations.empty() ? g.loc.line : annotations.front().loc.line;
    apply_annotations(annotations, AnnotSite::Group, g.diagram);
    g.annotations = std::move(annotations);
    advance();  // group
    const Loc label_loc = tok_.loc;
    g.label = tok_.text;
    advance();  // the label

    if (g.label.find_first_not_of(" \t") == std::string::npos) {
        err("SE0217", label_loc, "a group's label is empty", "empty label",
            {help("the label is the box's title on the topology diagram")});
    } else {
        for (const ast::Group& other : out.groups) {
            if (other.parent != parent || other.label != g.label) continue;
            err("SE0216", label_loc, "duplicate group label \"" + g.label + "\"",
                "already used by a sibling group",
                {note("first used here", src_.path() + ":" + std::to_string(other.loc.line)),
                 help("two boxes with one title, or one box merged from two places, "
                      "would each surprise the reader; give each group its own label")});
            break;
        }
    }

    const std::size_t index = out.groups.size();
    out.groups.push_back(std::move(g));
    const std::size_t instances_before = out.instances.size();

    if (!expect(Tok::LBrace, "SE0201", "to open the group")) return false;
    if (!parse_structure_body(out, index)) return false;
    if (!expect(Tok::RBrace, "SE0201", "to close the group")) return false;

    // Nested groups append their instances too, so this counts the subtree.
    if (out.instances.size() == instances_before)
        warn("SE0218", out.groups[index].loc,
             "group \"" + out.groups[index].label + "\" holds no instances",
             "draws nothing",
             {note("a group draws a box around the instances declared inside it; wires "
                   "placed in a group do not move, so this group does not appear on the "
                   "topology diagram (\xc2\xa7""6.9.3)")});
    return true;
}

bool Parser::parse_instance(ast::Instance& out) {
    out.loc = tok_.loc;
    advance();  // node

    if (!at(Tok::Ident)) {
        err("SE0201", tok_.loc,
            std::string("expected an instance name, found ") + describe(tok_.kind),
            "expected an instance name");
        return false;
    }
    out.name = tok_.text;
    out.loc = tok_.loc;
    advance();

    if (!expect(Tok::Colon, "SE0201", "after the instance name")) return false;
    if (!parse_qualified_name(out.definition)) return false;

    // §6.2a — `Biquad (m/s^2)`. Only `{`, `;` or `(` may follow the definition
    // path, and a `(` can be nothing but a unit argument list, so this costs no
    // lookahead.
    if (at(Tok::LParen)) {
        out.unit_args_loc = tok_.loc;
        advance();
        for (;;) {
            ast::UnitPtr u = parse_unit_expr();
            if (!u) return false;
            out.unit_args.push_back(std::move(u));
            if (at(Tok::Comma)) {
                advance();
                continue;
            }
            break;
        }
        if (!expect(Tok::RParen, "SE0201", "to close the unit arguments")) return false;
    }

    if (at(Tok::LBrace)) {
        if (!parse_bindings(out.bindings)) return false;
    }

    if (!at(Tok::Semi)) {
        err("SE0208", tok_.loc, "expected `;` after the instance declaration", "expected `;`",
            {note("the `;` after the closing brace is required: it terminates the "
                  "statement, as a port declaration's does (\xc2\xa7""6.9.1)")});
        return false;
    }
    advance();
    return true;
}

bool Parser::parse_endpoint(ast::Endpoint& out) {
    out.loc = tok_.loc;

    // §6.9.2 — a literal, which is a wire SOURCE and nothing else. That it may
    // not be a destination is settled here rather than at stage 3 (as the
    // `param` rule is, SE0315): a literal is recognisable without resolving
    // anything, so `parse_wire_stmt` can see the violation on its own.
    if (at(Tok::Number) || at(Tok::Plus) || at(Tok::Minus)) {
        bool negate = at(Tok::Minus);
        if (!at(Tok::Number)) {
            out.text = tok_.text;
            advance();
            if (!at(Tok::Number)) {
                err("SE0212", tok_.loc,
                    std::string("expected a number after `") + out.text + "`, found " +
                        describe(tok_.kind),
                    "expected a number");
                return false;
            }
        }
        out.is_literal = true;
        out.text += tok_.text;
        out.value = negate ? -std::strtod(tok_.text.c_str(), nullptr)
                           : std::strtod(tok_.text.c_str(), nullptr);
        advance();
        // The same rule `parse_primary` states: there is no implicit
        // multiplication, so a `(` after a literal is always a unit.
        if (at(Tok::LParen)) {
            ast::UnitPtr u = parse_unit();
            if (!u) return false;
            out.text += " (" + unit_to_string(u.get()) + ")";
            out.unit = std::shared_ptr<ast::UnitExpr>(u.release());
        }
        return true;
    }

    if (at(Tok::KwSelf)) {
        out.is_self = true;
        advance();
    } else if (at(Tok::Ident)) {
        out.segs.push_back(tok_.text);
        advance();
    } else {
        err("SE0212", tok_.loc,
            std::string("expected a wire endpoint, found ") + describe(tok_.kind),
            "expected `self`, an instance name, or a literal",
            {note("an endpoint is `self` or an instance name, optionally followed by "
                  "`.<port>`, or a literal source (\xc2\xa7""6.9.2)")});
        return false;
    }

    while (at(Tok::Dot)) {
        advance();
        if (!at(Tok::Ident)) {
            err("SE0212", tok_.loc,
                std::string("expected a port name after `.`, found ") + describe(tok_.kind),
                "expected a port name");
            return false;
        }
        out.segs.push_back(tok_.text);
        out.loc.length = tok_.loc.offset + tok_.loc.length - out.loc.offset;
        advance();
    }

    // §6.9.2 — an endpoint may name no port at all. A bare endpoint takes the
    // sole port on the side the arrow needs, which is a question about arity
    // and so cannot be answered here: the parser has not resolved the
    // instance, or even loaded the file its definition is in. Stage 3 decides
    // it (`SE0314`), the way it already decides that a port name resolves.
    return true;
}

// §6.9.2 — one statement, one or more wires. A chain `a --> b --> c` is sugar
// for `a --> b; b --> c;`, desugared here so that `ast::Wire` never learns what
// a chain is and no later stage has to.
//
// The comma list can only TERMINATE the statement: once a comma is seen only
// more comma-separated endpoints may follow, so `a --> b, c --> d` is a plain
// syntax error and the fan-out attaches to the last hop by construction rather
// than by a precedence rule anyone has to remember.
bool Parser::parse_wire_stmt(std::vector<ast::Wire>& out) {
    const Loc start = tok_.loc;

    // Every arrow-separated endpoint, in order. At least two after the loop.
    std::vector<ast::Endpoint> chain;
    {
        ast::Endpoint e;
        if (!parse_endpoint(e)) return false;
        chain.push_back(std::move(e));
    }

    if (!at(Tok::Arrow)) {
        if (swallow_invalid()) return false;
        std::vector<Attachment> att;
        if (at(Tok::Equal) || at(Tok::Minus))
            att.push_back(help("the connection operator is `-->`"));
        err("SE0213", tok_.loc, std::string("expected `-->`, found ") + describe(tok_.kind),
            "expected `-->`", std::move(att));
        return false;
    }

    while (accept(Tok::Arrow)) {
        ast::Endpoint e;
        if (!parse_endpoint(e)) return false;
        chain.push_back(std::move(e));
    }

    // The fan-out on the final hop.
    std::vector<ast::Endpoint> extra;
    while (accept(Tok::Comma)) {
        ast::Endpoint e;
        if (!parse_endpoint(e)) return false;
        extra.push_back(std::move(e));
    }

    if (!expect(Tok::Semi, "SE0208", "after the wire")) return false;

    // An interior endpoint is a destination and a source at once, so a port
    // name there would have to mean two different ports. Reported per offender
    // rather than at the first.
    //
    // Reported, but not FAILED: the `;` is already consumed, so the statement
    // is syntactically whole and the next one parses normally. Returning false
    // here would send the caller into `recover_in_list`, which skips to the
    // closing brace and would silently stop checking the rest of the block.
    // The wires are dropped instead, so nothing malformed enters the AST.
    bool ok = true;

    // §6.9.2 — a literal is a source and nothing else. Checked BEFORE the
    // interior rule below, which would otherwise claim an interior literal
    // "names a port": a literal has no segments, so it fails that test for a
    // reason that has nothing to do with why it is wrong here.
    auto no_literal = [&](const ast::Endpoint& e) {
        if (!e.is_literal) return;
        err("SE0215", e.loc, "a wire may not drive a literal",
            "`" + e.str() + "` is a destination",
            {note("a literal is a wire SOURCE only: it is a constant, and nothing "
                  "can be written to it (\xc2\xa7""6.9.2)"),
             help("to hold an input at a constant, put the literal on the left of "
                  "the arrow")});
        ok = false;
    };
    for (std::size_t i = 1; i < chain.size(); ++i) no_literal(chain[i]);
    for (const ast::Endpoint& e : extra) no_literal(e);
    if (!ok) return true;

    for (std::size_t i = 1; i + 1 < chain.size(); ++i) {
        const ast::Endpoint& e = chain[i];
        if (!e.is_self && e.segs.size() == 1) continue;
        err("SE0214", e.loc, "an interior endpoint of a wire chain must be a bare instance name",
            e.is_self ? "`self` here" : "names a port",
            {note("`a --> b --> c` means `a --> b; b --> c;`, so `b` is a destination and a "
                  "source at once and a port name here would have to name two different "
                  "ports (\xc2\xa7""6.9.2)"),
             help("split the chain into separate wires")});
        ok = false;
    }
    if (!ok) return true;

    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        ast::Wire w;
        w.loc = start;
        w.source = chain[i];
        w.dests.push_back(chain[i + 1]);   // a copy: an interior is used twice
        if (i + 2 == chain.size())
            for (ast::Endpoint& e : extra) w.dests.push_back(std::move(e));
        out.push_back(std::move(w));
    }
    return true;
}

// ─── Methods and helpers (§8.1, §11.4) ───────────────────────────────────────

bool Parser::parse_method(ast::NodeDef& node) {
    ast::Method m;
    m.loc = tok_.loc;
    switch (tok_.kind) {
        case Tok::KwInit:       m.which = ast::Method::Which::Init;       break;
        case Tok::KwOutput:     m.which = ast::Method::Which::Output;     break;
        case Tok::KwDerivative: m.which = ast::Method::Which::Derivative; break;
        case Tok::KwNext:       m.which = ast::Method::Which::Next;       break;
        case Tok::KwTransition: m.which = ast::Method::Which::Transition; break;
        case Tok::KwAdjust:     m.which = ast::Method::Which::Adjust;     break;
        case Tok::KwOnStep:     m.which = ast::Method::Which::OnStep;     break;
        default:                m.which = ast::Method::Which::Final;      break;
    }
    const std::string name = tok_.text;
    advance();

    if (!at(Tok::LParen)) {
        // The ruling of 2026-07-27: parens are required on every lifecycle
        // method, so `init { … }` is an error with a precise fix.
        err("SE0201", tok_.loc,
            "expected `(` after `" + name + "`, found " + std::string(describe(tok_.kind)),
            "expected `(`",
            {note("every lifecycle method carries a parameter list; on `output()` that "
                  "list IS the feedthrough declaration (\xc2\xa7""8.3)"),
             help("write `" + name + "() { ... }`")});
        return false;
    }
    advance();

    if (!at(Tok::RParen)) {
        for (;;) {
            if (!at(Tok::Ident)) {
                std::vector<Attachment> att;
                att.push_back(note("the parameter list holds bare input names — it can only "
                                   "contain inputs, so `in.` is not written here "
                                   "(\xc2\xa7""8.3)"));
                err("SE0201", tok_.loc,
                    std::string("expected an input name, found ") + describe(tok_.kind),
                    "expected an input name", std::move(att));
                return false;
            }
            m.params.push_back(tok_.text);
            m.param_locs.push_back(tok_.loc);
            advance();
            if (!accept(Tok::Comma)) break;
        }
    }
    if (!expect(Tok::RParen, "SE0201", "to close the parameter list")) return false;

    if (m.which == ast::Method::Which::Init && !m.params.empty()) {
        err("SE0241", m.param_locs.front(), "`init` takes no parameters", "",
            {note("`init()` runs before any signal has propagated, so it can read no "
                  "inputs (\xc2\xa7""8.1)")});
        return false;
    }
    if (m.which == ast::Method::Which::Final && m.params.size() > 1) {
        err("SE0242", m.param_locs[1], "`final` takes at most one parameter", "",
            {note("the single parameter is the read-only run context: reason, message, "
                  "time (\xc2\xa7""10.3)"),
             help("write `final(ctx) { ... }` or `final() { ... }`")});
        return false;
    }

    if (!at(Tok::LBrace)) {
        err("SE0201", tok_.loc,
            std::string("expected `{` to open the method body, found ") + describe(tok_.kind),
            "expected `{`");
        return false;
    }

    enter_raw();
    const bool ok = lexer_.scan_brace_block(m.body);
    resync();
    node.methods.push_back(std::move(m));
    return ok;
}

bool Parser::parse_helper(ast::NodeDef& node) {
    ast::Helper h;
    h.loc = tok_.loc;

    enter_raw();
    if (!lexer_.scan_signature(h.signature)) {
        resync();
        return false;
    }
    if (!lexer_.scan_brace_block(h.body)) {
        resync();
        return false;
    }
    resync();
    node.helpers.push_back(std::move(h));
    return true;
}

// ─── The sim file (§13) ──────────────────────────────────────────────────────

std::unique_ptr<ast::SimFile> Parser::parse_sim_file() {
    auto file = std::make_unique<ast::SimFile>();
    advance();

    if (!at(Tok::KwSim)) {
        err("SE0201", tok_.loc, std::string("expected `sim`, found ") + describe(tok_.kind),
            "expected `sim`",
            {note("a `.sim` file contains exactly one `sim` declaration (\xc2\xa7""13)")});
        return file;
    }
    file->loc = tok_.loc;
    advance();

    if (!at(Tok::Ident)) {
        err("SE0201", tok_.loc,
            std::string("expected a sim name, found ") + describe(tok_.kind),
            "expected a name");
    } else {
        file->name = tok_.text;
        file->loc = tok_.loc;
        advance();
    }
    if (!expect(Tok::LBrace, "SE0201", "to open the sim body")) return file;

    while (!at(Tok::RBrace) && !at_end() && !give_up()) {
        const std::uint32_t before = tok_.loc.offset;
        if (!parse_sim_entry(*file)) recover_in_list(Tok::RBrace);
        if (tok_.loc.offset == before && !at(Tok::RBrace) && !at_end()) advance();
    }
    expect(Tok::RBrace, "SE0201", "to close the sim body");

    if (!at_end()) {
        err("SE0201", tok_.loc, "unexpected content after the `sim` declaration", "",
            {note("a `.sim` file contains exactly one `sim` declaration (\xc2\xa7""13)")});
    }
    return file;
}

bool Parser::parse_sim_entry(ast::SimFile& out) {
    if (!tok_.is_word()) {
        err("SE0201", tok_.loc, std::string("expected an entry key, found ") + describe(tok_.kind),
            "expected a key");
        return false;
    }

    ast::SimEntry e;
    e.key = tok_.text;
    e.key_loc = tok_.loc;
    e.loc = tok_.loc;
    advance();

    // Duplicate check (§13.1, SE0260).
    for (const ast::SimEntry& prev : out.entries) {
        if (prev.key == e.key) {
            err("SE0260", e.key_loc, "duplicate `" + e.key + "` entry", "already set",
                {note("`" + e.key + "` first appears on line " +
                      std::to_string(prev.key_loc.line)),
                 note("each `sim` entry may appear at most once (\xc2\xa7""13.1)")});
            break;
        }
    }

    // Unknown keys still parse (§17), so the diagnostic can be a good one.
    if (!contains(sim_keys(), e.key)) {
        std::vector<Attachment> att;
        const std::string did = closest(e.key, sim_keys());
        if (!did.empty()) att.push_back(help("did you mean `" + did + "`?"));
        att.push_back(note("the entries are root, step, solver, duration, mode, sync, "
                           "window, settings, record, log (\xc2\xa7""13.1)"));
        err("SE0261", e.key_loc, "unknown `sim` entry `" + e.key + "`", "", std::move(att));
    }

    if (!expect(Tok::Colon, "SE0201", "after the entry key")) return false;

    if (e.key == "root") {
        e.kind = ast::SimEntry::Kind::Root;
        if (!parse_qualified_name(e.root)) return false;
        if (at(Tok::LBrace) && !parse_bindings(e.bindings)) return false;
        if (!expect(Tok::Semi, "SE0208", "after the root declaration")) return false;
        out.entries.push_back(std::move(e));
        return true;
    }

    if (e.key == "record" || e.key == "log") {
        const bool is_record = (e.key == "record");
        e.kind = is_record ? ast::SimEntry::Kind::Record : ast::SimEntry::Kind::Log;
        bool ok;
        if (is_record) {
            e.record.loc = tok_.loc;
            ok = parse_record_block(e.record);
        } else {
            e.log.loc = tok_.loc;
            ok = parse_log_block(e.log);
        }
        if (!ok) return false;
        if (at(Tok::Semi)) {
            warn("SE0201", tok_.loc, "no `;` is needed after a block entry", "remove this `;`",
                 {note("block-valued entries (`record:`, `log:`) take no trailing "
                       "semicolon (\xc2\xa7""13.1)")});
            advance();
        }
        out.entries.push_back(std::move(e));
        return true;
    }

    if (at(Tok::String)) {
        e.kind = ast::SimEntry::Kind::Text;
        e.text = tok_.text;
        advance();
    } else if (tok_.is_word() && peek().kind == Tok::Semi) {
        // A bare word value: `solver: rk4;`, `mode: realtime;`.
        e.kind = ast::SimEntry::Kind::Word;
        e.word = tok_.text;
        advance();
    } else {
        e.kind = ast::SimEntry::Kind::Value;
        e.value = parse_expr();
        if (!e.value) return false;
    }
    if (!expect(Tok::Semi, "SE0208", "after the entry")) return false;
    out.entries.push_back(std::move(e));
    return true;
}

bool Parser::parse_record_block(ast::RecordBlock& out) {
    if (!expect(Tok::LBrace, "SE0201", "to open the `record` block")) return false;

    static const std::vector<const char*> keys = {"file", "every", "signals"};

    while (!at(Tok::RBrace) && !at_end()) {
        const std::uint32_t before = tok_.loc.offset;

        if (!tok_.is_word()) {
            err("SE0201", tok_.loc,
                std::string("expected `file`, `every` or `signals`, found ") +
                    describe(tok_.kind),
                "expected a key");
            recover_in_list(Tok::RBrace);
        } else {
            const std::string key = tok_.text;
            const Loc key_loc = tok_.loc;
            advance();

            if (key == "signals") {
                if (!expect(Tok::LBrace, "SE0201", "to open `signals`")) {
                    recover_in_list(Tok::RBrace);
                } else {
                    while (!at(Tok::RBrace) && !at_end()) {
                        const std::uint32_t inner = tok_.loc.offset;
                        ast::Path p;
                        if (!parse_path(p, true, true)) {
                            recover_in_list(Tok::RBrace);
                        } else if (!expect(Tok::Semi, "SE0208", "after the signal path")) {
                            recover_in_list(Tok::RBrace);
                        } else {
                            out.signals.push_back(std::move(p));
                        }
                        if (give_up()) return false;
                        if (tok_.loc.offset == inner && !at(Tok::RBrace)) advance();
                    }
                    expect(Tok::RBrace, "SE0201", "to close `signals`");
                }
            } else if (key == "file" || key == "every") {
                if (!expect(Tok::Colon, "SE0201", "after the key")) {
                    recover_in_list(Tok::RBrace);
                } else if (key == "file") {
                    if (!at(Tok::String)) {
                        err("SE0201", tok_.loc,
                            std::string("expected a file name, found ") + describe(tok_.kind),
                            "expected a string");
                        recover_in_list(Tok::RBrace);
                    } else {
                        out.file = tok_.text;
                        out.has_file = true;
                        advance();
                        expect(Tok::Semi, "SE0208", "after the file name");
                    }
                } else {
                    if (!at(Tok::Number) || tok_.text.find('.') != std::string::npos) {
                        err("SE0201", tok_.loc, "`every` takes a whole number of base steps",
                            "expected an integer",
                            {note("`every: N` decimates the recording (\xc2\xa7""13.5)")});
                        recover_in_list(Tok::RBrace);
                    } else {
                        out.every = std::strtol(tok_.text.c_str(), nullptr, 10);
                        out.has_every = true;
                        advance();
                        expect(Tok::Semi, "SE0208", "after `every`");
                    }
                }
            } else {
                std::vector<Attachment> att;
                const std::string did = closest(key, keys);
                if (!did.empty()) att.push_back(help("did you mean `" + did + "`?"));
                att.push_back(note("the `record` block takes `file`, `every` and `signals` "
                                   "(\xc2\xa7""13.5)"));
                err("SE0261", key_loc, "unknown `record` key `" + key + "`", "", std::move(att));
                recover_in_list(Tok::RBrace);
            }
        }

        if (give_up()) return false;
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close the `record` block");
}

bool Parser::parse_log_block(ast::LogBlock& out) {
    if (!expect(Tok::LBrace, "SE0201", "to open the `log` block")) return false;

    static const std::vector<const char*> keys = {"file", "level", "levels"};

    while (!at(Tok::RBrace) && !at_end()) {
        const std::uint32_t before = tok_.loc.offset;

        if (!tok_.is_word()) {
            err("SE0201", tok_.loc,
                std::string("expected `file`, `level` or `levels`, found ") + describe(tok_.kind),
                "expected a key");
            recover_in_list(Tok::RBrace);
        } else {
            const std::string key = tok_.text;
            const Loc key_loc = tok_.loc;
            advance();

            if (key == "levels") {
                if (!expect(Tok::LBrace, "SE0201", "to open `levels`")) {
                    recover_in_list(Tok::RBrace);
                } else {
                    while (!at(Tok::RBrace) && !at_end()) {
                        const std::uint32_t inner = tok_.loc.offset;
                        ast::Path p;
                        if (!parse_path(p, true)) {
                            recover_in_list(Tok::RBrace);
                        } else if (!expect(Tok::Colon, "SE0201", "after the node path")) {
                            recover_in_list(Tok::RBrace);
                        } else if (!tok_.is_word()) {
                            err("SE0201", tok_.loc,
                                std::string("expected a log level, found ") +
                                    describe(tok_.kind),
                                "expected trace, debug, info, warn or error");
                            recover_in_list(Tok::RBrace);
                        } else {
                            const std::string level = tok_.text;
                            advance();
                            if (expect(Tok::Semi, "SE0208", "after the level override"))
                                out.levels.emplace_back(std::move(p), level);
                            else
                                recover_in_list(Tok::RBrace);
                        }
                        if (give_up()) return false;
                        if (tok_.loc.offset == inner && !at(Tok::RBrace)) advance();
                    }
                    expect(Tok::RBrace, "SE0201", "to close `levels`");
                }
            } else if (key == "file" || key == "level") {
                if (!expect(Tok::Colon, "SE0201", "after the key")) {
                    recover_in_list(Tok::RBrace);
                } else if (key == "file") {
                    if (!at(Tok::String)) {
                        err("SE0201", tok_.loc,
                            std::string("expected a file name, found ") + describe(tok_.kind),
                            "expected a string");
                        recover_in_list(Tok::RBrace);
                    } else {
                        out.file = tok_.text;
                        out.has_file = true;
                        advance();
                        expect(Tok::Semi, "SE0208", "after the file name");
                    }
                } else {
                    if (!tok_.is_word()) {
                        err("SE0201", tok_.loc,
                            std::string("expected a log level, found ") + describe(tok_.kind),
                            "expected trace, debug, info, warn or error");
                        recover_in_list(Tok::RBrace);
                    } else {
                        out.level = tok_.text;
                        out.has_level = true;
                        advance();
                        expect(Tok::Semi, "SE0208", "after the level");
                    }
                }
            } else {
                std::vector<Attachment> att;
                const std::string did = closest(key, keys);
                if (!did.empty()) att.push_back(help("did you mean `" + did + "`?"));
                att.push_back(note("the `log` block takes `file`, `level` and `levels` "
                                   "(\xc2\xa7""13.7)"));
                err("SE0261", key_loc, "unknown `log` key `" + key + "`", "", std::move(att));
                recover_in_list(Tok::RBrace);
            }
        }

        if (give_up()) return false;
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close the `log` block");
}

// ─── The settings source (§14) ───────────────────────────────────────────────

std::unique_ptr<ast::SettingsFile> Parser::parse_settings_file() {
    auto file = std::make_unique<ast::SettingsFile>();
    advance();

    while (!at_end() && !give_up()) {
        const std::uint32_t before = tok_.loc.offset;
        ast::Override o;
        o.loc = tok_.loc;

        if (!parse_path(o.path, true, true)) {
            recover_in_list(Tok::End);
        } else if (!expect(Tok::Equal, "SE0201", "after the path")) {
            recover_in_list(Tok::End);
        } else if ((o.value = parse_expr()) == nullptr) {
            recover_in_list(Tok::End);
        } else if (!expect(Tok::Semi, "SE0208", "after the override")) {
            recover_in_list(Tok::End);
        } else {
            file->overrides.push_back(std::move(o));
        }

        if (tok_.loc.offset == before && !at_end()) advance();
    }
    return file;
}

}  // namespace se
