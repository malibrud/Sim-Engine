#include "lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace se {

// ─── Token spellings and the reserved-word table ─────────────────────────────

const char* describe(Tok k) {
    switch (k) {
        case Tok::End:            return "end of file";
        case Tok::Invalid:        return "invalid token";
        case Tok::Ident:          return "an identifier";
        case Tok::Number:         return "a number";
        case Tok::String:         return "a string";
        case Tok::KwPackage:      return "`package`";
        case Tok::KwUse:          return "`use`";
        case Tok::KwType:         return "`type`";
        case Tok::KwNode:         return "`node`";
        case Tok::KwSelf:         return "`self`";
        case Tok::KwSettings:     return "`settings`";
        case Tok::KwInputs:       return "`inputs`";
        case Tok::KwOutputs:      return "`outputs`";
        case Tok::KwStates:       return "`states`";
        case Tok::KwVars:         return "`vars`";
        case Tok::KwUnits:        return "`units`";
        case Tok::KwNative:       return "`native`";
        case Tok::KwStructure:    return "`structure`";
        case Tok::KwDeclarations: return "`declarations`";
        case Tok::KwBuild:        return "`build`";
        case Tok::KwContinuous:   return "`continuous`";
        case Tok::KwDiscrete:     return "`discrete`";
        case Tok::KwInit:         return "`init`";
        case Tok::KwOutput:       return "`output`";
        case Tok::KwDerivative:   return "`derivative`";
        case Tok::KwNext:         return "`next`";
        case Tok::KwOnStep:       return "`on_step`";
        case Tok::KwFinal:        return "`final`";
        case Tok::KwSim:          return "`sim`";
        case Tok::LBrace:         return "`{`";
        case Tok::RBrace:         return "`}`";
        case Tok::LParen:         return "`(`";
        case Tok::RParen:         return "`)`";
        case Tok::LBracket:       return "`[`";
        case Tok::RBracket:       return "`]`";
        case Tok::Semi:           return "`;`";
        case Tok::Comma:          return "`,`";
        case Tok::Colon:          return "`:`";
        case Tok::Dot:            return "`.`";
        case Tok::Equal:          return "`=`";
        case Tok::Arrow:          return "`-->`";
        case Tok::Plus:           return "`+`";
        case Tok::Minus:          return "`-`";
        case Tok::Star:           return "`*`";
        case Tok::Slash:          return "`/`";
        case Tok::Caret:          return "`^`";
        case Tok::Percent:        return "`%`";
    }
    return "a token";
}

Tok keyword_kind(std::string_view w) {
    static const std::unordered_map<std::string_view, Tok> table = {
        {"package", Tok::KwPackage},           {"use", Tok::KwUse},
        {"type", Tok::KwType},                 {"node", Tok::KwNode},
        {"self", Tok::KwSelf},                 {"settings", Tok::KwSettings},
        {"inputs", Tok::KwInputs},             {"outputs", Tok::KwOutputs},
        {"states", Tok::KwStates},             {"vars", Tok::KwVars},
        {"units", Tok::KwUnits},
        {"native", Tok::KwNative},             {"structure", Tok::KwStructure},
        {"declarations", Tok::KwDeclarations}, {"build", Tok::KwBuild},
        {"continuous", Tok::KwContinuous},     {"discrete", Tok::KwDiscrete},
        {"init", Tok::KwInit},                 {"output", Tok::KwOutput},
        {"derivative", Tok::KwDerivative},     {"next", Tok::KwNext},
        {"on_step", Tok::KwOnStep},
        {"final", Tok::KwFinal},               {"sim", Tok::KwSim},
    };
    auto it = table.find(w);
    return it == table.end() ? Tok::Ident : it->second;
}

// ─── Construction and position ───────────────────────────────────────────────

Lexer::Lexer(const Source& src, Diagnostics& diag)
    : src_(src), diag_(diag), text_(src.text()) {}

void Lexer::bump() {
    if (eof()) return;
    const char c = text_[pos_];
    ++pos_;
    if (c == '\n') {
        ++line_;
        line_start_ = pos_;
    } else if (c == '\r') {
        // CRLF counts once, at the LF; a lone CR breaks the line here.
        if (cur() != '\n') {
            ++line_;
            line_start_ = pos_;
        }
    }
}

