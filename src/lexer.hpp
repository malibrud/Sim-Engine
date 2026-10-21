// ─────────────────────────────────────────────────────────────────────────────
//  Lexer.  SPECIFICATION.md §3, plus the verbatim-region scanners of §11.
//
//  Invariant I1: the compiler NEVER parses C++. Verbatim regions are found by
//  bracket matching (§11.2) or by scanning to a terminator (§11.3), and copied
//  through byte for byte.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_LEXER_HPP
#define SE_LEXER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "diag.hpp"
#include "token.hpp"

namespace se {

// A verbatim C++ region: text copied through untouched, plus enough position
// information for the emitter to write a correct `#line` (§11.2).
struct Verbatim {
    std::string text;              // exact source text, interior only for blocks
    Loc open;                      // the `{` for a block; the first char otherwise
    std::uint32_t first_line = 0;  // line of text[0] — what `#line` must name
    bool present = false;
};

// One `///` doc comment line (§3.2). The lexer records these and otherwise
// skips them like any comment; `sec --doc` attaches them to declarations by
// line, so no other stage ever sees them.
struct DocLine {
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::uint32_t offset = 0;
    std::string text;        // after `///` and at most one space
    std::uint32_t text_col = 0;   // column of text[0], for carets inside it
    bool trailing = false;   // something other than whitespace precedes it
};

class Lexer {
public:
    Lexer(const Source& src, Diagnostics& diag);

    // Every `///` line seen in token mode, in source order. A `///` inside a
    // verbatim C++ region is C++'s business and is not recorded.
    const std::vector<DocLine>& doc_lines() const { return docs_; }

    // Token mode (§3).
    Token next();

    // Rewind so that the next call to next() re-lexes from `loc`. Used when the
    // parser has looked ahead and then decides the region is verbatim.
    void seek(const Loc& loc);

    // Current scan position, as a zero-length Loc.
    Loc here() const;

    // The next character past whitespace and comments, without lexing or
    // consuming anything. Lets the parser see that an item continues in C++ --
    // `template <class T>` -- before token mode reports the `<` as an illegal
    // character in a region that is about to turn out verbatim (§11.4).
    char peek_char() const;

    // ─── Verbatim scanners (§11) ─────────────────────────────────────────────
    // Each starts at the current position and leaves the position as noted.
    // All return false (having reported) if the region is unterminated.

    // Requires the current character to be '{'. Consumes through the matching
    // '}'. `out.text` is the interior, without the braces.
    bool scan_brace_block(Verbatim& out);

    // Scans to the next ';' at bracket depth 0. Leaves the position AT the ';'.
    // Used for `native` member type text (§11.3).
    bool scan_to_semi(Verbatim& out);

    // Scans to the first '{' at bracket depth 0 — a helper function's signature
    // (§11.4). Leaves the position AT the '{'. Reports SE0240 if a ';' at depth
    // 0 arrives first, i.e. the helper is a declaration rather than a definition.
    bool scan_signature(Verbatim& out);

private:
    // Position primitives.
    bool eof() const { return pos_ >= text_.size(); }
    char cur() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    char at(std::size_t k) const {
        return pos_ + k < text_.size() ? text_[pos_ + k] : '\0';
    }
    void bump();
    Loc mark() const;
    void finish(Loc& loc) const;   // set loc.length from the current position

    // Token mode.
    void skip_trivia();            // whitespace + both comment forms
    Token lex_number(Loc start);
    Token lex_string(Loc start);
    Token lex_word(Loc start);

    // Raw mode. Consumes one C++ construct inside which braces, semicolons and
    // quotes must not be counted. Returns true if something was consumed.
    // Sets `ok` to false on an unterminated raw string literal.
    bool skip_cpp_trivia(bool& ok);
    bool skip_raw_string();
    void skip_quoted(char terminator);
    bool is_char_literal_start() const;   // the `1'000'000` trap (§11.2)

    const Source& src_;
    Diagnostics& diag_;
    std::string_view text_;

    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;
    std::size_t line_start_ = 0;

    // Last non-whitespace byte consumed in raw mode — needed to tell a character
    // literal from a digit separator.
    char last_sig_ = '\0';

    // `seek()` re-lexes, so a doc line is recorded only past the last one kept.
    std::vector<DocLine> docs_;
};

}  // namespace se

#endif  // SE_LEXER_HPP
