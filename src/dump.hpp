// ─────────────────────────────────────────────────────────────────────────────
//  AST dumper — a readable, diffable rendering of stage-2 output.
//  Used by `sec --dump-ast` and by the test corpus.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_DUMP_HPP
#define SE_DUMP_HPP

#include <iosfwd>

#include "ast.hpp"

namespace se {

void dump(std::ostream& out, const ast::ModelFile& file);
void dump(std::ostream& out, const ast::SimFile& file);
void dump(std::ostream& out, const ast::SettingsFile& file);

// Rendered forms, also useful in diagnostics.
std::string unit_to_string(const ast::UnitExpr* u);
std::string expr_to_string(const ast::Expr* e);

}  // namespace se

#endif  // SE_DUMP_HPP