Loc Lexer::mark() const {
    Loc l;
    l.line = line_;
    l.col = static_cast<std::uint32_t>(pos_ - line_start_ + 1);
    l.offset = static_cast<std::uint32_t>(pos_);
    l.length = 0;
    return l;
}

void Lexer::finish(Loc& loc) const {
    loc.length = static_cast<std::uint32_t>(pos_ - loc.offset);
    if (loc.length == 0) loc.length = 1;
}

Loc Lexer::here() const { return mark(); }

void Lexer::seek(const Loc& loc) {
    pos_ = loc.offset;
    line_ = loc.line;
    // Recover the line start from the column so that `mark()` stays exact.
    line_start_ = (loc.col > 0) ? loc.offset - (loc.col - 1) : loc.offset;
    last_sig_ = '\0';
}

// ─── Token mode ──────────────────────────────────────────────────────────────

void Lexer::skip_trivia() {
    for (;;) {
        const char c = cur();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            bump();
        } else if (c == '/' && at(1) == '/') {
            while (!eof() && cur() != '\n' && cur() != '\r') bump();
        } else if (c == '/' && at(1) == '*') {
            const Loc open = mark();
            bump();
            bump();
            for (;;) {
                if (eof()) {
                    Loc l = open;
                    l.length = 2;
                    diag_.error("SE0102", src_, l, "unterminated block comment",
                                "opened here",
                                {note("expected `*/` before end of file"),
                                 note("block comments do not nest (SPECIFICATION.md \xc2\xa7""3.2)")});
                    return;
                }
                if (cur() == '*' && at(1) == '/') {
                    bump();
                    bump();
                    break;
                }
                bump();
            }
        } else {
            return;
        }
    }
}

Token Lexer::lex_number(Loc start) {
    // §3.5: integer | decimal, with no suffixes, separators, hex or octal.
    bool ok = true;
    bool any_digit = false;
    while (std::isdigit(static_cast<unsigned char>(cur()))) {
        bump();
        any_digit = true;
    }
    if (cur() == '.') {
        bump();
        while (std::isdigit(static_cast<unsigned char>(cur()))) {
            bump();
            any_digit = true;
        }
    }
    if (!any_digit) ok = false;
    if (cur() == 'e' || cur() == 'E') {
        const std::size_t save = pos_;
        const std::uint32_t save_line = line_;
        const std::size_t save_ls = line_start_;
        bump();
        if (cur() == '+' || cur() == '-') bump();
        if (!std::isdigit(static_cast<unsigned char>(cur()))) {
            // Not an exponent after all (`1e` could be `1` followed by a name),
            // but per §3.5 that is a malformed literal, not two tokens.
            pos_ = save;
            line_ = save_line;
            line_start_ = save_ls;
            ok = false;
        } else {
            while (std::isdigit(static_cast<unsigned char>(cur()))) bump();
        }
    }
    // A trailing identifier character means a suffix, which does not exist.
    if (std::isalpha(static_cast<unsigned char>(cur())) || cur() == '_') {
        while (std::isalnum(static_cast<unsigned char>(cur())) || cur() == '_') bump();
        ok = false;
    }

    Token t;
    t.loc = start;
    finish(t.loc);
    t.text = std::string(text_.substr(start.offset, t.loc.length));
    if (!ok) {
        diag_.error("SE0106", src_, t.loc, "malformed number literal `" + t.text + "`", "",
                    {note("§3.5: digits, one optional `.`, one optional `e`/`E` exponent"),
                     note("there are no type suffixes, digit separators, hex or octal literals")});
        t.kind = Tok::Invalid;
    } else {
        t.kind = Tok::Number;
    }
    return t;
}

