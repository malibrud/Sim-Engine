// ─────────────────────────────────────────────────────────────────────────────
//  Recursive-descent parser.  SPECIFICATION.md §17.
//
//  Implements stages 1–2 only (§1.3): lexical and syntactic. No name
//  resolution, no unit evaluation, no elaboration. One token of lookahead
//  throughout; verbatim regions (§11) are scanned by rewinding the lexer to the
//  current token and handing control to the raw scanners.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_PARSER_HPP
#define SE_PARSER_HPP

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast.hpp"
#include "diag.hpp"
#include "lexer.hpp"
#include "token.hpp"

namespace se {

class Parser {
public:
    Parser(const Source& src, Diagnostics& diag);

    std::unique_ptr<ast::ModelFile> parse_model_file();
    std::unique_ptr<ast::SimFile> parse_sim_file();
    std::unique_ptr<ast::SettingsFile> parse_settings_file();

private:
    // ─── Token stream ────────────────────────────────────────────────────────
    void advance();
    const Token& peek();               // one token beyond the current one
    bool at(Tok k) const { return tok_.kind == k; }
    bool at_end() const { return tok_.kind == Tok::End; }
    bool accept(Tok k);
    bool expect(Tok k, const char* code = "SE0201", const char* context = nullptr);
    // The lexer already reported an Invalid token, so consume it and fail
    // quietly rather than emitting a second diagnostic for the same bytes.
    bool swallow_invalid();
    // Rewind the lexer so raw scanning starts at the current token.
    void enter_raw();
    void resync();

    // ─── Diagnostics ─────────────────────────────────────────────────────────
    void err(const char* code, Loc loc, std::string msg, std::string caret = {},
             std::vector<Attachment> att = {});
    void warn(const char* code, Loc loc, std::string msg, std::string caret = {},
              std::vector<Attachment> att = {});
    bool give_up() const { return diag_.exhausted(); }

    // ─── Recovery (§16.3) ────────────────────────────────────────────────────
    void recover_in_list(Tok closer);
    void recover_in_node_body();
    void recover_to_definition();

    // ─── Shared productions ──────────────────────────────────────────────────
    bool parse_path(ast::Path& out, bool allow_keywords = false);
    bool parse_qualified_name(ast::Path& out);
    ast::UnitPtr parse_unit();
    ast::UnitPtr parse_unit_expr();
    ast::UnitPtr parse_unit_term();
    ast::UnitPtr parse_unit_factor();
    bool parse_unit_exponent(long& num, long& den);
    bool parse_type_ref(ast::TypeRef& out, const char* what);
    ast::ExprPtr parse_expr();
    ast::ExprPtr parse_additive();
    ast::ExprPtr parse_multiplicative();
    ast::ExprPtr parse_unary();
    ast::ExprPtr parse_power();
    ast::ExprPtr parse_primary();
    bool parse_bindings(std::vector<ast::Binding>& out);

    // ─── Model file ──────────────────────────────────────────────────────────
    bool parse_type_def(ast::TypeDef& out);
    bool parse_node_def(ast::NodeDef& out);
    bool parse_node_item(ast::NodeDef& node);
    bool parse_field_list(std::vector<ast::FieldDecl>& out, const char* what,
                          bool allow_records);
    bool parse_setting_list(std::vector<ast::SettingDecl>& out);
    bool parse_state_list(std::vector<ast::StateDecl>& out);
    bool parse_native_list(std::vector<ast::NativeDecl>& out);
    bool parse_build_list(std::vector<ast::BuildStmt>& out);
    bool parse_structure(ast::Structure& out);
    bool parse_instance(ast::Instance& out);
    bool parse_wire(ast::Wire& out);
    bool parse_endpoint(ast::Endpoint& out);
    bool parse_method(ast::NodeDef& node);
    bool parse_helper(ast::NodeDef& node);
    bool mark_section(ast::SectionMark& m, const char* name, Loc loc);

    // ─── Sim file ────────────────────────────────────────────────────────────
    bool parse_sim_entry(ast::SimFile& out);
    bool parse_record_block(ast::RecordBlock& out);
    bool parse_log_block(ast::LogBlock& out);

    const Source& src_;
    Diagnostics& diag_;
    Lexer lexer_;
    Token tok_;
    std::optional<Token> peeked_;
};

}  // namespace se

#endif  // SE_PARSER_HPP
