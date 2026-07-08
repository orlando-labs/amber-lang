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
  enum class StopMode { Normal, InlineBlock, InlineIfBranch, ControlHeader };

  enum class BodyContext { Module, Class, Mixin, Def };

  struct ClauseBody {
    std::vector<std::unique_ptr<ast::Expr>> clauses;
    std::vector<std::unique_ptr<ast::Expr>> else_body;
  };

  struct PropertySuite {
    bool grouped = false;
    bool has_getter = false;
    bool has_setter = false;
    lexer::Span end_span;
    std::vector<std::unique_ptr<ast::Expr>> getter_body;
    std::unique_ptr<ast::Expr> setter_signature;
    std::vector<std::unique_ptr<ast::Expr>> setter_body;
  };

  struct HandlerSuffix {
    std::vector<std::unique_ptr<ast::Expr>> rescues;
    std::vector<std::unique_ptr<ast::Expr>> ensure_body;
    bool has_ensure = false;
    lexer::Span end_span;
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
  // Wrap a simple statement in a trailing `... if COND` / `... unless COND`
  // guard when one follows it, reusing the ordinary AstIf / AstUnless nodes.
  std::unique_ptr<ast::Expr>
  maybe_postfix_conditional(std::unique_ptr<ast::Expr> stmt);
  std::unique_ptr<ast::Expr> parse_package_decl();
  std::unique_ptr<ast::Expr> parse_import_decl();
  std::unique_ptr<ast::Expr> parse_from_import_decl();
  std::unique_ptr<ast::Expr> parse_export_stmt();
  std::unique_ptr<ast::Expr> parse_error_decl();
  std::unique_ptr<ast::Expr>
  parse_def_stmt(bool class_method,
                 const lexer::Token *start_override = nullptr,
                 bool is_native = false, bool is_macro = false);
  std::unique_ptr<ast::Expr> parse_prop_def(bool class_property);
  std::unique_ptr<ast::Expr> parse_attr_def();
  PropertySuite parse_property_suite(const lexer::Span &fallback_span);
  PropertySuite parse_grouped_property_suite(const lexer::Span &fallback_span);
  void parse_property_arm(PropertySuite *suite);
  std::unique_ptr<ast::Expr>
  parse_class_def(const lexer::Token *native_start = nullptr);
  std::unique_ptr<ast::Expr> parse_mixin_def();
  std::unique_ptr<ast::Expr> parse_include_stmt(bool extend);
  std::unique_ptr<ast::Expr> parse_pass_like_stmt(const char *kind);
  std::unique_ptr<ast::Expr> parse_invalid_handler_stmt(bool rescue);
  std::vector<std::unique_ptr<ast::Expr>> parse_body(BodyContext context);
  HandlerSuffix parse_handler_suffix(const lexer::Span &fallback_span,
                                     BodyContext context);
  std::unique_ptr<ast::Expr> parse_rescue_clause(BodyContext context);
  std::string parse_rescue_matcher_text();
  std::string parse_exception_binding();
  std::vector<std::unique_ptr<ast::Expr>>
  parse_ensure_clause(BodyContext context);
  std::unique_ptr<ast::Expr> parse_signature();
  std::unique_ptr<ast::Expr> parse_param();
  std::string parse_type_term_text_until_param_boundary();
  std::string parse_type_term_text_until_return_boundary();
  std::string parse_effect_row_text();
  ClauseBody parse_clause_body();
  std::unique_ptr<ast::Expr> parse_clause();
  std::unique_ptr<ast::Expr> parse_if_expr(StopMode stop_mode);
  std::vector<std::unique_ptr<ast::Expr>> parse_if_tail();
  std::unique_ptr<ast::Expr> parse_unless_expr();
  std::unique_ptr<ast::Expr> parse_loop_expr(const char *kind);
  std::unique_ptr<ast::Expr> parse_do_while_expr();
  std::unique_ptr<ast::Expr> parse_numeric_directive();
  std::unique_ptr<ast::Expr> parse_break_expr();
  std::unique_ptr<ast::Expr> parse_next_expr();
  std::unique_ptr<ast::Expr> parse_return_expr();
  std::unique_ptr<ast::Expr> parse_throw_expr();
  std::unique_ptr<ast::Expr> parse_catch_expr();
  std::unique_ptr<ast::Expr> parse_raise_expr();
  std::unique_ptr<ast::Expr> parse_try_expr();
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
  const lexer::Token &consume_identifier_like(const std::string &message);
  // Like consume_identifier_like, but also accepts the boolean operator-word
  // keywords (or/and/not) as member/method names after a `.` (e.g. Result#or).
  const lexer::Token &consume_member_name(const std::string &message);
  std::string consume_identifier_text(const std::string &message);
  bool looks_like_property_declaration() const;
  bool looks_like_attr_declaration() const;
  bool property_suite_starts_grouped() const;
  bool match_contextual(const char *text);
  bool starts_clause_body() const;
  bool is_simple_many_def_header() const;

  std::unique_ptr<ast::Expr> parse_expression(int min_precedence,
                                              StopMode stop_mode);
  std::unique_ptr<ast::Expr>
  parse_comparison_chain(std::unique_ptr<ast::Expr> left,
                         const InfixInfo &first_info,
                         const lexer::Token &first_op_token,
                         StopMode stop_mode);
  std::unique_ptr<ast::Expr> parse_prefix(StopMode stop_mode);
  std::unique_ptr<ast::Expr>
  parse_paren_or_tuple_literal(const lexer::Token &open, StopMode stop_mode);
  std::unique_ptr<ast::Expr>
  parse_brace_collection_literal(const lexer::Token &open, StopMode stop_mode);
  std::vector<std::unique_ptr<ast::Expr>>
  parse_collection_elements(lexer::TokenKind closing_kind,
                            const char *conditional_kind,
                            StopMode stop_mode);
  std::unique_ptr<ast::Expr> parse_collection_element(
      lexer::TokenKind closing_kind, const char *conditional_kind,
      StopMode stop_mode);
  std::unique_ptr<ast::Expr> parse_collection_condition();
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
  std::vector<std::unique_ptr<ast::Expr>>
  parse_call_arg_list(lexer::TokenKind closing_kind, StopMode stop_mode);
  std::unique_ptr<ast::Expr> parse_call_arg(StopMode stop_mode,
                                            lexer::TokenKind closing_kind);
  std::vector<std::unique_ptr<ast::Expr>> parse_bare_args(StopMode stop_mode);
  std::vector<std::unique_ptr<ast::Expr>>
  parse_expr_list(lexer::TokenKind closing_kind, StopMode stop_mode);

  bool is_stop_token(StopMode stop_mode) const;
  bool starts_primary() const;
  bool starts_bare_arg() const;
  bool starts_indented_postfix_continuation() const;
  bool starts_same_indent_postfix_continuation() const;
  bool starts_map_literal_entry() const;
  bool is_contextual_at(std::size_t index, const char *text) const;
  bool is_literal_float_expr(const ast::Expr &expr) const;
  bool is_literal_zero_expr(const ast::Expr &expr) const;
  std::unique_ptr<ast::Expr> make_null_literal(const lexer::Span &span) const;
  bool token_slice_parses_expression(std::size_t begin,
                                     std::size_t end) const;
  std::size_t find_inline_then_delimiter(std::size_t begin) const;
  bool can_accept_bare_call(const ast::Expr &expr) const;
  bool can_accept_direct_block_suffix(const ast::Expr &expr) const;
  bool is_assignable(const ast::Expr &expr) const;
  bool is_optional_bracket_access(const ast::Expr &expr) const;
  bool infix_info(lexer::TokenKind kind, InfixInfo *info) const;
  bool compound_assignment_op(lexer::TokenKind kind, const char **op) const;
  bool is_method_name_token(const lexer::Token &token) const;

  void error(const lexer::Token &token, const std::string &message);
  void error_code(const lexer::Token &token, const std::string &code,
                  const std::string &message);
  lexer::Span current_zero_width_span() const;

  const std::vector<lexer::Token> &tokens_;
  std::size_t current_ = 0;
  std::vector<lexer::Diagnostic> diagnostics_;
  // True while parsing the body of a `native class`; a plain `def ... from`
  // binding is only valid in that context (otherwise use `native def`).
  bool in_native_class_body_ = false;
  // Nesting depth of enclosing `quote:` bodies. Inside a quote (> 0) an
  // `unquote(...)` / `unquote_splice(...)` call is a splice hole; at depth 0 it
  // is an ordinary call. Used by parse_prefix.
  int quote_depth_ = 0;
  // Non-zero while parsing a `macro def` body: a line-leading `%` there marks
  // a compile-time control statement (DESIGN-macro-system §6 `%`-lines).
  int macro_body_depth_ = 0;
  lexer::Token synthetic_error_token_;
};

} // namespace amber::parser