Token Lexer::lex_string(Loc start) {
    bump();  // the opening quote
    std::string value;
    for (;;) {
        if (eof() || cur() == '\n' || cur() == '\r') {
            Loc l = start;
            l.length = 1;
            diag_.error("SE0103", src_, l, "unterminated string literal", "opened here",
                        {note("a string literal may not span a line break (\xc2\xa7""3.6)")});
            Token t;
            t.kind = Tok::Invalid;
            t.loc = start;
            finish(t.loc);
            return t;
        }
        if (cur() == '"') {
            bump();
            break;
        }
        if (cur() == '\\') {
            const Loc esc = mark();
            bump();
            const char e = cur();
            switch (e) {
                case '"':  value.push_back('"');  bump(); break;
                case '\\': value.push_back('\\'); bump(); break;
                case 'n':  value.push_back('\n'); bump(); break;
                case 'r':  value.push_back('\r'); bump(); break;
                case 't':  value.push_back('\t'); bump(); break;
                case '0':  value.push_back('\0'); bump(); break;
                default: {
                    Loc l = esc;
                    l.length = 2;
                    std::string shown = "\\";
                    if (e) shown.push_back(e);
                    diag_.error("SE0104", src_, l,
                                "unrecognised escape sequence `" + shown + "`", "",
                                {note("§3.6 allows only \\\" \\\\ \\n \\r \\t \\0"),
                                 help("an unknown escape is rejected rather than passed "
                                      "through, so adding escapes later is not a silent "
                                      "behaviour change")});
                    if (e) bump();
                    break;
                }
            }
            continue;
        }
        value.push_back(cur());
        bump();
    }
    Token t;
    t.kind = Tok::String;
    t.loc = start;
    finish(t.loc);
    t.text = std::move(value);
    return t;
}

Token Lexer::lex_word(Loc start) {
    while (std::isalnum(static_cast<unsigned char>(cur())) || cur() == '_') bump();
    Token t;
    t.loc = start;
    finish(t.loc);
    t.text = std::string(text_.substr(start.offset, t.loc.length));
    t.kind = keyword_kind(t.text);
    return t;
}

Token Lexer::next() {
    skip_trivia();
    Loc start = mark();
    Token t;
    t.loc = start;

    if (eof()) {
        t.kind = Tok::End;
        t.loc.length = 0;
        return t;
    }

    const char c = cur();

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return lex_word(start);
    if (std::isdigit(static_cast<unsigned char>(c))) return lex_number(start);
    // §3.5 permits `.5`; a `.` followed by a digit is therefore a number.
    if (c == '.' && std::isdigit(static_cast<unsigned char>(at(1)))) return lex_number(start);
    if (c == '"') return lex_string(start);

    auto one = [&](Tok k) {
        bump();
        t.kind = k;
        finish(t.loc);
        t.text = std::string(text_.substr(start.offset, t.loc.length));
        return t;
    };

    switch (c) {
        case '{': return one(Tok::LBrace);
        case '}': return one(Tok::RBrace);
        case '(': return one(Tok::LParen);
        case ')': return one(Tok::RParen);
        case '[': return one(Tok::LBracket);
        case ']': return one(Tok::RBracket);
        case ';': return one(Tok::Semi);
        case ',': return one(Tok::Comma);
        case ':': return one(Tok::Colon);
        case '.': return one(Tok::Dot);
        case '=': return one(Tok::Equal);
        case '+': return one(Tok::Plus);
        case '*': return one(Tok::Star);
        case '/': return one(Tok::Slash);
        case '^': return one(Tok::Caret);
        case '%': return one(Tok::Percent);
        case '-':
            // Maximal munch: `-->` always wins over `-` `-` `>` (§3.7).
            if (at(1) == '-' && at(2) == '>') {
                bump();
                bump();
                bump();
                t.kind = Tok::Arrow;
                finish(t.loc);
                t.text = "-->";
                return t;
            }
            return one(Tok::Minus);
        default: break;
    }

    // Everything else is illegal outside a verbatim region.
    bump();
    finish(t.loc);
    t.kind = Tok::Invalid;
    t.text = std::string(text_.substr(start.offset, t.loc.length));

    if (c == '>') {
        diag_.error("SE0105", src_, t.loc, "unexpected `>`", "",
                    {note("`>` is not a token in this language; the connection "
                          "operator is `-->` (\xc2\xa7""3.7)")});
    } else if (static_cast<unsigned char>(c) >= 0x80) {
        // Consume the rest of the UTF-8 sequence so the caret spans the glyph.
        while (!eof() && (static_cast<unsigned char>(cur()) & 0xC0) == 0x80) bump();
        finish(t.loc);
        diag_.error("SE0101", src_, t.loc,
                    "non-ASCII character outside a comment, string or verbatim region", "",
                    {note("§3.1: only the ASCII subset is significant in code"),
                     help("non-ASCII text is fine in comments, string literals and C++ bodies")});
    } else {
        std::string shown(1, c);
        diag_.error("SE0101", src_, t.loc, "illegal character `" + shown + "` in source", "");
    }
    return t;
}

