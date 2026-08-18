// ─────────────────────────────────────────────────────────────────────────────
//  Tokens.  SPECIFICATION.md §3.
//
//  The reserved-word set is deliberately tiny (§3.4): every sim-file key, every
//  build primitive, `when`, and every platform atom is CONTEXTUAL, so adding one
//  later can never invalidate a model that used the word as a port name.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_TOKEN_HPP
#define SE_TOKEN_HPP

#include <string>
#include <string_view>

#include "diag.hpp"

namespace se {

enum class Tok {
    End,
    Invalid,   // already diagnosed by the lexer

    Ident,
    Number,
    String,

    // Reserved words — §3.4 and Appendix B.
    KwPackage, KwUse, KwType, KwNode, KwSelf,
    KwSettings, KwInputs, KwOutputs, KwStates, KwVars, KwNative, KwUnits,
    KwStructure, KwDeclarations, KwBuild,
    KwContinuous, KwDiscrete,
    KwInit, KwOutput, KwRates, KwOnStep, KwFinal,
    KwSim,

    // Punctuation — §3.7.
    LBrace, RBrace, LParen, RParen,
    Semi, Comma, Colon, Dot, Equal,
    Arrow,      // -->
    Plus, Minus, Star, Slash, Caret, Percent,
};

struct Token {
    Tok kind = Tok::End;
    Loc loc;
    // For Ident/Number: the exact source spelling. For String: the decoded
    // value (escapes resolved). For keywords: the keyword spelling.
    std::string text;

    bool is(Tok k) const { return kind == k; }
    bool is_keyword() const { return kind >= Tok::KwPackage && kind <= Tok::KwSim; }
    // §13.1 / §17: a sim-file or record-block key may be spelled with a word
    // that happens to be reserved (`settings:`), so keys accept both.
    bool is_word() const { return kind == Tok::Ident || is_keyword(); }
};

// Human-readable spelling for diagnostics ("expected `;`, found `}`").
const char* describe(Tok k);

// Reserved-word lookup; returns Tok::Ident when the word is not reserved.
Tok keyword_kind(std::string_view word);

}  // namespace se

#endif  // SE_TOKEN_HPP
