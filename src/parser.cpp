#include "parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

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
        "native",   "build",  "structure", "declarations", "units"};
    return v;
}

const std::vector<const char*>& lifecycle_names() {
    static const std::vector<const char*> v = {"init",    "output", "derivative",
                                              "next",    "on_step", "final"};
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
        if (depth == 0 && (tok_.kind == Tok::KwNode || tok_.kind == Tok::KwType)) return;
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

        if (at(Tok::KwType)) {
            ast::TypeDef t;
            if (parse_type_def(t)) file->types.push_back(std::move(t));
            else recover_to_definition();
        } else if (at(Tok::KwNode)) {
            ast::NodeDef n;
            if (parse_node_def(n)) file->nodes.push_back(std::move(n));
            else recover_to_definition();
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
            att.push_back(note("a `.se` file contains `node` and `type` declarations "
                               "(\xc2\xa7""2.3)"));
            if (at(Tok::KwSim))
                att.push_back(help("a `sim` declaration belongs in its own `.sim` file "
                                   "(\xc2\xa7""13)"));
            err("SE0204", tok_.loc,
                std::string("expected a definition (`node` or `type`), found ") +
                    describe(tok_.kind),
                "expected `node` or `type`", std::move(att));
            recover_to_definition();
        }

        if (tok_.loc.offset == before && !at_end()) advance();
    }
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
        case Tok::KwOnStep:
        case Tok::KwFinal:
            return parse_method(node);

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
            const Tok after = peek().kind;
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
                               "on_step, final (\xc2\xa7""8.1)")});
            }
            return parse_helper(node);
        }

        default:
            return parse_helper(node);
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
            } else if (!s.type.is_scalar) {
                err("SE0231", s.type.loc, "a setting may not be a record type", "",
                    {note("settings are scalars: the flow-down expression language is "
                          "scalar (\xc2\xa7""5.2)")});
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

        if (at(Tok::KwContinuous)) {
            s.is_continuous = true;
            s.kind_loc = tok_.loc;
            have_kind = true;
            advance();
        } else if (at(Tok::KwDiscrete)) {
            s.is_continuous = false;
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
                    "state `" + tok_.text + "` must be declared `continuous` or `discrete`",
                    "expected `continuous` or `discrete` before the state name",
                    {note("there is deliberately no default state kind — the two differ in "
                          "write accessor (`der.` vs `next.`), in schedule, and in whether "
                          "they enter the solver's state vector (\xc2\xa7""6.4)"),
                     help("prefix the declaration with `continuous` or `discrete`")});
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
                if (!s.type.is_scalar) {
                    err("SE0232", s.type.loc, "a state may not be a record type", "",
                        {note("the solver ABI is a flat `double` vector (\xc2\xa7""5.2)")});
                    ok = false;
                }
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

// ─── Structure (§6.9) ────────────────────────────────────────────────────────

bool Parser::parse_structure(ast::Structure& out) {
    if (!expect(Tok::LBrace, "SE0201", "to open `structure`")) return false;

    while (!at(Tok::RBrace) && !at_end()) {
        const std::uint32_t before = tok_.loc.offset;

        if (at(Tok::KwNode)) {
            ast::Instance inst;
            if (parse_instance(inst)) out.instances.push_back(std::move(inst));
            else recover_in_list(Tok::RBrace);
        } else if (at(Tok::KwSelf) || at(Tok::Ident)) {
            ast::Wire w;
            if (parse_wire(w)) out.wires.push_back(std::move(w));
            else recover_in_list(Tok::RBrace);
        } else {
            err("SE0211", tok_.loc,
                std::string("expected a child instance or a wire, found ") + describe(tok_.kind),
                "expected `node` or an endpoint",
                {note("`structure` holds `node name : Def { ... };` instances and "
                      "`a.b --> c.d;` wires (\xc2\xa7""6.9)")});
            recover_in_list(Tok::RBrace);
        }

        if (give_up()) return false;
        if (tok_.loc.offset == before && !at(Tok::RBrace)) advance();
    }
    return expect(Tok::RBrace, "SE0201", "to close `structure`");
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

    if (at(Tok::KwSelf)) {
        out.is_self = true;
        advance();
    } else if (at(Tok::Ident)) {
        out.segs.push_back(tok_.text);
        advance();
    } else {
        err("SE0212", tok_.loc,
            std::string("expected a wire endpoint, found ") + describe(tok_.kind),
            "expected `self` or an instance name",
            {note("an endpoint is `self.<port>` or `<instance>.<port>` (\xc2\xa7""6.9.2)")});
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

    const std::size_t needed = out.is_self ? 1u : 2u;
    if (out.segs.size() < needed) {
        err("SE0212", out.loc, "wire endpoint `" + out.str() + "` does not name a port",
            "expected `.<port>` here",
            {note("an endpoint is `self.<port>` or `<instance>.<port>`; a composite wires "
                  "its own ports with `self.` because bare names would collide with child "
                  "instance names (\xc2\xa7""6.9.2)")});
        return false;
    }
    return true;
}

bool Parser::parse_wire(ast::Wire& out) {
    out.loc = tok_.loc;
    if (!parse_endpoint(out.source)) return false;

    if (!at(Tok::Arrow)) {
        if (swallow_invalid()) return false;
        std::vector<Attachment> att;
        if (at(Tok::Equal) || at(Tok::Minus))
            att.push_back(help("the connection operator is `-->`"));
        err("SE0213", tok_.loc, std::string("expected `-->`, found ") + describe(tok_.kind),
            "expected `-->`", std::move(att));
        return false;
    }
    advance();

    for (;;) {
        ast::Endpoint e;
        if (!parse_endpoint(e)) return false;
        out.dests.push_back(std::move(e));
        if (!accept(Tok::Comma)) break;
    }
    return expect(Tok::Semi, "SE0208", "after the wire");
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