// ─── Raw mode (§11) ──────────────────────────────────────────────────────────

// The `1'000'000` trap (§11.2 trap 1): a `'` opens a character literal only when
// the previous significant byte cannot end a numeric or identifier token.
bool Lexer::is_char_literal_start() const {
    if (cur() != '\'') return false;
    const unsigned char p = static_cast<unsigned char>(last_sig_);
    if (std::isalnum(p) || p == '_') return false;
    return true;
}

void Lexer::skip_quoted(char terminator) {
    bump();  // opening delimiter
    while (!eof()) {
        const char c = cur();
        if (c == '\\') {
            bump();
            if (!eof()) bump();   // the escaped byte, including an escaped newline
            continue;
        }
        if (c == terminator) {
            bump();
            return;
        }
        // An unterminated C++ literal cannot cross a raw line break; stopping
        // here keeps brace matching from being derailed by an apostrophe.
        if (c == '\n' || c == '\r') return;
        bump();
    }
}

// R"delim( … )delim"  — §11.2 trap 2.
bool Lexer::skip_raw_string() {
    const Loc open = mark();
    bump();  // R
    bump();  // "
    std::string delim;
    while (!eof() && cur() != '(' && delim.size() <= 16) {
        delim.push_back(cur());
        bump();
    }
    if (cur() != '(') {
        Loc l = open;
        l.length = 2;
        diag_.error("SE0111", src_, l, "malformed raw string literal", "opened here",
                    {note("expected `(` after the delimiter in `R\"delim(`")});
        return false;
    }
    bump();  // (
    const std::string close = ")" + delim + "\"";
    while (!eof()) {
        if (cur() == ')' && text_.compare(pos_, close.size(), close) == 0) {
            for (std::size_t i = 0; i < close.size(); ++i) bump();
            return true;
        }
        bump();
    }
    Loc l = open;
    l.length = 2;
    diag_.error("SE0111", src_, l, "unterminated raw string literal", "opened here",
                {note("expected `)" + delim + "\"` before end of file")});
    return false;
}

bool Lexer::skip_cpp_trivia(bool& ok) {
    const char c = cur();
    if (c == '/' && at(1) == '/') {
        while (!eof() && cur() != '\n' && cur() != '\r') bump();
        return true;
    }
    if (c == '/' && at(1) == '*') {
        bump();
        bump();
        while (!eof() && !(cur() == '*' && at(1) == '/')) bump();
        if (!eof()) {
            bump();
            bump();
        }
        return true;
    }
    if (c == 'R' && at(1) == '"') {
        // Only a raw-string prefix when `R` does not continue an identifier —
        // except for the legal encoding prefixes u8R, uR, UR, LR.
        const unsigned char p = static_cast<unsigned char>(last_sig_);
        const bool prefix_ok = !(std::isalnum(p) || p == '_') || p == 'u' || p == 'U' ||
                               p == 'L' || p == '8';
        if (prefix_ok) {
            if (!skip_raw_string()) ok = false;
            last_sig_ = '"';
            return true;
        }
    }
    if (c == '"') {
        skip_quoted('"');
        last_sig_ = '"';
        return true;
    }
    if (is_char_literal_start()) {
        skip_quoted('\'');
        last_sig_ = '\'';
        return true;
    }
    return false;
}

