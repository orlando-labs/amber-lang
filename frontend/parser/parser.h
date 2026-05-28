#pragma once

#include "frontend/ast/expr.h"
#include "frontend/lexer/token.h"

#include <memory>
#include <string>
#include <vector>

namespace amber::parser {

struct ParseResult {
  std::unique_ptr<ast::Expr> expr;
  std::vector<lexer::Diagnostic> diagnostics;

  bool ok() const { return diagnostics.empty() && expr != nullptr; }
};

struct ParseModuleResult {
  std::vector<std::unique_ptr<ast::Expr>> items;
  std::string module_name;
  std::vector<lexer::Diagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

class Parser {
public:
  explicit Parser(const std::vector<lexer::Token> &tokens);

  ParseResult parse_expression_unit();
  ParseModuleResult parse_module_unit();

private:
  enum class StopMode { Normal, InlineBlock };

  enum class BodyContext { Module, Class, Mixin, Def };

  struct ClauseBody {
    std::vector<std::unique_ptr<ast::Expr>> clauses;
    std::vector<std::unique_ptr<ast::Expr>> else_body;
  };

  enum class Assoc { Left, Right };

  struct InfixInfo {
    int precedence;
    Assoc assoc;
    const char *op;
  };

  const lexer::Token &current() const;
  const lexer::Token &previous() const;
  const lexer::Token &peek(std::size_t distance = 1) const;
  bool at_end() const;
  bool check(lexer::TokenKind kind) const;
  bool match(lexer::TokenKind kind);
  const lexer::Token &advance();
  const lexer::Token &consume(lexer::TokenKind kind,
                              const std::string &message);

  std::unique_ptr<ast::Expr> parse_statement(BodyContext context);
  std::unique_ptr<ast::Expr> parse_package_decl();
  std::unique_ptr<ast::Expr> parse_import_decl();
  std::unique_ptr<ast::Expr> parse_from_import_decl();
  std::unique_ptr<ast::Expr> parse_export_stmt();
  std::unique_ptr<ast::Expr>
  parse_def_stmt(bool class_method,
                 const lexer::Token *start_override = nullptr);
  std::unique_ptr<ast::Expr> parse_class_def();
  std::unique_ptr<ast::Expr> parse_mixin_def();
  std::unique_ptr<ast::Expr> parse_include_stmt(bool extend);
  std::unique_ptr<ast::Expr> parse_pass_like_stmt(const char *kind);
  std::vector<std::unique_ptr<ast::Expr>> parse_body(BodyContext context);
  std::unique_ptr<ast::Expr> parse_signature();
  std::unique_ptr<ast::Expr> parse_param();
  std::string parse_type_term_text_until_param_boundary();
  std::string parse_type_term_text_until_return_boundary();
  std::string parse_effect_row_text();
  ClauseBody parse_clause_body();
  std::unique_ptr<ast::Expr> parse_clause();
  std::unique_ptr<ast::Expr> parse_if_expr();
  std::vector<std::unique_ptr<ast::Expr>> parse_if_tail();
  std::unique_ptr<ast::Expr> parse_unless_expr();
  std::unique_ptr<ast::Expr> parse_loop_expr(const char *kind);
  std::unique_ptr<ast::Expr> parse_do_while_expr();
  std::unique_ptr<ast::Expr> parse_break_expr();
  std::unique_ptr<ast::Expr> parse_case_expr(bool strict);
  std::unique_ptr<ast::Expr> parse_case_arm();
  std::vector<std::unique_ptr<ast::Expr>>
  parse_control_body(BodyContext context);
  std::vector<std::unique_ptr<ast::Expr>>
  parse_path_list(const std::string &item_kind);
  std::unique_ptr<ast::Expr> parse_path_node(const std::string &kind);
  std::unique_ptr<ast::Expr> try_parse_pattern_assignment();
  std::string parse_module_path();
  std::string parse_clause_pattern_text();
  std::vector<std::unique_ptr<ast::Expr>> parse_block_params();
  std::vector<std::string> parse_many_def_patterns(lexer::Span *span_out);
  std::string consume_method_name_text(const std::string &message);
  std::string consume_identifier_text(const std::string &message);
  bool match_contextual(const char *text);
  bool starts_clause_body() const;
  bool is_simple_many_def_header() const;

  std::unique_ptr<ast::Expr> parse_expression(int min_precedence,
                                              StopMode stop_mode);
  std::unique_ptr<ast::Expr> parse_prefix(StopMode stop_mode);
  std::unique_ptr<ast::Expr>
  parse_paren_or_tuple_literal(const lexer::Token &open, StopMode stop_mode);
  std::unique_ptr<ast::Expr>
  parse_brace_collection_literal(const lexer::Token &open, StopMode stop_mode);
  std::unique_ptr<ast::Expr> parse_set_literal(const lexer::Token &open,
                                               StopMode stop_mode);
  std::unique_ptr<ast::Expr> parse_map_literal(const lexer::Token &open,
                                               StopMode stop_mode);
  std::unique_ptr<ast::Expr>
  parse_string_literal_expr(const lexer::Token &token);
  std::unique_ptr<ast::Expr> parse_postfix(std::unique_ptr<ast::Expr> expr,
                                           StopMode stop_mode);
  std::unique_ptr<ast::Expr> parse_block_suffix(StopMode stop_mode);
  std::vector<std::unique_ptr<ast::Expr>> parse_paren_args(StopMode stop_mode);
  std::vector<std::unique_ptr<ast::Expr>> parse_bare_args(StopMode stop_mode);
  std::vector<std::unique_ptr<ast::Expr>>
  parse_expr_list(lexer::TokenKind closing_kind, StopMode stop_mode);

  bool is_stop_token(StopMode stop_mode) const;
  bool starts_primary() const;
  bool starts_bare_arg() const;
  bool starts_map_literal_entry() const;
  bool can_accept_bare_call(const ast::Expr &expr) const;
  bool can_accept_direct_block_suffix(const ast::Expr &expr) const;
  bool is_assignable(const ast::Expr &expr) const;
  bool infix_info(lexer::TokenKind kind, InfixInfo *info) const;
  bool is_method_name_token(const lexer::Token &token) const;

  void error(const lexer::Token &token, const std::string &message);
  void error_code(const lexer::Token &token, const std::string &code,
                  const std::string &message);
  lexer::Span current_zero_width_span() const;

  const std::vector<lexer::Token> &tokens_;
  std::size_t current_ = 0;
  std::vector<lexer::Diagnostic> diagnostics_;
  lexer::Token synthetic_error_token_;
};

} // namespace amber::parser