bool Lexer::scan_brace_block(Verbatim& out) {
    if (cur() != '{') return false;
    const Loc open = mark();
    bump();  // the opening brace

    const std::size_t body_start = pos_;
    const std::uint32_t body_line = line_;
    int depth = 1;
    bool ok = true;
    last_sig_ = '{';

    while (!eof()) {
        if (skip_cpp_trivia(ok)) continue;
        const char c = cur();
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                out.text = std::string(text_.substr(body_start, pos_ - body_start));
                out.open = open;
                out.open.length = 1;
                out.first_line = body_line;
                out.present = true;
                bump();  // the matching brace
                return ok;
            }
        }
        if (!std::isspace(static_cast<unsigned char>(c))) last_sig_ = c;
        bump();
    }

    Loc l = open;
    l.length = 1;
    diag_.error("SE0110", src_, l, "unterminated verbatim region", "opened here",
                {note("reached end of file still " + std::to_string(depth) +
                      " brace" + (depth == 1 ? "" : "s") + " deep"),
                 note("braces inside comments, string, character and raw string "
                      "literals are not counted (\xc2\xa7""11.2)")});
    out.text = std::string(text_.substr(body_start));
    out.open = l;
    out.first_line = body_line;
    out.present = true;
    return false;
}

bool Lexer::scan_to_semi(Verbatim& out) {
    const std::size_t start = pos_;
    const Loc open = mark();
    int depth = 0;
    bool ok = true;
    last_sig_ = ':';

    while (!eof()) {
        if (skip_cpp_trivia(ok)) continue;
        const char c = cur();
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) --depth;
        } else if (c == ';' && depth == 0) {
            std::string text(text_.substr(start, pos_ - start));
            // Trim; §11.3 keeps interior bytes exactly.
            std::size_t b = text.find_first_not_of(" \t\r\n");
            std::size_t e = text.find_last_not_of(" \t\r\n");
            out.text = (b == std::string::npos) ? std::string() : text.substr(b, e - b + 1);
            out.open = open;
            out.open.length = static_cast<std::uint32_t>(pos_ - start);
            if (out.open.length == 0) out.open.length = 1;
            out.first_line = open.line;
            out.present = true;
            return ok;   // position left AT the ';'
        }
        if (!std::isspace(static_cast<unsigned char>(c))) last_sig_ = c;
        bump();
    }

    Loc l = open;
    l.length = 1;
    diag_.error("SE0112", src_, l, "unterminated `native` member type", "starts here",
                {note("a `native` declaration's C++ type text runs to a `;` at bracket "
                      "depth 0 (\xc2\xa7""11.3)")});
    return false;
}

bool Lexer::scan_signature(Verbatim& out) {
    const std::size_t start = pos_;
    const Loc open = mark();
    int depth = 0;
    bool ok = true;
    last_sig_ = '\0';

    while (!eof()) {
        if (skip_cpp_trivia(ok)) continue;
        const char c = cur();
        if (c == '(' || c == '[') {
            ++depth;
        } else if (c == ')' || c == ']') {
            if (depth > 0) --depth;
        } else if (c == '{' && depth == 0) {
            std::string text(text_.substr(start, pos_ - start));
            std::size_t b = text.find_first_not_of(" \t\r\n");
            std::size_t e = text.find_last_not_of(" \t\r\n");
            out.text = (b == std::string::npos) ? std::string() : text.substr(b, e - b + 1);
            out.open = open;
            out.open.length = static_cast<std::uint32_t>(pos_ - start);
            if (out.open.length == 0) out.open.length = 1;
            out.first_line = open.line;
            out.present = true;
            return ok;   // position left AT the '{'
        } else if (c == ';' && depth == 0) {
            // §11.4: a helper must be a definition, not a declaration.
            Loc l = mark();
            l.length = 1;
            diag_.error("SE0240", src_, l, "expected a function body", "found `;`",
                        {note("a node-body item that is not a section or a lifecycle "
                              "method is a verbatim helper function, which must be a "
                              "definition (\xc2\xa7""11.4)"),
                         help("put forward declarations, `#include`s and free functions "
                              "in `declarations { ... }`")});
            bump();
            return false;
        } else if (c == '}' && depth == 0) {
            // Ran out of node body without finding a signature's brace.
            Loc l = open;
            l.length = 1;
            diag_.error("SE0201", src_, l, "expected a section, a lifecycle method, or a "
                                           "helper function definition",
                        "starts here");
            return false;
        }
        if (!std::isspace(static_cast<unsigned char>(c))) last_sig_ = c;
        bump();
    }

    Loc l = open;
    l.length = 1;
    diag_.error("SE0209", src_, l, "unbalanced braces at end of file", "starts here");
    return false;
}

}  // namespace se
