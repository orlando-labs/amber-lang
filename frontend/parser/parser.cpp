#include "frontend/parser/parser.h"
#include "frontend/lexer/lexer.h"
#include "frontend/pattern/pattern.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace amber::parser {
namespace {

constexpr const char *kParseErrorCode = "P0001";

ast::Expr &keyword_arg_field(ast::Expr &expr, const lexer::Token &name) {
  expr.string_field("name", name.lexeme);
  return expr;
}

const ast::NodeField *find_node_field(const ast::Expr &expr,
                                      const std::string &name) {
  for (const ast::NodeField &field : expr.node_fields) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

ast::ListField *find_list_field(ast::Expr &expr, const std::string &name) {
  for (ast::ListField &field : expr.list_fields) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

const ast::ListField *find_list_field(const ast::Expr &expr,
                                      const std::string &name) {
  for (const ast::ListField &field : expr.list_fields) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

const std::string *find_string_field(const ast::Expr &expr,
                                     const std::string &name) {
  for (const ast::StringField &field : expr.string_fields) {
    if (field.name == name) {
      return &field.value;
    }
  }
  return nullptr;
}

std::string string_value(const ast::Expr &expr, const std::string &name) {
  const std::string *value = find_string_field(expr, name);
  return value == nullptr ? "" : *value;
}

bool bool_value(const ast::Expr &expr, const std::string &name) {
  for (const ast::BoolField &field : expr.bool_fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  return false;
}

bool param_nodes_match(const ast::Expr &left, const ast::Expr &right) {
  if (left.kind != right.kind) {
    return false;
  }
  if (string_value(left, "param_kind") != string_value(right, "param_kind") ||
      string_value(left, "external_name") !=
          string_value(right, "external_name") ||
      string_value(left, "local_name") != string_value(right, "local_name") ||
      string_value(left, "auto_assign_kind") !=
          string_value(right, "auto_assign_kind") ||
      string_value(left, "type_expr") != string_value(right, "type_expr")) {
    return false;
  }
  const ast::NodeField *left_default = find_node_field(left, "default_expr");
  const ast::NodeField *right_default = find_node_field(right, "default_expr");
  if ((left_default == nullptr) != (right_default == nullptr)) {
    return false;
  }
  if (left_default != nullptr && right_default != nullptr &&
      left_default->value != nullptr && right_default->value != nullptr &&
      left_default->value->kind != right_default->value->kind) {
    return false;
  }
  return true;
}

bool signatures_match(const ast::Expr &left, const ast::Expr &right) {
  if (string_value(left, "return_type_expr") !=
      string_value(right, "return_type_expr")) {
    return false;
  }
  if (bool_value(left, "has_effect_row") !=
          bool_value(right, "has_effect_row") ||
      string_value(left, "effect_row_expr") !=
          string_value(right, "effect_row_expr")) {
    return false;
  }
  const ast::ListField *left_params = find_list_field(left, "params");
  const ast::ListField *right_params = find_list_field(right, "params");
  const std::size_t left_count =
      left_params == nullptr ? 0 : left_params->values.size();
  const std::size_t right_count =
      right_params == nullptr ? 0 : right_params->values.size();
  if (left_count != right_count) {
    return false;
  }
  for (std::size_t i = 0; i < left_count; ++i) {
    if (!param_nodes_match(*left_params->values[i], *right_params->values[i])) {
      return false;
    }
  }
  return true;
}

bool clause_defs_mergeable(const ast::Expr &left, const ast::Expr &right) {
  if (left.kind != "AstClauseDef" || right.kind != "AstClauseDef") {
    return false;
  }
  if (string_value(left, "name") != string_value(right, "name")) {
    return false;
  }
  const ast::NodeField *left_signature =
      find_node_field(left, "base_signature");
  const ast::NodeField *right_signature =
      find_node_field(right, "base_signature");
  if (left_signature == nullptr || right_signature == nullptr ||
      left_signature->value == nullptr || right_signature->value == nullptr) {
    return false;
  }
  if (!signatures_match(*left_signature->value, *right_signature->value)) {
    return false;
  }
  const ast::ListField *left_else = find_list_field(left, "else_body");
  return left_else == nullptr || left_else->values.empty();
}

bool signatures_compatible_for_fallback_clause(const ast::Expr &left,
                                               const ast::Expr &right) {
  if (string_value(left, "return_type_expr") !=
      string_value(right, "return_type_expr")) {
    return false;
  }
  if (bool_value(left, "has_effect_row") !=
          bool_value(right, "has_effect_row") ||
      string_value(left, "effect_row_expr") !=
          string_value(right, "effect_row_expr")) {
    return false;
  }
  const ast::ListField *left_params = find_list_field(left, "params");
  const ast::ListField *right_params = find_list_field(right, "params");
  const std::size_t left_count =
      left_params == nullptr ? 0 : left_params->values.size();
  const std::size_t right_count =
      right_params == nullptr ? 0 : right_params->values.size();
  if (left_count != right_count) {
    return false;
  }
  for (std::size_t i = 0; i < left_count; ++i) {
    if (string_value(*left_params->values[i], "param_kind") !=
        string_value(*right_params->values[i], "param_kind")) {
      return false;
    }
  }
  return true;
}

bool def_stmt_clause_defs_mergeable(const ast::Expr &left,
                                    const ast::Expr &right) {
  if (left.kind != "AstDefStmt" || right.kind != "AstClauseDef") {
    return false;
  }
  if (string_value(left, "name") != string_value(right, "name")) {
    return false;
  }
  const ast::NodeField *left_signature = find_node_field(left, "signature");
  const ast::NodeField *right_signature =
      find_node_field(right, "base_signature");
  if (left_signature == nullptr || right_signature == nullptr ||
      left_signature->value == nullptr || right_signature->value == nullptr) {
    return false;
  }
  const ast::ListField *right_else = find_list_field(right, "else_body");
  return (right_else == nullptr || right_else->values.empty()) &&
         signatures_compatible_for_fallback_clause(*left_signature->value,
                                                   *right_signature->value);
}

bool is_identifier_like_token(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::Identifier ||
         kind == lexer::TokenKind::KeywordAttr ||
         kind == lexer::TokenKind::KeywordProp ||
         kind == lexer::TokenKind::KeywordClassProp;
}

bool is_contextual_token(const lexer::Token &token, const char *text) {
  return token.kind == lexer::TokenKind::Identifier && token.lexeme == text;
}

bool starts_property_arm_label(const lexer::Token &label,
                               const lexer::Token &after_label) {
  if (is_contextual_token(label, "get")) {
    return after_label.kind == lexer::TokenKind::Colon ||
           after_label.kind == lexer::TokenKind::LParen;
  }
  if (is_contextual_token(label, "set")) {
    return after_label.kind == lexer::TokenKind::LParen;
  }
  return false;
}

std::unique_ptr<ast::Expr> take_node_field(ast::Expr &expr,
                                           const std::string &name) {
  for (ast::NodeField &field : expr.node_fields) {
    if (field.name == name) {
      return std::move(field.value);
    }
  }
  return {};
}

std::vector<std::unique_ptr<ast::Expr>>
take_list_field(ast::Expr &expr, const std::string &name) {
  for (ast::ListField &field : expr.list_fields) {
    if (field.name == name) {
      return std::move(field.values);
    }
  }
  return {};
}

void merge_clause_def_into(ast::Expr &left, std::unique_ptr<ast::Expr> right) {
  left.span = ast::join_spans(left.span, right->span);
  ast::ListField *left_clauses = find_list_field(left, "clauses");
  ast::ListField *left_else = find_list_field(left, "else_body");
  ast::ListField *right_clauses = find_list_field(*right, "clauses");
  ast::ListField *right_else = find_list_field(*right, "else_body");
  if (left_clauses != nullptr && right_clauses != nullptr) {
    for (std::unique_ptr<ast::Expr> &clause : right_clauses->values) {
      left_clauses->values.push_back(std::move(clause));
    }
  }
  if (left_else != nullptr && right_else != nullptr &&
      left_else->values.empty()) {
    for (std::unique_ptr<ast::Expr> &item : right_else->values) {
      left_else->values.push_back(std::move(item));
    }
  }
}

std::unique_ptr<ast::Expr>
merge_def_stmt_with_clause_def(std::unique_ptr<ast::Expr> def_stmt,
                               std::unique_ptr<ast::Expr> clause_def) {
  auto merged = ast::make_expr(
      "AstClauseDef", ast::join_spans(def_stmt->span, clause_def->span));
  merged->string_field("name", string_value(*def_stmt, "name"));
  merged->node_field("base_signature", take_node_field(*def_stmt, "signature"));
  merged->list_field("clauses", take_list_field(*clause_def, "clauses"));
  merged->list_field("else_body", take_list_field(*def_stmt, "body"));
  return merged;
}

void append_item_or_merge_clause_def(
    std::vector<std::unique_ptr<ast::Expr>> *items,
    std::unique_ptr<ast::Expr> item) {
  if (item == nullptr) {
    return;
  }
  if (!items->empty() && clause_defs_mergeable(*items->back(), *item)) {
    merge_clause_def_into(*items->back(), std::move(item));
    return;
  }
  if (!items->empty() &&
      def_stmt_clause_defs_mergeable(*items->back(), *item)) {
    items->back() = merge_def_stmt_with_clause_def(std::move(items->back()),
                                                   std::move(item));
    return;
  }
  items->push_back(std::move(item));
}

bool no_space_before_pattern_token(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::Comma || kind == lexer::TokenKind::Colon ||
         kind == lexer::TokenKind::Dot || kind == lexer::TokenKind::LParen ||
         kind == lexer::TokenKind::DotDot ||
         kind == lexer::TokenKind::LBracket ||
         kind == lexer::TokenKind::LBrace ||
         kind == lexer::TokenKind::Question ||
         kind == lexer::TokenKind::RParen ||
         kind == lexer::TokenKind::RBracket || kind == lexer::TokenKind::RBrace;
}

bool no_space_after_pattern_token(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::Colon || kind == lexer::TokenKind::Dot ||
         kind == lexer::TokenKind::DotDot || kind == lexer::TokenKind::Caret ||
         kind == lexer::TokenKind::Star || kind == lexer::TokenKind::LParen ||
         kind == lexer::TokenKind::LBracket || kind == lexer::TokenKind::LBrace;
}

void append_pattern_token(std::string *text, const lexer::Token &token,
                          const lexer::Token *previous) {
  if (!text->empty() && previous != nullptr &&
      !no_space_after_pattern_token(previous->kind) &&
      !no_space_before_pattern_token(token.kind)) {
    *text += " ";
  }
  *text += token.lexeme;
}

std::string pattern_text_from_tokens(const std::vector<lexer::Token> &tokens,
                                     std::size_t begin, std::size_t end) {
  std::string text;
  const lexer::Token *previous = nullptr;
  for (std::size_t i = begin; i < end; ++i) {
    append_pattern_token(&text, tokens[i], previous);
    previous = &tokens[i];
  }
  return text;
}

std::vector<lexer::Token>
expression_slice_tokens(const std::vector<lexer::Token> &tokens,
                        std::size_t begin, std::size_t end) {
  std::vector<lexer::Token> slice;
  for (std::size_t i = begin; i < end; ++i) {
    slice.push_back(tokens[i]);
  }
  lexer::Token eof;
  eof.kind = lexer::TokenKind::Eof;
  if (end > begin) {
    eof.span = tokens[end - 1].span;
  }
  slice.push_back(eof);
  return slice;
}

std::unique_ptr<ast::Expr> make_synthetic_signature(const lexer::Span &span,
                                                    std::size_t arity) {
  auto signature = ast::make_expr("AstSignature", span);
  std::vector<std::unique_ptr<ast::Expr>> params;
  params.reserve(arity);
  for (std::size_t i = 0; i < arity; ++i) {
    auto param = ast::make_expr("AstParam", span);
    const std::string name = "__arg" + std::to_string(i);
    param->string_field("param_kind", "positional");
    param->string_field("external_name", name);
    param->string_field("local_name", name);
    param->string_field("auto_assign_kind", "none");
    param->string_field("type_expr", "");
    params.push_back(std::move(param));
  }
  signature->list_field("params", std::move(params));
  return signature;
}

std::unique_ptr<ast::Expr> make_attr_setter_signature(const lexer::Span &span) {
  auto signature = ast::make_expr("AstSignature", span);
  std::vector<std::unique_ptr<ast::Expr>> params;
  auto param = ast::make_expr("AstParam", span);
  param->string_field("param_kind", "positional");
  param->string_field("external_name", "value");
  param->string_field("local_name", "value");
  param->string_field("auto_assign_kind", "none");
  param->string_field("type_expr", "");
  params.push_back(std::move(param));
  signature->list_field("params", std::move(params));
  return signature;
}

std::unique_ptr<ast::Expr> make_attr_ivar(const std::string &field_name,
                                          const lexer::Span &span) {
  auto expr = ast::make_expr("AstIvar", span);
  expr->string_field("name", field_name);
  return expr;
}

std::unique_ptr<ast::Expr>
make_attr_expr_stmt(std::unique_ptr<ast::Expr> expr) {
  lexer::Span span = expr->span;
  auto stmt = ast::make_expr("AstExprStmt", span);
  stmt->node_field("expr", std::move(expr));
  return stmt;
}

std::vector<std::unique_ptr<ast::Expr>>
make_attr_getter_body(const std::string &field_name,
                      const lexer::Span &field_span) {
  std::vector<std::unique_ptr<ast::Expr>> body;
  body.push_back(make_attr_expr_stmt(make_attr_ivar(field_name, field_span)));
  return body;
}

std::vector<std::unique_ptr<ast::Expr>>
make_attr_setter_body(const std::string &field_name,
                      const lexer::Span &field_span) {
  std::vector<std::unique_ptr<ast::Expr>> body;
  auto assign = ast::make_expr("AstAssign", field_span);
  assign->string_field("op", "=");
  assign->node_field("left", make_attr_ivar(field_name, field_span));
  auto value = ast::make_expr("AstName", field_span);
  value->string_field("name", "value");
  assign->node_field("right", std::move(value));
  body.push_back(make_attr_expr_stmt(std::move(assign)));
  return body;
}

bool attr_storage_stop_token(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::Newline ||
         kind == lexer::TokenKind::Dedent || kind == lexer::TokenKind::Eof;
}

std::unique_ptr<ast::Expr>
ensure_postfix_chain(std::unique_ptr<ast::Expr> base) {
  if (base->kind == "AstPostfixChain") {
    return base;
  }
  lexer::Span span = base->span;
  auto chain = ast::make_expr("AstPostfixChain", span);
  chain->node_field("base", std::move(base));
  chain->list_field("tails", {});
  return chain;
}

void append_postfix_tail(ast::Expr &chain, std::unique_ptr<ast::Expr> tail) {
  chain.span = ast::join_spans(chain.span, tail->span);
  ast::ListField *tails = find_list_field(chain, "tails");
  if (tails != nullptr) {
    tails->values.push_back(std::move(tail));
  }
}

const ast::Expr *last_postfix_tail(const ast::Expr &expr) {
  if (expr.kind != "AstPostfixChain") {
    return nullptr;
  }
  const ast::ListField *tails = find_list_field(expr, "tails");
  if (tails == nullptr || tails->values.empty()) {
    return nullptr;
  }
  return tails->values.back().get();
}

lexer::Position offset_position_in_token(const lexer::Token &token,
                                         std::size_t lexeme_offset) {
  lexer::Position position = token.span.start;
  position.offset += lexeme_offset;
  position.col += lexeme_offset;
  return position;
}

lexer::Span span_for_token_slice(const lexer::Token &token,
                                 std::size_t begin_offset,
                                 std::size_t end_offset) {
  return lexer::Span{token.span.file,
                     offset_position_in_token(token, begin_offset),
                     offset_position_in_token(token, end_offset)};
}

std::string utf8_from_codepoint(std::uint32_t codepoint) {
  std::string out;
  if (codepoint <= 0x7FU) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFU) {
    out.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
    out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint <= 0xFFFFU) {
    out.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
  return out;
}

bool is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

std::uint32_t hex_digit_value(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<std::uint32_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<std::uint32_t>(c - 'a' + 10);
  }
  return static_cast<std::uint32_t>(c - 'A' + 10);
}

struct StringEscapePart {
  std::string kind;
  std::string source;
  std::string value;
  std::size_t end_offset = 0;
};

StringEscapePart parse_string_escape_part(const std::string &lexeme,
                                          std::size_t offset,
                                          std::size_t content_end) {
  StringEscapePart part;
  part.end_offset = std::min(offset + 1U, content_end);
  if (offset + 1U >= content_end) {
    part.kind = "invalid";
    part.source = "\\";
    return part;
  }

  const char escaped = lexeme[offset + 1U];
  part.end_offset = offset + 2U;
  switch (escaped) {
  case 'n':
    part.kind = "newline";
    part.value = "\n";
    break;
  case 'r':
    part.kind = "carriage_return";
    part.value = "\r";
    break;
  case 't':
    part.kind = "tab";
    part.value = "\t";
    break;
  case '"':
    part.kind = "quote";
    part.value = "\"";
    break;
  case '\'':
    part.kind = "single_quote";
    part.value = "'";
    break;
  case '\\':
    part.kind = "backslash";
    part.value = "\\";
    break;
  case '#':
    part.kind = "hash";
    part.value = "#";
    break;
  case 'u': {
    std::size_t cursor = offset + 2U;
    if (cursor < content_end && lexeme[cursor] == '{') {
      ++cursor;
      std::uint32_t codepoint = 0;
      while (cursor < content_end && lexeme[cursor] != '}') {
        if (is_hex_digit(lexeme[cursor])) {
          codepoint = (codepoint << 4U) | hex_digit_value(lexeme[cursor]);
        }
        ++cursor;
      }
      if (cursor < content_end && lexeme[cursor] == '}') {
        ++cursor;
        part.end_offset = cursor;
        part.kind = "unicode";
        part.value = utf8_from_codepoint(codepoint);
        break;
      }
    }
    part.kind = "invalid";
    part.value = "u";
    break;
  }
  default:
    part.kind = "invalid";
    part.value = std::string(1, escaped);
    break;
  }
  part.source = lexeme.substr(offset, part.end_offset - offset);
  return part;
}

bool string_is_blank(const std::string &value) {
  for (char c : value) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      return false;
    }
  }
  return true;
}

void shift_position_from_interpolation(lexer::Position *position,
                                       const lexer::Position &base) {
  position->offset += base.offset;
  position->line += base.line - 1;
  if (position->line == base.line) {
    position->col += base.col - 1;
  }
}

void shift_token_spans_from_interpolation(std::vector<lexer::Token> *tokens,
                                          const lexer::Position &base) {
  for (lexer::Token &token : *tokens) {
    shift_position_from_interpolation(&token.span.start, base);
    shift_position_from_interpolation(&token.span.end, base);
  }
}

bool is_operator_method_name(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::Plus ||
         kind == lexer::TokenKind::Minus ||
         kind == lexer::TokenKind::Star ||
         kind == lexer::TokenKind::Slash ||
         kind == lexer::TokenKind::SlashSlash ||
         kind == lexer::TokenKind::Percent ||
         kind == lexer::TokenKind::EqualEqual ||
         kind == lexer::TokenKind::EqualEqualEqual ||
         kind == lexer::TokenKind::BangEqual ||
         kind == lexer::TokenKind::Less ||
         kind == lexer::TokenKind::LessEqual ||
         kind == lexer::TokenKind::LessEqualGreater ||
         kind == lexer::TokenKind::Greater ||
         kind == lexer::TokenKind::GreaterEqual;
}

bool is_chain_comparison_op(const std::string &op) {
  return op == "<" || op == "<=" || op == ">" || op == ">=";
}

int chain_comparison_direction(const std::string &op) {
  if (op == "<" || op == "<=") {
    return -1;
  }
  if (op == ">" || op == ">=") {
    return 1;
  }
  return 0;
}

} // namespace

Parser::Parser(const std::vector<lexer::Token> &tokens) : tokens_(tokens) {
  if (!tokens.empty()) {
    synthetic_error_token_ = tokens.back();
  }
}

ParseResult Parser::parse_expression_unit() {
  while (match(lexer::TokenKind::Newline)) {
  }
  std::unique_ptr<ast::Expr> expr = parse_expression(1, StopMode::Normal);
  while (match(lexer::TokenKind::Newline)) {
  }
  if (!check(lexer::TokenKind::Eof)) {
    error(current(), "unexpected token after expression");
  }
  return ParseResult{std::move(expr), std::move(diagnostics_)};
}

ParseModuleResult Parser::parse_module_unit() {
  std::vector<std::unique_ptr<ast::Expr>> items;
  std::string module_name;

  while (match(lexer::TokenKind::Newline)) {
  }
  while (!at_end()) {
    const std::size_t before = current_;
    std::unique_ptr<ast::Expr> item = parse_statement(BodyContext::Module);
    if (item) {
      if (item->kind == "AstPackageDecl") {
        for (const ast::StringField &field : item->string_fields) {
          if (field.name == "module_path") {
            module_name = field.value;
            break;
          }
        }
      }
      append_item_or_merge_clause_def(&items, std::move(item));
    }
    while (match(lexer::TokenKind::Newline)) {
    }
    if (current_ == before && !at_end()) {
      error(current(), check(lexer::TokenKind::Dedent)
                           ? "unexpected dedent at module level"
                           : "unexpected token at module level");
      advance();
      while (match(lexer::TokenKind::Newline)) {
      }
    }
  }

  return ParseModuleResult{std::move(items), module_name,
                           std::move(diagnostics_)};
}

const lexer::Token &Parser::current() const {
  if (current_ < tokens_.size()) {
    return tokens_[current_];
  }
  return synthetic_error_token_;
}

const lexer::Token &Parser::previous() const {
  if (current_ == 0 || tokens_.empty()) {
    return synthetic_error_token_;
  }
  return tokens_[current_ - 1];
}

const lexer::Token &Parser::peek(std::size_t distance) const {
  const std::size_t target = current_ + distance;
  if (target < tokens_.size()) {
    return tokens_[target];
  }
  return synthetic_error_token_;
}

bool Parser::at_end() const { return check(lexer::TokenKind::Eof); }

bool Parser::check(lexer::TokenKind kind) const {
  return current().kind == kind;
}

bool Parser::match(lexer::TokenKind kind) {
  if (!check(kind)) {
    return false;
  }
  advance();
  return true;
}

const lexer::Token &Parser::advance() {
  if (!at_end()) {
    ++current_;
  }
  return previous();
}

const lexer::Token &Parser::consume(lexer::TokenKind kind,
                                    const std::string &message) {
  if (check(kind)) {
    return advance();
  }
  error(current(), message);
  return current();
}

std::unique_ptr<ast::Expr> Parser::parse_statement(BodyContext context) {
  while (match(lexer::TokenKind::Newline)) {
  }
  if (at_end() || check(lexer::TokenKind::Dedent)) {
    return nullptr;
  }

  switch (current().kind) {
  case lexer::TokenKind::KeywordPackage:
    return parse_package_decl();
  case lexer::TokenKind::KeywordImport:
    return parse_import_decl();
  case lexer::TokenKind::KeywordFrom:
    return parse_from_import_decl();
  case lexer::TokenKind::KeywordExport:
    return parse_export_stmt();
  case lexer::TokenKind::KeywordDef:
    return parse_def_stmt(false);
  case lexer::TokenKind::KeywordProp:
    if (looks_like_property_declaration()) {
      if (context == BodyContext::Def) {
        error_code(current(), "AMB_PROP_INVALID_CONTEXT",
                   "property declarations are not allowed in function or "
                   "block bodies");
      }
      return parse_prop_def(false);
    }
    break;
  case lexer::TokenKind::KeywordAttr:
    if (looks_like_attr_declaration()) {
      if (context != BodyContext::Class && context != BodyContext::Mixin) {
        error_code(current(), "E_ATTR_INVALID_CONTEXT",
                   "attr declarations are only allowed in class or mixin "
                   "bodies");
      }
      return parse_attr_def();
    }
    break;
  case lexer::TokenKind::KeywordClassMethod: {
    const lexer::Token start = advance();
    consume(lexer::TokenKind::KeywordDef, "expected 'def' after class_method");
    return parse_def_stmt(true, &start);
  }
  case lexer::TokenKind::KeywordClassProp:
    if (looks_like_property_declaration()) {
      if (context != BodyContext::Class) {
        error_code(current(), "AMB_PROP_CLASS_PROP_OUTSIDE_CLASS",
                   "class_prop is only allowed in class body");
      }
      return parse_prop_def(true);
    }
    break;
  case lexer::TokenKind::KeywordClass:
    return parse_class_def();
  case lexer::TokenKind::KeywordMixin:
    return parse_mixin_def();
  case lexer::TokenKind::KeywordInclude:
    if (context != BodyContext::Class && context != BodyContext::Mixin) {
      error_code(current(), "E3001",
                 "include is only allowed in class or mixin body");
    }
    return parse_include_stmt(false);
  case lexer::TokenKind::KeywordExtend:
    if (context != BodyContext::Class) {
      error_code(current(), "E3007", "extend is only allowed in class body");
    }
    return parse_include_stmt(true);
  case lexer::TokenKind::KeywordPass:
    return parse_pass_like_stmt("AstPassStmt");
  case lexer::TokenKind::KeywordNoop:
    return parse_pass_like_stmt("AstNoopStmt");
  default:
    break;
  }

  if (std::unique_ptr<ast::Expr> pattern_assign =
          try_parse_pattern_assignment()) {
    return pattern_assign;
  }

  std::unique_ptr<ast::Expr> expr = parse_expression(1, StopMode::Normal);
  auto stmt = ast::make_expr("AstExprStmt", expr->span);
  stmt->node_field("expr", std::move(expr));
  return stmt;
}

std::unique_ptr<ast::Expr> Parser::parse_package_decl() {
  const lexer::Token start = advance();
  const std::string module_path = parse_module_path();
  auto node = ast::make_expr("AstPackageDecl",
                             ast::join_spans(start.span, previous().span));
  node->string_field("module_path", module_path);
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_import_decl() {
  const lexer::Token start = advance();
  const std::string module_path = parse_module_path();
  std::string alias;
  if (match_contextual("as")) {
    alias = consume_identifier_text("expected import alias after 'as'");
  }

  auto node = ast::make_expr("AstImportStmt",
                             ast::join_spans(start.span, previous().span));
  node->string_field("import_kind", "module");
  node->string_field("module_path", module_path);
  node->string_field("alias", alias);
  node->list_field("names", {});
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_from_import_decl() {
  const lexer::Token start = advance();
  const std::string module_path = parse_module_path();
  consume(lexer::TokenKind::KeywordImport, "expected 'import' in from-import");

  std::vector<std::unique_ptr<ast::Expr>> names;
  while (!at_end() && !check(lexer::TokenKind::Newline)) {
    if (check(lexer::TokenKind::Star)) {
      error_code(current(), "E2003", "from ... import * is forbidden in v1");
      advance();
      break;
    }
    const lexer::Token source = current();
    const std::string source_name =
        consume_identifier_text("expected imported name");
    std::string local_name = source_name;
    if (match_contextual("as")) {
      local_name = consume_identifier_text("expected import alias after 'as'");
    }
    auto name = ast::make_expr("AstImportName",
                               ast::join_spans(source.span, previous().span));
    name->string_field("source_name", source_name);
    name->string_field("local_name", local_name);
    names.push_back(std::move(name));
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }

  auto node = ast::make_expr("AstImportStmt",
                             ast::join_spans(start.span, previous().span));
  node->string_field("import_kind", "from");
  node->string_field("module_path", module_path);
  node->string_field("alias", "");
  node->list_field("names", std::move(names));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_export_stmt() {
  const lexer::Token start = advance();
  std::vector<std::unique_ptr<ast::Expr>> items;
  while (!at_end() && !check(lexer::TokenKind::Newline)) {
    const lexer::Token local = current();
    const std::string local_name =
        consume_identifier_text("expected exported name");
    std::string public_name = local_name;
    if (match_contextual("as")) {
      public_name = consume_identifier_text("expected public name after 'as'");
    }
    auto item = ast::make_expr("AstExportItem",
                               ast::join_spans(local.span, previous().span));
    item->string_field("local_name", local_name);
    item->string_field("public_name", public_name);
    items.push_back(std::move(item));
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  auto node = ast::make_expr("AstExportStmt",
                             ast::join_spans(start.span, previous().span));
  node->list_field("items", std::move(items));
  return node;
}

std::unique_ptr<ast::Expr>
Parser::parse_def_stmt(bool class_method, const lexer::Token *start_override) {
  const lexer::Token start =
      start_override != nullptr ? *start_override : advance();
  const std::string name_text =
      consume_method_name_text("expected function name");
  std::unique_ptr<ast::Expr> signature;
  if (!class_method && is_simple_many_def_header()) {
    lexer::Span signature_span{};
    std::vector<std::string> patterns =
        parse_many_def_patterns(&signature_span);
    signature = make_synthetic_signature(signature_span, patterns.size());
    std::unique_ptr<ast::Expr> guard;
    if (match(lexer::TokenKind::KeywordIf)) {
      guard = parse_expression(1, StopMode::Normal);
    }
    consume(lexer::TokenKind::Colon, "expected ':' after function signature");
    std::vector<std::unique_ptr<ast::Expr>> body =
        parse_control_body(BodyContext::Def);
    const lexer::Span end_span =
        body.empty() ? previous().span : body.back()->span;

    auto clause =
        ast::make_expr("AstClause", ast::join_spans(start.span, end_span));
    std::string pattern_text;
    if (patterns.size() == 1) {
      pattern_text = patterns[0];
    } else {
      pattern_text = "(";
      for (std::size_t i = 0; i < patterns.size(); ++i) {
        if (i != 0) {
          pattern_text += ", ";
        }
        pattern_text += patterns[i];
      }
      pattern_text += ")";
    }
    clause->string_field("pattern", pattern_text);
    if (guard) {
      clause->node_field("guard_expr", std::move(guard));
    }
    clause->list_field("body", std::move(body));

    std::vector<std::unique_ptr<ast::Expr>> clauses;
    clauses.push_back(std::move(clause));
    auto node =
        ast::make_expr("AstClauseDef", ast::join_spans(start.span, end_span));
    node->string_field("name", name_text);
    node->node_field("base_signature", std::move(signature));
    node->list_field("clauses", std::move(clauses));
    node->list_field("else_body", {});
    return node;
  }

  signature = parse_signature();
  if (match(lexer::TokenKind::Arrow)) {
    const lexer::Token arrow = previous();
    std::string return_type = parse_type_term_text_until_return_boundary();
    if (return_type.empty()) {
      error(arrow, "expected return type after '->'");
    } else {
      signature->string_field("return_type_expr", return_type);
      signature->span = ast::join_spans(signature->span, previous().span);
    }
  }
  if (check(lexer::TokenKind::Bang)) {
    const std::string effect_row = parse_effect_row_text();
    signature->bool_field("has_effect_row", true);
    signature->string_field("effect_row_expr", effect_row);
    signature->span = ast::join_spans(signature->span, previous().span);
  }
  consume(lexer::TokenKind::Colon, "expected ':' after function signature");
  if (!class_method && starts_clause_body()) {
    ClauseBody clause_body = parse_clause_body();
    lexer::Span end_span = signature->span;
    if (!clause_body.else_body.empty()) {
      end_span = clause_body.else_body.back()->span;
    } else if (!clause_body.clauses.empty()) {
      end_span = clause_body.clauses.back()->span;
    }
    auto node =
        ast::make_expr("AstClauseDef", ast::join_spans(start.span, end_span));
    node->string_field("name", name_text);
    node->node_field("base_signature", std::move(signature));
    node->list_field("clauses", std::move(clause_body.clauses));
    node->list_field("else_body", std::move(clause_body.else_body));
    return node;
  }

  std::vector<std::unique_ptr<ast::Expr>> body = parse_body(BodyContext::Def);
  const lexer::Span end_span =
      body.empty() ? previous().span : body.back()->span;

  auto node = ast::make_expr(class_method ? "AstClassMethodDef" : "AstDefStmt",
                             ast::join_spans(start.span, end_span));
  node->string_field("name", name_text);
  node->node_field("signature", std::move(signature));
  node->list_field("body", std::move(body));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_prop_def(bool class_property) {
  const lexer::Token start = advance();
  const lexer::Token name =
      consume_identifier_like("expected property name");

  if (match(lexer::TokenKind::LParen)) {
    const lexer::Token open = previous();
    error_code(open, "AMB_PROP_PARAM_LIST_FORBIDDEN",
               "property declarations cannot have parameters");
    int depth = 1;
    while (!at_end() && depth > 0) {
      if (check(lexer::TokenKind::LParen)) {
        ++depth;
      } else if (check(lexer::TokenKind::RParen)) {
        --depth;
      } else if (check(lexer::TokenKind::Newline) ||
                 check(lexer::TokenKind::Colon)) {
        break;
      }
      advance();
    }
  }

  consume(lexer::TokenKind::Colon, "expected ':' after property name");
  PropertySuite suite = parse_property_suite(name.span);

  auto node = ast::make_expr(class_property ? "AstClassPropDef" : "AstPropDef",
                             ast::join_spans(start.span, suite.end_span));
  node->string_field("name", name.lexeme);
  node->bool_field("grouped_descriptor", suite.grouped);
  node->bool_field("has_getter", suite.has_getter);
  node->bool_field("has_setter", suite.has_setter);
  node->list_field("getter_body", std::move(suite.getter_body));
  if (suite.setter_signature) {
    node->node_field("setter_signature", std::move(suite.setter_signature));
  }
  node->list_field("setter_body", std::move(suite.setter_body));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_attr_def() {
  const lexer::Token start = advance();

  std::string mode = "get_only";
  bool has_getter = true;
  bool has_setter = false;
  if (check(lexer::TokenKind::Identifier) &&
      (current().lexeme == "var" || current().lexeme == "set") &&
      (is_identifier_like_token(peek().kind) ||
       peek().kind == lexer::TokenKind::KeywordFrom)) {
    mode = current().lexeme == "var" ? "get_set" : "set_only";
    has_getter = current().lexeme == "var";
    has_setter = true;
    advance();
  }

  bool has_name = false;
  lexer::Token name = current();
  if (is_identifier_like_token(current().kind)) {
    name = advance();
    has_name = true;
  } else {
    error_code(current(), "E_ATTR_EXPECTED_NAME",
               "attribute declaration requires a member name");
    if (!check(lexer::TokenKind::KeywordFrom) &&
        !attr_storage_stop_token(current().kind)) {
      advance();
    }
  }

  const std::string attr_name = has_name ? name.lexeme : "";
  std::string storage_field = attr_name;
  lexer::Span storage_span = has_name ? name.span : start.span;
  bool explicit_storage = false;

  if (match(lexer::TokenKind::KeywordFrom)) {
    explicit_storage = true;
    const lexer::Token from = previous();
    if (check(lexer::TokenKind::At)) {
      const lexer::Token at = advance();
      if (check(lexer::TokenKind::Identifier)) {
        const lexer::Token field = advance();
        storage_field = field.lexeme;
        storage_span = ast::join_spans(at.span, field.span);
      } else {
        error_code(current(), "E_ATTR_EXPECTED_STORAGE_FIELD",
                   "expected instance field after `from`");
        storage_span = from.span;
      }
    } else if (attr_storage_stop_token(current().kind)) {
      error_code(current(), "E_ATTR_EXPECTED_STORAGE_FIELD",
                 "expected instance field after `from`");
      storage_span = from.span;
    } else {
      error_code(current(), "E_ATTR_INVALID_STORAGE",
                 "attribute storage must be an instance field token");
      storage_span = current().span;
      while (!attr_storage_stop_token(current().kind)) {
        advance();
      }
    }
  }

  const lexer::Span end_span =
      previous().span.end.offset >= start.span.start.offset ? previous().span
                                                            : start.span;
  auto node =
      ast::make_expr("AstAttrDef", ast::join_spans(start.span, end_span));
  node->string_field("name", attr_name);
  node->string_field("attr_mode", mode);
  node->string_field("storage_field", storage_field);
  node->bool_field("explicit_storage", explicit_storage);
  node->bool_field("grouped_descriptor", false);
  node->bool_field("has_getter", has_getter);
  node->bool_field("has_setter", has_setter);
  node->list_field("getter_body",
                   has_getter
                       ? make_attr_getter_body(storage_field, storage_span)
                       : std::vector<std::unique_ptr<ast::Expr>>{});
  if (has_setter) {
    node->node_field("setter_signature",
                     make_attr_setter_signature(storage_span));
    node->list_field("setter_body",
                     make_attr_setter_body(storage_field, storage_span));
  } else {
    node->list_field("setter_body", {});
  }
  return node;
}

Parser::PropertySuite
Parser::parse_property_suite(const lexer::Span &fallback_span) {
  if (property_suite_starts_grouped()) {
    return parse_grouped_property_suite(fallback_span);
  }

  PropertySuite suite;
  suite.has_getter = true;
  suite.end_span = fallback_span;
  suite.getter_body = parse_body(BodyContext::Def);
  if (!suite.getter_body.empty()) {
    suite.end_span = suite.getter_body.back()->span;
  } else {
    suite.end_span = previous().span;
  }
  return suite;
}

Parser::PropertySuite
Parser::parse_grouped_property_suite(const lexer::Span &fallback_span) {
  PropertySuite suite;
  suite.grouped = true;
  suite.end_span = fallback_span;

  consume(lexer::TokenKind::Newline, "expected newline before property arms");
  consume(lexer::TokenKind::Indent, "expected indented property descriptor");
  while (!at_end() && !check(lexer::TokenKind::Dedent)) {
    while (match(lexer::TokenKind::Newline)) {
    }
    if (check(lexer::TokenKind::Dedent) || at_end()) {
      break;
    }
    if (starts_property_arm_label(current(), peek())) {
      parse_property_arm(&suite);
    } else {
      error_code(current(), "AMB_PROP_BAD_DECL",
                 "expected get: or set(value): property arm");
      std::unique_ptr<ast::Expr> skipped = parse_statement(BodyContext::Def);
      if (skipped != nullptr) {
        suite.end_span = skipped->span;
      }
    }
    while (match(lexer::TokenKind::Newline)) {
    }
  }
  consume(lexer::TokenKind::Dedent, "expected property descriptor dedent");
  if (!suite.has_getter && !suite.has_setter) {
    error_code(previous(), "AMB_PROP_EMPTY_DESCRIPTOR",
               "property descriptor must declare a getter or setter arm");
  }
  if (suite.end_span.start.offset == fallback_span.start.offset &&
      suite.end_span.end.offset == fallback_span.end.offset) {
    suite.end_span = previous().span;
  }
  return suite;
}

void Parser::parse_property_arm(PropertySuite *suite) {
  if (suite == nullptr) {
    return;
  }
  const lexer::Token label = advance();
  if (is_contextual_token(label, "get")) {
    const bool duplicate = suite->has_getter;
    if (duplicate) {
      error_code(label, "AMB_PROP_DUPLICATE_GETTER",
                 "property descriptor has more than one getter arm");
    }
    if (match(lexer::TokenKind::LParen)) {
      error_code(previous(), "AMB_PROP_GETTER_PARAM_LIST_FORBIDDEN",
                 "property getter cannot declare parameters");
      int depth = 1;
      while (!at_end() && depth > 0) {
        if (check(lexer::TokenKind::LParen)) {
          ++depth;
        } else if (check(lexer::TokenKind::RParen)) {
          --depth;
        } else if (check(lexer::TokenKind::Newline) ||
                   check(lexer::TokenKind::Colon)) {
          break;
        }
        advance();
      }
    }
    consume(lexer::TokenKind::Colon, "expected ':' after getter arm");
    std::vector<std::unique_ptr<ast::Expr>> body =
        parse_body(BodyContext::Def);
    if (!body.empty()) {
      suite->end_span = body.back()->span;
    } else {
      suite->end_span = previous().span;
    }
    if (!duplicate) {
      suite->has_getter = true;
      suite->getter_body = std::move(body);
    }
    return;
  }

  if (!is_contextual_token(label, "set")) {
    error_code(label, "AMB_PROP_BAD_DECL",
               "expected get: or set(value): property arm");
    return;
  }

  const bool duplicate = suite->has_setter;
  if (duplicate) {
    error_code(label, "AMB_PROP_DUPLICATE_SETTER",
               "property descriptor has more than one setter arm");
  }

  std::unique_ptr<ast::Expr> signature;
  if (check(lexer::TokenKind::LParen)) {
    signature = parse_signature();
  } else {
    error_code(current(), "AMB_PROP_SETTER_ARITY",
               "property setter must declare exactly one parameter");
    signature = ast::make_expr("AstSignature", label.span);
    signature->list_field("params", {});
  }

  const ast::ListField *params = find_list_field(*signature, "params");
  const std::size_t param_count =
      params == nullptr ? 0U : params->values.size();
  if (param_count != 1U) {
    const lexer::Token signature_token{lexer::TokenKind::Identifier, "",
                                       signature->span};
    error_code(signature_token, "AMB_PROP_SETTER_ARITY",
               "property setter must declare exactly one parameter");
  } else {
    const ast::Expr &param = *params->values.front();
    if (string_value(param, "param_kind") != "positional" ||
        string_value(param, "auto_assign_kind") != "none") {
      const lexer::Token param_token{lexer::TokenKind::Identifier, "",
                                     param.span};
      error_code(param_token, "AMB_PROP_SETTER_PARAM_KIND",
                 "property setter parameter must be one plain positional "
                 "identifier");
    }
    if (find_node_field(param, "default_expr") != nullptr) {
      const lexer::Token param_token{lexer::TokenKind::Identifier, "",
                                     param.span};
      error_code(param_token, "AMB_PROP_SETTER_DEFAULT",
                 "property setter parameter cannot have a default value");
    }
  }

  consume(lexer::TokenKind::Colon, "expected ':' after setter arm");
  std::vector<std::unique_ptr<ast::Expr>> body = parse_body(BodyContext::Def);
  if (!body.empty()) {
    suite->end_span = body.back()->span;
  } else {
    suite->end_span = previous().span;
  }
  if (!duplicate) {
    suite->has_setter = true;
    suite->setter_signature = std::move(signature);
    suite->setter_body = std::move(body);
  }
}

std::unique_ptr<ast::Expr> Parser::parse_class_def() {
  const lexer::Token start = advance();
  const lexer::Token name =
      consume(lexer::TokenKind::Identifier, "expected class name");
  std::string superclass;
  if (match(lexer::TokenKind::Less)) {
    superclass = parse_module_path();
  }
  consume(lexer::TokenKind::Colon, "expected ':' after class header");
  std::vector<std::unique_ptr<ast::Expr>> body = parse_body(BodyContext::Class);
  const lexer::Span end_span =
      body.empty() ? previous().span : body.back()->span;

  auto node =
      ast::make_expr("AstClassDef", ast::join_spans(start.span, end_span));
  node->string_field("name", name.lexeme);
  node->string_field("superclass", superclass);
  node->list_field("body", std::move(body));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_mixin_def() {
  const lexer::Token start = advance();
  const lexer::Token name =
      consume(lexer::TokenKind::Identifier, "expected mixin name");
  consume(lexer::TokenKind::Colon, "expected ':' after mixin header");
  std::vector<std::unique_ptr<ast::Expr>> body = parse_body(BodyContext::Mixin);
  const lexer::Span end_span =
      body.empty() ? previous().span : body.back()->span;

  auto node =
      ast::make_expr("AstMixinDef", ast::join_spans(start.span, end_span));
  node->string_field("name", name.lexeme);
  node->list_field("body", std::move(body));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_include_stmt(bool extend) {
  const lexer::Token start = advance();
  std::vector<std::unique_ptr<ast::Expr>> paths =
      parse_path_list(extend ? "AstExtendPath" : "AstIncludePath");
  auto node = ast::make_expr(extend ? "AstExtendStmt" : "AstIncludeStmt",
                             ast::join_spans(start.span, previous().span));
  node->list_field("paths", std::move(paths));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_pass_like_stmt(const char *kind) {
  const lexer::Token token = advance();
  return ast::make_expr(kind, token.span);
}

std::vector<std::unique_ptr<ast::Expr>>
Parser::parse_body(BodyContext context) {
  std::vector<std::unique_ptr<ast::Expr>> body;
  if (match(lexer::TokenKind::Newline)) {
    consume(lexer::TokenKind::Indent, "expected indented block");
    while (!at_end() && !check(lexer::TokenKind::Dedent)) {
      std::unique_ptr<ast::Expr> item = parse_statement(context);
      if (item) {
        append_item_or_merge_clause_def(&body, std::move(item));
      }
      while (match(lexer::TokenKind::Newline)) {
      }
    }
    consume(lexer::TokenKind::Dedent, "expected block dedent");
    return body;
  }

  std::unique_ptr<ast::Expr> item = parse_statement(context);
  if (item) {
    append_item_or_merge_clause_def(&body, std::move(item));
  }
  return body;
}

std::unique_ptr<ast::Expr> Parser::parse_signature() {
  const lexer::Token start =
      consume(lexer::TokenKind::LParen, "expected '(' before parameters");
  std::vector<std::unique_ptr<ast::Expr>> params;
  while (!check(lexer::TokenKind::RParen) && !at_end()) {
    params.push_back(parse_param());
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  const lexer::Token close =
      consume(lexer::TokenKind::RParen, "expected ')' after parameters");
  auto signature =
      ast::make_expr("AstSignature", ast::join_spans(start.span, close.span));
  signature->list_field("params", std::move(params));
  return signature;
}

std::unique_ptr<ast::Expr> Parser::parse_param() {
  const lexer::Token start = current();
  std::string auto_assign_kind = "none";
  if (match(lexer::TokenKind::At)) {
    auto_assign_kind = "@";
  } else if (match(lexer::TokenKind::AtAt)) {
    auto_assign_kind = "@@";
  }
  const lexer::Token name =
      consume(lexer::TokenKind::Identifier, "expected parameter name");
  std::string kind = "positional";
  std::string type_expr;
  std::unique_ptr<ast::Expr> default_expr;

  if (match_contextual("as")) {
    type_expr = parse_type_term_text_until_param_boundary();
  }

  if (match(lexer::TokenKind::Colon)) {
    kind = "keyword";
    if (!check(lexer::TokenKind::Comma) && !check(lexer::TokenKind::RParen)) {
      default_expr = parse_expression(1, StopMode::Normal);
    }
  } else if (match(lexer::TokenKind::Equal)) {
    default_expr = parse_expression(1, StopMode::Normal);
  }

  auto param =
      ast::make_expr("AstParam", ast::join_spans(start.span, previous().span));
  param->string_field("param_kind", kind);
  param->string_field("external_name", name.lexeme);
  param->string_field("local_name", name.lexeme);
  param->string_field("auto_assign_kind", auto_assign_kind);
  param->string_field("type_expr", type_expr);
  if (default_expr) {
    param->node_field("default_expr", std::move(default_expr));
  }
  return param;
}

std::string Parser::parse_type_term_text_until_param_boundary() {
  std::string text;
  const lexer::Token *previous_type_token = nullptr;
  int depth = 0;
  while (!at_end()) {
    const lexer::TokenKind kind = current().kind;
    if (depth == 0 &&
        (kind == lexer::TokenKind::Comma || kind == lexer::TokenKind::RParen ||
         kind == lexer::TokenKind::Equal || kind == lexer::TokenKind::Colon ||
         kind == lexer::TokenKind::Newline || kind == lexer::TokenKind::Eof)) {
      break;
    }
    if (kind == lexer::TokenKind::LParen ||
        kind == lexer::TokenKind::LBracket ||
        kind == lexer::TokenKind::LBrace) {
      ++depth;
    }
    if ((kind == lexer::TokenKind::RParen ||
         kind == lexer::TokenKind::RBracket ||
         kind == lexer::TokenKind::RBrace) &&
        depth == 0) {
      break;
    }
    append_pattern_token(&text, current(), previous_type_token);
    previous_type_token = &current();
    advance();
    if ((kind == lexer::TokenKind::RParen ||
         kind == lexer::TokenKind::RBracket ||
         kind == lexer::TokenKind::RBrace) &&
        depth > 0) {
      --depth;
    }
  }
  return text;
}

std::string Parser::parse_type_term_text_until_return_boundary() {
  std::string text;
  const lexer::Token *previous_type_token = nullptr;
  int depth = 0;
  while (!at_end()) {
    const lexer::TokenKind kind = current().kind;
    if (depth == 0 &&
        (kind == lexer::TokenKind::Bang || kind == lexer::TokenKind::Colon ||
         kind == lexer::TokenKind::Newline || kind == lexer::TokenKind::Eof)) {
      break;
    }
    if (kind == lexer::TokenKind::LParen ||
        kind == lexer::TokenKind::LBracket ||
        kind == lexer::TokenKind::LBrace) {
      ++depth;
    }
    if ((kind == lexer::TokenKind::RParen ||
         kind == lexer::TokenKind::RBracket ||
         kind == lexer::TokenKind::RBrace) &&
        depth == 0) {
      break;
    }
    append_pattern_token(&text, current(), previous_type_token);
    previous_type_token = &current();
    advance();
    if ((kind == lexer::TokenKind::RParen ||
         kind == lexer::TokenKind::RBracket ||
         kind == lexer::TokenKind::RBrace) &&
        depth > 0) {
      --depth;
    }
  }
  return text;
}

std::string Parser::parse_effect_row_text() {
  consume(lexer::TokenKind::Bang, "expected '!' before effect row");
  consume(lexer::TokenKind::LBrace, "expected '{' after '!'");
  std::string text = "!{";
  const lexer::Token *previous_effect_token = nullptr;
  while (!at_end() && !check(lexer::TokenKind::RBrace) &&
         !check(lexer::TokenKind::Newline)) {
    append_pattern_token(&text, current(), previous_effect_token);
    previous_effect_token = &current();
    advance();
  }
  consume(lexer::TokenKind::RBrace, "expected '}' after effect row");
  text += "}";
  return text;
}

Parser::ClauseBody Parser::parse_clause_body() {
  ClauseBody result;

  if (match(lexer::TokenKind::Newline)) {
    consume(lexer::TokenKind::Indent, "expected indented clause body");
    while (!at_end() && !check(lexer::TokenKind::Dedent)) {
      while (match(lexer::TokenKind::Newline)) {
      }
      if (check(lexer::TokenKind::KeywordWhen)) {
        result.clauses.push_back(parse_clause());
      } else if (match(lexer::TokenKind::KeywordElse)) {
        consume(lexer::TokenKind::Colon, "expected ':' after else");
        result.else_body = parse_control_body(BodyContext::Def);
      } else {
        error(current(), "expected when or else in function clause body");
        advance();
      }
      while (match(lexer::TokenKind::Newline)) {
      }
    }
    consume(lexer::TokenKind::Dedent, "expected dedent after clause body");
    return result;
  }

  while (!at_end()) {
    if (check(lexer::TokenKind::KeywordWhen)) {
      result.clauses.push_back(parse_clause());
    } else if (match(lexer::TokenKind::KeywordElse)) {
      consume(lexer::TokenKind::Colon, "expected ':' after else");
      result.else_body = parse_control_body(BodyContext::Def);
      break;
    } else {
      break;
    }
    if (!match(lexer::TokenKind::Newline)) {
      break;
    }
    while (match(lexer::TokenKind::Newline)) {
    }
  }
  return result;
}

std::unique_ptr<ast::Expr> Parser::parse_clause() {
  const lexer::Token start = advance();
  const std::string pattern_text = parse_clause_pattern_text();
  std::unique_ptr<ast::Expr> guard;
  if (match(lexer::TokenKind::KeywordIf)) {
    guard = parse_expression(1, StopMode::Normal);
  }
  consume(lexer::TokenKind::Colon, "expected ':' after function clause");
  std::vector<std::unique_ptr<ast::Expr>> body =
      parse_control_body(BodyContext::Def);

  const lexer::Span end_span =
      body.empty() ? previous().span : body.back()->span;
  auto node =
      ast::make_expr("AstClause", ast::join_spans(start.span, end_span));
  node->string_field("pattern", pattern_text);
  if (guard) {
    node->node_field("guard_expr", std::move(guard));
  }
  node->list_field("body", std::move(body));
  return node;
}

std::string Parser::parse_clause_pattern_text() {
  std::string pattern_text;
  const lexer::Token *previous_token = nullptr;
  int bracket_depth = 0;

  while (!at_end()) {
    const lexer::Token &token = current();
    if (bracket_depth == 0 && (token.kind == lexer::TokenKind::Colon ||
                               token.kind == lexer::TokenKind::KeywordIf ||
                               token.kind == lexer::TokenKind::Newline)) {
      break;
    }

    const lexer::Token consumed = advance();
    append_pattern_token(&pattern_text, consumed, previous_token);
    if (consumed.kind == lexer::TokenKind::LParen ||
        consumed.kind == lexer::TokenKind::LBracket ||
        consumed.kind == lexer::TokenKind::LBrace) {
      ++bracket_depth;
    } else if ((consumed.kind == lexer::TokenKind::RParen ||
                consumed.kind == lexer::TokenKind::RBracket ||
                consumed.kind == lexer::TokenKind::RBrace) &&
               bracket_depth > 0) {
      --bracket_depth;
    }
    previous_token = &previous();
  }

  return pattern_text;
}

std::vector<std::unique_ptr<ast::Expr>> Parser::parse_block_params() {
  std::vector<std::unique_ptr<ast::Expr>> params;
  std::string current_pattern;
  const lexer::Token *previous_token = nullptr;
  bool has_pattern_span = false;
  lexer::Span pattern_span;
  int bracket_depth = 0;

  while (!at_end() && !check(lexer::TokenKind::Pipe)) {
    const lexer::Token &token = current();
    if (token.kind == lexer::TokenKind::Comma && bracket_depth == 0) {
      if (!current_pattern.empty()) {
        auto param = ast::make_expr("AstPatternParam", pattern_span);
        param->string_field("pattern", current_pattern);
        params.push_back(std::move(param));
        current_pattern.clear();
        previous_token = nullptr;
        has_pattern_span = false;
      }
      advance();
      continue;
    }

    const lexer::Token consumed = advance();
    if (!has_pattern_span) {
      pattern_span = consumed.span;
      has_pattern_span = true;
    } else {
      pattern_span = ast::join_spans(pattern_span, consumed.span);
    }
    append_pattern_token(&current_pattern, consumed, previous_token);
    if (consumed.kind == lexer::TokenKind::LParen ||
        consumed.kind == lexer::TokenKind::LBracket ||
        consumed.kind == lexer::TokenKind::LBrace) {
      ++bracket_depth;
    } else if ((consumed.kind == lexer::TokenKind::RParen ||
                consumed.kind == lexer::TokenKind::RBracket ||
                consumed.kind == lexer::TokenKind::RBrace) &&
               bracket_depth > 0) {
      --bracket_depth;
    }
    previous_token = &previous();
  }

  if (!current_pattern.empty()) {
    auto param = ast::make_expr("AstPatternParam", pattern_span);
    param->string_field("pattern", current_pattern);
    params.push_back(std::move(param));
  }
  return params;
}

std::vector<std::string>
Parser::parse_many_def_patterns(lexer::Span *span_out) {
  const lexer::Token open =
      consume(lexer::TokenKind::LParen, "expected '(' before parameters");
  std::vector<std::string> patterns;
  std::string current_pattern;
  const lexer::Token *previous_token = nullptr;
  int bracket_depth = 0;

  while (!at_end() && !check(lexer::TokenKind::RParen)) {
    const lexer::Token &token = current();
    if (token.kind == lexer::TokenKind::Comma && bracket_depth == 0) {
      patterns.push_back(current_pattern);
      current_pattern.clear();
      previous_token = nullptr;
      advance();
      continue;
    }

    const lexer::Token consumed = advance();
    append_pattern_token(&current_pattern, consumed, previous_token);
    if (consumed.kind == lexer::TokenKind::LParen ||
        consumed.kind == lexer::TokenKind::LBracket ||
        consumed.kind == lexer::TokenKind::LBrace) {
      ++bracket_depth;
    } else if ((consumed.kind == lexer::TokenKind::RParen ||
                consumed.kind == lexer::TokenKind::RBracket ||
                consumed.kind == lexer::TokenKind::RBrace) &&
               bracket_depth > 0) {
      --bracket_depth;
    }
    previous_token = &previous();
  }

  if (!current_pattern.empty()) {
    patterns.push_back(current_pattern);
  }

  const lexer::Token close =
      consume(lexer::TokenKind::RParen, "expected ')' after parameters");
  if (span_out != nullptr) {
    *span_out = ast::join_spans(open.span, close.span);
  }
  return patterns;
}

bool Parser::starts_clause_body() const {
  if (check(lexer::TokenKind::KeywordWhen) ||
      check(lexer::TokenKind::KeywordElse)) {
    return true;
  }
  return check(lexer::TokenKind::Newline) &&
         peek().kind == lexer::TokenKind::Indent &&
         (peek(2).kind == lexer::TokenKind::KeywordWhen ||
          peek(2).kind == lexer::TokenKind::KeywordElse);
}

bool Parser::is_simple_many_def_header() const {
  if (!check(lexer::TokenKind::LParen)) {
    return false;
  }

  int bracket_depth = 0;
  std::size_t close_index = current_;
  bool found_close = false;
  for (std::size_t i = current_; i < tokens_.size(); ++i) {
    const lexer::TokenKind kind = tokens_[i].kind;
    if (kind == lexer::TokenKind::LParen) {
      ++bracket_depth;
    } else if (kind == lexer::TokenKind::RParen) {
      --bracket_depth;
      if (bracket_depth == 0) {
        close_index = i;
        found_close = true;
        break;
      }
    }
  }
  if (!found_close) {
    return false;
  }
  if (close_index + 1 < tokens_.size() &&
      tokens_[close_index + 1].kind == lexer::TokenKind::KeywordIf) {
    return true;
  }

  auto consume_nested_until = [&](std::size_t *cursor,
                                  lexer::TokenKind first_boundary,
                                  lexer::TokenKind second_boundary) {
    int nested_depth = 0;
    while (*cursor < close_index) {
      const lexer::TokenKind kind = tokens_[*cursor].kind;
      if (kind == lexer::TokenKind::LParen ||
          kind == lexer::TokenKind::LBracket ||
          kind == lexer::TokenKind::LBrace) {
        ++nested_depth;
        ++(*cursor);
        continue;
      }
      if (kind == lexer::TokenKind::RParen ||
          kind == lexer::TokenKind::RBracket ||
          kind == lexer::TokenKind::RBrace) {
        if (nested_depth == 0) {
          break;
        }
        --nested_depth;
        ++(*cursor);
        continue;
      }
      if (nested_depth == 0 &&
          (kind == first_boundary || kind == second_boundary ||
           kind == lexer::TokenKind::Comma)) {
        break;
      }
      ++(*cursor);
    }
  };

  std::size_t cursor = current_ + 1;
  while (cursor < close_index) {
    if (tokens_[cursor].kind == lexer::TokenKind::Comma) {
      return true;
    }
    if (tokens_[cursor].kind == lexer::TokenKind::At ||
        tokens_[cursor].kind == lexer::TokenKind::AtAt) {
      ++cursor;
    }
    if (cursor >= close_index ||
        tokens_[cursor].kind != lexer::TokenKind::Identifier) {
      return true;
    }
    ++cursor;

    while (cursor < close_index) {
      const lexer::Token &token = tokens_[cursor];
      if (token.kind == lexer::TokenKind::Comma) {
        ++cursor;
        break;
      }
      if (token.kind == lexer::TokenKind::Equal ||
          token.kind == lexer::TokenKind::Colon) {
        ++cursor;
        consume_nested_until(&cursor, lexer::TokenKind::Comma,
                             lexer::TokenKind::Eof);
        if (cursor < close_index &&
            tokens_[cursor].kind == lexer::TokenKind::Comma) {
          ++cursor;
        }
        break;
      }
      if (token.kind == lexer::TokenKind::Identifier && token.lexeme == "as") {
        ++cursor;
        consume_nested_until(&cursor, lexer::TokenKind::Equal,
                             lexer::TokenKind::Colon);
        continue;
      }
      return true;
    }
  }
  return false;
}

std::unique_ptr<ast::Expr> Parser::parse_if_expr(StopMode stop_mode) {
  const lexer::Token start = advance();
  const std::size_t then_index = find_inline_then_delimiter(current_);
  if (then_index < tokens_.size()) {
    const std::vector<lexer::Token> cond_tokens =
        expression_slice_tokens(tokens_, current_, then_index);
    Parser cond_parser(cond_tokens);
    ParseResult cond_parse = cond_parser.parse_expression_unit();
    diagnostics_.insert(diagnostics_.end(), cond_parse.diagnostics.begin(),
                        cond_parse.diagnostics.end());
    std::unique_ptr<ast::Expr> cond = std::move(cond_parse.expr);
    if (cond == nullptr) {
      cond = ast::make_expr("AstError", start.span);
    }

    current_ = then_index;
    match_contextual("then");
    std::unique_ptr<ast::Expr> consequent =
        parse_expression(1, StopMode::InlineIfBranch);
    std::unique_ptr<ast::Expr> alternative;
    if (match(lexer::TokenKind::KeywordElse)) {
      alternative = parse_expression(1, stop_mode);
    } else {
      error_code(current(), "AMB-SYN-INLINE-IF-MISSING-ELSE",
                 "Inline conditional expression requires `else`.");
      alternative = ast::make_expr("AstError", current_zero_width_span());
    }

    const lexer::Span end_span =
        alternative != nullptr
            ? alternative->span
            : (consequent != nullptr ? consequent->span : cond->span);
    auto node = ast::make_expr("AstInlineIfExpr",
                               ast::join_spans(start.span, end_span));
    node->string_field("form", "inline");
    node->node_field("condition", std::move(cond));
    node->node_field("consequent", std::move(consequent));
    node->node_field("alternative", std::move(alternative));
    return node;
  }

  std::unique_ptr<ast::Expr> cond = parse_expression(1, StopMode::Normal);
  consume(lexer::TokenKind::Colon, "expected ':' after if condition");
  std::vector<std::unique_ptr<ast::Expr>> then_body =
      parse_control_body(BodyContext::Def);
  std::vector<std::unique_ptr<ast::Expr>> else_body = parse_if_tail();

  const lexer::Span end_span =
      !else_body.empty()
          ? else_body.back()->span
          : (!then_body.empty() ? then_body.back()->span : cond->span);
  auto node = ast::make_expr("AstIf", ast::join_spans(start.span, end_span));
  node->node_field("cond", std::move(cond));
  node->list_field("then_body", std::move(then_body));
  node->list_field("else_body", std::move(else_body));
  return node;
}

std::vector<std::unique_ptr<ast::Expr>> Parser::parse_if_tail() {
  std::vector<std::unique_ptr<ast::Expr>> else_body;
  if (match(lexer::TokenKind::KeywordElse)) {
    if (check(lexer::TokenKind::KeywordIf)) {
      else_body.push_back(parse_if_expr(StopMode::Normal));
    } else {
      consume(lexer::TokenKind::Colon, "expected ':' after else");
      else_body = parse_control_body(BodyContext::Def);
    }
  } else if (check(lexer::TokenKind::KeywordElif) ||
             check(lexer::TokenKind::KeywordElsif)) {
    const lexer::Token elif_token = advance();
    std::unique_ptr<ast::Expr> nested_cond =
        parse_expression(1, StopMode::Normal);
    consume(lexer::TokenKind::Colon, "expected ':' after elif condition");
    std::vector<std::unique_ptr<ast::Expr>> nested_body =
        parse_control_body(BodyContext::Def);
    std::vector<std::unique_ptr<ast::Expr>> nested_else = parse_if_tail();
    const lexer::Span nested_end =
        !nested_else.empty() ? nested_else.back()->span
                             : (!nested_body.empty() ? nested_body.back()->span
                                                     : nested_cond->span);
    auto nested =
        ast::make_expr("AstIf", ast::join_spans(elif_token.span, nested_end));
    nested->node_field("cond", std::move(nested_cond));
    nested->list_field("then_body", std::move(nested_body));
    nested->list_field("else_body", std::move(nested_else));
    else_body.push_back(std::move(nested));
  }
  return else_body;
}

std::unique_ptr<ast::Expr> Parser::parse_unless_expr() {
  const lexer::Token start = advance();
  std::unique_ptr<ast::Expr> cond = parse_expression(1, StopMode::Normal);
  consume(lexer::TokenKind::Colon, "expected ':' after unless condition");
  std::vector<std::unique_ptr<ast::Expr>> then_body =
      parse_control_body(BodyContext::Def);
  std::vector<std::unique_ptr<ast::Expr>> else_body;
  if (match(lexer::TokenKind::KeywordElse)) {
    consume(lexer::TokenKind::Colon, "expected ':' after else");
    else_body = parse_control_body(BodyContext::Def);
  }

  const lexer::Span end_span =
      !else_body.empty()
          ? else_body.back()->span
          : (!then_body.empty() ? then_body.back()->span : cond->span);
  auto node =
      ast::make_expr("AstUnless", ast::join_spans(start.span, end_span));
  node->node_field("cond", std::move(cond));
  node->list_field("then_body", std::move(then_body));
  node->list_field("else_body", std::move(else_body));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_loop_expr(const char *kind) {
  const lexer::Token start = advance();
  std::unique_ptr<ast::Expr> cond;
  if (std::string(kind) == "AstWhile" || std::string(kind) == "AstUntil") {
    cond = parse_expression(1, StopMode::Normal);
  }
  consume(lexer::TokenKind::Colon, "expected ':' after loop header");
  std::vector<std::unique_ptr<ast::Expr>> body =
      parse_control_body(BodyContext::Def);
  const lexer::Span end_span =
      body.empty() ? (cond ? cond->span : start.span) : body.back()->span;
  auto node = ast::make_expr(kind, ast::join_spans(start.span, end_span));
  if (cond) {
    node->node_field("cond", std::move(cond));
  }
  node->list_field("body", std::move(body));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_do_while_expr() {
  const lexer::Token start = advance();
  consume(lexer::TokenKind::Colon, "expected ':' after do");
  std::vector<std::unique_ptr<ast::Expr>> body =
      parse_control_body(BodyContext::Def);
  consume(lexer::TokenKind::KeywordWhile,
          "expected trailing while after do body");
  std::unique_ptr<ast::Expr> cond = parse_expression(1, StopMode::Normal);

  auto node =
      ast::make_expr("AstDoWhile", ast::join_spans(start.span, cond->span));
  node->list_field("body", std::move(body));
  node->node_field("cond", std::move(cond));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_break_expr() {
  const lexer::Token start = advance();
  std::unique_ptr<ast::Expr> value;
  if (!is_stop_token(StopMode::Normal)) {
    value = parse_expression(1, StopMode::Normal);
  }
  auto node = ast::make_expr("AstBreak",
                             value ? ast::join_spans(start.span, value->span)
                                   : start.span);
  if (value) {
    node->node_field("value", std::move(value));
  }
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_case_expr(bool strict) {
  const lexer::Token start = advance();
  std::unique_ptr<ast::Expr> scrutinee = parse_expression(1, StopMode::Normal);
  consume(lexer::TokenKind::Colon, "expected ':' after case scrutinee");
  consume(lexer::TokenKind::Newline, "expected newline after case header");
  consume(lexer::TokenKind::Indent, "expected indented case body");

  std::vector<std::unique_ptr<ast::Expr>> arms;
  std::vector<std::unique_ptr<ast::Expr>> else_body;
  while (!at_end() && !check(lexer::TokenKind::Dedent)) {
    while (match(lexer::TokenKind::Newline)) {
    }
    if (check(lexer::TokenKind::KeywordWhen)) {
      arms.push_back(parse_case_arm());
    } else if (match(lexer::TokenKind::KeywordElse)) {
      consume(lexer::TokenKind::Colon, "expected ':' after case else");
      else_body = parse_control_body(BodyContext::Def);
    } else {
      error(current(), "expected when or else in case body");
      advance();
    }
    while (match(lexer::TokenKind::Newline)) {
    }
  }
  consume(lexer::TokenKind::Dedent, "expected dedent after case body");

  lexer::Span end_span = scrutinee->span;
  if (!else_body.empty()) {
    end_span = else_body.back()->span;
  } else if (!arms.empty()) {
    end_span = arms.back()->span;
  }
  auto node = ast::make_expr(strict ? "AstCase" : "AstCase",
                             ast::join_spans(start.span, end_span));
  node->bool_field("strict", strict);
  node->node_field("scrutinee", std::move(scrutinee));
  node->list_field("arms", std::move(arms));
  node->list_field("else_body", std::move(else_body));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_case_arm() {
  const lexer::Token start = advance();
  const std::string pattern_text = parse_clause_pattern_text();

  std::unique_ptr<ast::Expr> guard;
  if (match(lexer::TokenKind::KeywordIf)) {
    guard = parse_expression(1, StopMode::Normal);
  }
  consume(lexer::TokenKind::Colon, "expected ':' after case arm");
  std::vector<std::unique_ptr<ast::Expr>> body =
      parse_control_body(BodyContext::Def);

  const lexer::Span end_span =
      body.empty() ? previous().span : body.back()->span;
  auto node =
      ast::make_expr("AstCaseArm", ast::join_spans(start.span, end_span));
  node->string_field("pattern", pattern_text);
  if (guard) {
    node->node_field("guard_expr", std::move(guard));
  }
  node->list_field("body", std::move(body));
  return node;
}

std::vector<std::unique_ptr<ast::Expr>>
Parser::parse_control_body(BodyContext context) {
  return parse_body(context);
}

std::unique_ptr<ast::Expr> Parser::try_parse_pattern_assignment() {
  const std::size_t start_index = current_;
  int bracket_depth = 0;
  std::size_t equal_index = tokens_.size();
  std::size_t index = current_;
  while (index < tokens_.size()) {
    const lexer::Token &token = tokens_[index];
    if (token.kind == lexer::TokenKind::Newline ||
        token.kind == lexer::TokenKind::Dedent ||
        token.kind == lexer::TokenKind::Eof) {
      break;
    }
    if (token.kind == lexer::TokenKind::LParen ||
        token.kind == lexer::TokenKind::LBracket ||
        token.kind == lexer::TokenKind::LBrace) {
      ++bracket_depth;
    } else if ((token.kind == lexer::TokenKind::RParen ||
                token.kind == lexer::TokenKind::RBracket ||
                token.kind == lexer::TokenKind::RBrace) &&
               bracket_depth > 0) {
      --bracket_depth;
    } else if (token.kind == lexer::TokenKind::Equal && bracket_depth == 0) {
      equal_index = index;
      break;
    }
    ++index;
  }
  if (equal_index == tokens_.size() || equal_index == current_) {
    return nullptr;
  }

  const std::vector<lexer::Token> left_tokens =
      expression_slice_tokens(tokens_, current_, equal_index);
  Parser left_parser(left_tokens);
  ParseResult left_parse = left_parser.parse_expression_unit();
  if (left_parse.ok() && left_parse.expr != nullptr &&
      is_assignable(*left_parse.expr)) {
    return nullptr;
  }

  const std::string pattern_text =
      pattern_text_from_tokens(tokens_, current_, equal_index);
  current_ = equal_index + 1;
  std::unique_ptr<ast::Expr> right = parse_expression(1, StopMode::Normal);
  const lexer::Span left_span =
      ast::join_spans(tokens_[start_index].span, tokens_[equal_index - 1].span);
  auto node = ast::make_expr("AstPatternAssign",
                             ast::join_spans(left_span, right->span));
  node->string_field("pattern", pattern_text);
  node->node_field("right", std::move(right));
  return node;
}

std::vector<std::unique_ptr<ast::Expr>>
Parser::parse_path_list(const std::string &item_kind) {
  std::vector<std::unique_ptr<ast::Expr>> paths;
  while (!at_end() && !check(lexer::TokenKind::Newline)) {
    paths.push_back(parse_path_node(item_kind));
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  return paths;
}

std::unique_ptr<ast::Expr> Parser::parse_path_node(const std::string &kind) {
  const lexer::Token start = current();
  const std::string path = parse_module_path();
  auto node =
      ast::make_expr(kind, ast::join_spans(start.span, previous().span));
  node->string_field("path", path);
  return node;
}

std::string Parser::parse_module_path() {
  std::string path = consume_identifier_text("expected module path segment");
  while (match(lexer::TokenKind::Dot)) {
    path += ".";
    path += consume_identifier_text("expected module path segment after '.'");
  }
  return path;
}

std::string Parser::consume_method_name_text(const std::string &message) {
  if (is_identifier_like_token(current().kind) ||
      is_operator_method_name(current().kind)) {
    return advance().lexeme;
  }
  error(current(), message);
  return current().lexeme;
}

const lexer::Token &
Parser::consume_identifier_like(const std::string &message) {
  if (is_identifier_like_token(current().kind)) {
    return advance();
  }
  error(current(), message);
  return current();
}

std::string Parser::consume_identifier_text(const std::string &message) {
  const lexer::Token token = consume_identifier_like(message);
  return token.lexeme;
}

bool Parser::looks_like_property_declaration() const {
  if (!check(lexer::TokenKind::KeywordProp) &&
      !check(lexer::TokenKind::KeywordClassProp)) {
    return false;
  }
  if (!is_identifier_like_token(peek().kind)) {
    return false;
  }
  const lexer::TokenKind after_name = peek(2).kind;
  return after_name == lexer::TokenKind::Colon ||
         after_name == lexer::TokenKind::LParen;
}

bool Parser::looks_like_attr_declaration() const {
  if (!check(lexer::TokenKind::KeywordAttr)) {
    return false;
  }
  if (peek().kind == lexer::TokenKind::KeywordFrom) {
    return true;
  }
  if (is_identifier_like_token(peek().kind)) {
    return true;
  }
  return false;
}

bool Parser::property_suite_starts_grouped() const {
  if (!check(lexer::TokenKind::Newline) ||
      peek().kind != lexer::TokenKind::Indent) {
    return false;
  }
  std::size_t index = current_ + 2U;
  while (index < tokens_.size() &&
         tokens_[index].kind == lexer::TokenKind::Newline) {
    ++index;
  }
  if (index + 1U >= tokens_.size()) {
    return false;
  }
  return starts_property_arm_label(tokens_[index], tokens_[index + 1U]);
}

bool Parser::match_contextual(const char *text) {
  if (check(lexer::TokenKind::Identifier) && current().lexeme == text) {
    advance();
    return true;
  }
  return false;
}

std::unique_ptr<ast::Expr> Parser::parse_expression(int min_precedence,
                                                    StopMode stop_mode) {
  std::unique_ptr<ast::Expr> left = parse_prefix(stop_mode);
  int postfix_continuation_depth = 0;

  while (true) {
    if (starts_indented_postfix_continuation()) {
      advance();
      advance();
      ++postfix_continuation_depth;
    } else if (postfix_continuation_depth > 0 &&
               starts_same_indent_postfix_continuation()) {
      advance();
    }

    if (is_stop_token(stop_mode) && !(check(lexer::TokenKind::Colon) &&
                                      can_accept_direct_block_suffix(*left))) {
      break;
    }

    if (check(lexer::TokenKind::Dot) || check(lexer::TokenKind::ChainDot) ||
        check(lexer::TokenKind::SafeDot) || check(lexer::TokenKind::LParen) ||
        check(lexer::TokenKind::LBracket) || check(lexer::TokenKind::Colon) ||
        check(lexer::TokenKind::Pipe) || starts_bare_arg()) {
      const std::size_t before = current_;
      left = parse_postfix(std::move(left), stop_mode);
      if (current_ != before) {
        continue;
      }
    }

    if (check(lexer::TokenKind::Question)) {
      const lexer::Token question = advance();
      error_code(question, "AMB-SYN-INLINE-TERNARY-CSTYLE",
                 "C-style ternary operator is not supported; use `if cond "
                 "then a else b`.");
      parse_expression(1, StopMode::Normal);
      if (match(lexer::TokenKind::Colon)) {
        parse_expression(1, stop_mode);
      }
      break;
    }

    InfixInfo info{};
    if (!infix_info(current().kind, &info) ||
        info.precedence < min_precedence) {
      break;
    }
    const lexer::Token op_token = advance();
    if (op_token.kind == lexer::TokenKind::Equal && !is_assignable(*left)) {
      error(op_token, "left side of assignment is not assignable");
    }
    if (is_chain_comparison_op(info.op)) {
      left = parse_comparison_chain(std::move(left), info, op_token, stop_mode);
      continue;
    }
    const int next_min =
        info.assoc == Assoc::Left ? info.precedence + 1 : info.precedence;
    std::unique_ptr<ast::Expr> right = parse_expression(next_min, stop_mode);
    lexer::Span span = ast::join_spans(left->span, right->span);
    auto binary = ast::make_expr(
        op_token.kind == lexer::TokenKind::Equal ? "AstAssign" : "AstBinary",
        span);
    binary->string_field("op", info.op);
    binary->node_field("left", std::move(left));
    binary->node_field("right", std::move(right));
    left = std::move(binary);
  }

  while (postfix_continuation_depth > 0) {
    match(lexer::TokenKind::Newline);
    if (match(lexer::TokenKind::Dedent)) {
      --postfix_continuation_depth;
      continue;
    }
    break;
  }

  return left;
}

std::unique_ptr<ast::Expr>
Parser::parse_comparison_chain(std::unique_ptr<ast::Expr> left,
                               const InfixInfo &first_info,
                               const lexer::Token &first_op_token,
                               StopMode stop_mode) {
  const int direction = chain_comparison_direction(first_info.op);
  const int next_min = first_info.assoc == Assoc::Left
                           ? first_info.precedence + 1
                           : first_info.precedence;
  std::unique_ptr<ast::Expr> first_right =
      parse_expression(next_min, stop_mode);

  InfixInfo next_info{};
  if (!infix_info(current().kind, &next_info) ||
      !is_chain_comparison_op(next_info.op) ||
      chain_comparison_direction(next_info.op) != direction ||
      next_info.precedence != first_info.precedence) {
    lexer::Span span = ast::join_spans(left->span, first_right->span);
    auto binary = ast::make_expr("AstBinary", span);
    binary->string_field("op", first_info.op);
    binary->node_field("left", std::move(left));
    binary->node_field("right", std::move(first_right));
    return binary;
  }

  std::vector<std::unique_ptr<ast::Expr>> links;
  auto first_link =
      ast::make_expr("AstCompareLink",
                     ast::join_spans(first_op_token.span, first_right->span));
  first_link->string_field("op", first_info.op);
  first_link->node_field("right", std::move(first_right));
  links.push_back(std::move(first_link));

  lexer::Span span = left->span;
  while (true) {
    InfixInfo info{};
    if (!infix_info(current().kind, &info) ||
        !is_chain_comparison_op(info.op) ||
        chain_comparison_direction(info.op) != direction ||
        info.precedence != first_info.precedence) {
      break;
    }
    const lexer::Token op_token = advance();
    const int link_next_min =
        info.assoc == Assoc::Left ? info.precedence + 1 : info.precedence;
    std::unique_ptr<ast::Expr> right =
        parse_expression(link_next_min, stop_mode);
    span = ast::join_spans(span, right->span);
    auto link =
        ast::make_expr("AstCompareLink",
                       ast::join_spans(op_token.span, right->span));
    link->string_field("op", info.op);
    link->node_field("right", std::move(right));
    links.push_back(std::move(link));
  }

  auto chain = ast::make_expr("AstCompareChain", span);
  chain->node_field("first", std::move(left));
  chain->list_field("links", std::move(links));
  return chain;
}

std::unique_ptr<ast::Expr> Parser::parse_prefix(StopMode stop_mode) {
  const lexer::Token token = advance();
  if (is_identifier_like_token(token.kind)) {
    auto expr = ast::make_expr("AstName", token.span);
    expr->string_field("name", token.lexeme);
    return expr;
  }
  if (token.kind == lexer::TokenKind::KeywordIf) {
    --current_;
    return parse_if_expr(stop_mode);
  }
  if (token.kind == lexer::TokenKind::KeywordUnless) {
    --current_;
    return parse_unless_expr();
  }
  if (token.kind == lexer::TokenKind::KeywordWhile) {
    --current_;
    return parse_loop_expr("AstWhile");
  }
  if (token.kind == lexer::TokenKind::KeywordUntil) {
    --current_;
    return parse_loop_expr("AstUntil");
  }
  if (token.kind == lexer::TokenKind::KeywordLoop) {
    --current_;
    return parse_loop_expr("AstLoop");
  }
  if (token.kind == lexer::TokenKind::KeywordDo) {
    --current_;
    return parse_do_while_expr();
  }
  if (token.kind == lexer::TokenKind::KeywordBreak) {
    --current_;
    return parse_break_expr();
  }
  if (token.kind == lexer::TokenKind::KeywordCase) {
    --current_;
    return parse_case_expr(false);
  }
  if (token.kind == lexer::TokenKind::KeywordCaseBang) {
    --current_;
    return parse_case_expr(true);
  }
  if (token.kind == lexer::TokenKind::Placeholder) {
    auto expr = ast::make_expr("AstPlaceholder", token.span);
    expr->string_field("name", token.lexeme);
    return expr;
  }
  if (token.kind == lexer::TokenKind::LastValue) {
    return ast::make_expr("AstLastValue", token.span);
  }
  if (token.kind == lexer::TokenKind::Colon) {
    const lexer::Token name =
        consume(lexer::TokenKind::Identifier, "expected symbol name after ':'");
    auto expr =
        ast::make_expr("AstLiteral", ast::join_spans(token.span, name.span));
    expr->string_field("token", "SYMBOL");
    expr->string_field("value", name.lexeme);
    return expr;
  }
  if (token.kind == lexer::TokenKind::String) {
    return parse_string_literal_expr(token);
  }
  if (token.kind == lexer::TokenKind::Integer ||
      token.kind == lexer::TokenKind::Float ||
      token.kind == lexer::TokenKind::KeywordTrue ||
      token.kind == lexer::TokenKind::KeywordFalse ||
      token.kind == lexer::TokenKind::KeywordNull) {
    auto expr = ast::make_expr("AstLiteral", token.span);
    expr->string_field("token", lexer::token_kind_name(token.kind));
    expr->string_field("value", token.lexeme);
    return expr;
  }
  if (token.kind == lexer::TokenKind::At ||
      token.kind == lexer::TokenKind::AtAt) {
    const lexer::Token name = consume(lexer::TokenKind::Identifier,
                                      "expected field name after sigil");
    auto expr = ast::make_expr(token.kind == lexer::TokenKind::At ? "AstIvar"
                                                                  : "AstCvar",
                               ast::join_spans(token.span, name.span));
    expr->string_field("name", name.lexeme);
    return expr;
  }
  if (token.kind == lexer::TokenKind::LParen) {
    return parse_paren_or_tuple_literal(token, stop_mode);
  }
  if (token.kind == lexer::TokenKind::LBracket) {
    std::vector<std::unique_ptr<ast::Expr>> elements =
        parse_collection_elements(lexer::TokenKind::RBracket, "AstArrayElement",
                                  stop_mode);
    const lexer::Token close = previous();
    auto expr = ast::make_expr("AstListLiteral",
                               ast::join_spans(token.span, close.span));
    expr->list_field("elements", std::move(elements));
    return expr;
  }
  if (token.kind == lexer::TokenKind::LBrace) {
    return parse_brace_collection_literal(token, stop_mode);
  }
  if (token.kind == lexer::TokenKind::Plus ||
      token.kind == lexer::TokenKind::Minus ||
      token.kind == lexer::TokenKind::KeywordNot) {
    std::unique_ptr<ast::Expr> operand = parse_expression(8, stop_mode);
    auto expr =
        ast::make_expr("AstUnary", ast::join_spans(token.span, operand->span));
    expr->string_field("op", token.lexeme);
    expr->node_field("operand", std::move(operand));
    return expr;
  }

  error(token, "expected expression");
  auto expr = ast::make_expr("AstError", token.span);
  expr->string_field("token", token.lexeme);
  return expr;
}

std::unique_ptr<ast::Expr>
Parser::parse_paren_or_tuple_literal(const lexer::Token &open,
                                     StopMode stop_mode) {
  std::vector<std::unique_ptr<ast::Expr>> elements;
  bool saw_comma = false;

  if (match(lexer::TokenKind::RParen)) {
    const lexer::Token close = previous();
    auto expr = ast::make_expr("AstTupleLiteral",
                               ast::join_spans(open.span, close.span));
    expr->list_field("elements", std::move(elements));
    return expr;
  }

  elements.push_back(parse_expression(1, stop_mode));
  while (match(lexer::TokenKind::Comma)) {
    saw_comma = true;
    if (check(lexer::TokenKind::RParen)) {
      break;
    }
    elements.push_back(parse_expression(1, stop_mode));
  }

  const lexer::Token close =
      consume(lexer::TokenKind::RParen, "expected ')' after expression");
  if (!saw_comma && elements.size() == 1U) {
    auto expr =
        ast::make_expr("AstGroup", ast::join_spans(open.span, close.span));
    expr->node_field("expr", std::move(elements.front()));
    return expr;
  }

  auto expr =
      ast::make_expr("AstTupleLiteral", ast::join_spans(open.span, close.span));
  expr->list_field("elements", std::move(elements));
  return expr;
}

std::unique_ptr<ast::Expr>
Parser::parse_brace_collection_literal(const lexer::Token &open,
                                       StopMode stop_mode) {
  if (check(lexer::TokenKind::RBrace) || starts_map_literal_entry()) {
    return parse_map_literal(open, stop_mode);
  }
  return parse_set_literal(open, stop_mode);
}

std::vector<std::unique_ptr<ast::Expr>>
Parser::parse_collection_elements(lexer::TokenKind closing_kind,
                                  const char *conditional_kind,
                                  StopMode stop_mode) {
  std::vector<std::unique_ptr<ast::Expr>> elements;
  if (match(closing_kind)) {
    return elements;
  }
  while (!check(closing_kind) && !at_end()) {
    elements.push_back(
        parse_collection_element(closing_kind, conditional_kind, stop_mode));
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
    if (check(closing_kind)) {
      break;
    }
  }
  consume(closing_kind, "expected closing delimiter");
  return elements;
}

std::unique_ptr<ast::Expr>
Parser::parse_collection_element(lexer::TokenKind closing_kind,
                                 const char *conditional_kind,
                                 StopMode stop_mode) {
  (void)closing_kind;
  if (check(lexer::TokenKind::KeywordIf) ||
      check(lexer::TokenKind::KeywordUnless)) {
    const lexer::Token token = advance();
    error_code(token, "AMB-SYN-CONDITIONAL-ELEMENT-MISSING-VALUE",
               "Conditional collection element requires a value before `if` "
               "or `unless`.");
    auto error_node = ast::make_expr("AstError", token.span);
    error_node->string_field("token", token.lexeme);
    return error_node;
  }

  std::unique_ptr<ast::Expr> value = parse_expression(1, stop_mode);
  std::unique_ptr<ast::Expr> condition = parse_collection_condition();
  if (condition == nullptr) {
    return value;
  }

  auto element = ast::make_expr(
      conditional_kind, value == nullptr
                            ? condition->span
                            : ast::join_spans(value->span, condition->span));
  element->node_field("expr", std::move(value));
  element->node_field("condition", std::move(condition));
  return element;
}

std::unique_ptr<ast::Expr> Parser::parse_collection_condition() {
  if (!check(lexer::TokenKind::KeywordIf) &&
      !check(lexer::TokenKind::KeywordUnless)) {
    return nullptr;
  }
  const lexer::Token token = advance();
  std::unique_ptr<ast::Expr> expr = parse_expression(1, StopMode::Normal);
  auto condition = ast::make_expr(
      "AstCollectionCondition",
      expr == nullptr ? token.span : ast::join_spans(token.span, expr->span));
  condition->string_field("kind", token.lexeme);
  condition->node_field("expr", std::move(expr));
  return condition;
}

std::unique_ptr<ast::Expr> Parser::parse_set_literal(const lexer::Token &open,
                                                     StopMode stop_mode) {
  std::vector<std::unique_ptr<ast::Expr>> elements;

  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    elements.push_back(parse_collection_element(lexer::TokenKind::RBrace,
                                                "AstSetElement", stop_mode));
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
    if (check(lexer::TokenKind::RBrace)) {
      break;
    }
  }

  const lexer::Token close =
      consume(lexer::TokenKind::RBrace, "expected '}' after set literal");
  auto expr =
      ast::make_expr("AstSetLiteral", ast::join_spans(open.span, close.span));
  expr->list_field("elements", std::move(elements));
  return expr;
}

std::unique_ptr<ast::Expr> Parser::parse_map_literal(const lexer::Token &open,
                                                     StopMode stop_mode) {
  std::vector<std::unique_ptr<ast::Expr>> entries;

  if (match(lexer::TokenKind::RBrace)) {
    const lexer::Token close = previous();
    auto expr =
        ast::make_expr("AstMapLiteral", ast::join_spans(open.span, close.span));
    expr->list_field("entries", std::move(entries));
    return expr;
  }

  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    lexer::Token key = current();
    std::string key_kind = "symbol";
    std::string key_value;
    lexer::Span key_span = key.span;

    if (match(lexer::TokenKind::Identifier)) {
      key_value = key.lexeme;
    } else if (match(lexer::TokenKind::String)) {
      key_kind = "string";
      key_value = key.lexeme;
    } else if (match(lexer::TokenKind::Colon)) {
      const lexer::Token name = consume(lexer::TokenKind::Identifier,
                                        "expected symbol key name after ':'");
      key_value = name.lexeme;
      key_span = ast::join_spans(key.span, name.span);
    } else {
      error(key, "expected map literal key");
      advance();
      continue;
    }

    consume(lexer::TokenKind::Colon, "expected ':' after map literal key");
    std::unique_ptr<ast::Expr> value = parse_expression(1, stop_mode);
    std::unique_ptr<ast::Expr> condition = parse_collection_condition();
    auto entry = ast::make_expr(
        "AstMapEntry",
        condition != nullptr
            ? ast::join_spans(key_span, condition->span)
            : (value == nullptr ? key_span
                                : ast::join_spans(key_span, value->span)));
    entry->string_field("key_kind", key_kind);
    entry->string_field("key", key_value);
    entry->node_field("value", std::move(value));
    if (condition != nullptr) {
      entry->node_field("condition", std::move(condition));
    }
    entries.push_back(std::move(entry));

    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
    if (check(lexer::TokenKind::RBrace)) {
      break;
    }
  }

  const lexer::Token close =
      consume(lexer::TokenKind::RBrace, "expected '}' after map literal");
  auto expr =
      ast::make_expr("AstMapLiteral", ast::join_spans(open.span, close.span));
  expr->list_field("entries", std::move(entries));
  return expr;
}

std::unique_ptr<ast::Expr>
Parser::parse_string_literal_expr(const lexer::Token &token) {
  auto literal = [&]() {
    auto expr = ast::make_expr("AstLiteral", token.span);
    expr->string_field("token", lexer::token_kind_name(token.kind));
    expr->string_field("value", token.lexeme);
    return expr;
  };

  if (token.lexeme.size() < 2 || token.lexeme.front() != '"') {
    return literal();
  }

  const std::size_t content_end = token.lexeme.size() - 1U;
  std::vector<std::unique_ptr<ast::Expr>> parts;
  std::size_t text_begin = 1U;
  std::size_t cursor = 1U;
  bool saw_interpolation = false;

  auto push_text_part = [&](std::size_t begin, std::size_t end) {
    if (end <= begin) {
      return;
    }
    auto part = ast::make_expr("AstStringText",
                               span_for_token_slice(token, begin, end));
    part->string_field("value", token.lexeme.substr(begin, end - begin));
    parts.push_back(std::move(part));
  };

  auto push_escape_part = [&](std::size_t begin) {
    const StringEscapePart escape =
        parse_string_escape_part(token.lexeme, begin, content_end);
    auto part =
        ast::make_expr("AstStringEscape",
                       span_for_token_slice(token, begin, escape.end_offset));
    part->string_field("escape_kind", escape.kind);
    part->string_field("source", escape.source);
    part->string_field("value", escape.value);
    parts.push_back(std::move(part));
    return escape.end_offset;
  };

  auto find_interpolation_end = [&](std::size_t begin,
                                    std::size_t *end_out) -> bool {
    int depth = 0;
    std::size_t i = begin;
    while (i < content_end) {
      const char c = token.lexeme[i];
      if (c == '\\') {
        i += 2;
        continue;
      }
      if (c == '"' || c == '\'') {
        const char quote = c;
        ++i;
        while (i < content_end) {
          if (token.lexeme[i] == '\\') {
            i += 2;
            continue;
          }
          if (token.lexeme[i] == quote) {
            ++i;
            break;
          }
          ++i;
        }
        continue;
      }
      if (c == '{') {
        ++depth;
        ++i;
        continue;
      }
      if (c == '}') {
        if (depth == 0) {
          *end_out = i;
          return true;
        }
        --depth;
        ++i;
        continue;
      }
      ++i;
    }
    return false;
  };

  while (cursor < content_end) {
    if (token.lexeme[cursor] == '\\') {
      push_text_part(text_begin, cursor);
      cursor = push_escape_part(cursor);
      text_begin = cursor;
      continue;
    }
    if (token.lexeme[cursor] == '#' && cursor + 1U < content_end &&
        token.lexeme[cursor + 1U] == '{') {
      saw_interpolation = true;
      push_text_part(text_begin, cursor);
      const std::size_t expr_begin = cursor + 2U;
      std::size_t expr_end = expr_begin;
      if (!find_interpolation_end(expr_begin, &expr_end)) {
        diagnostics_.push_back(lexer::Diagnostic{
            "AMB_STRING_INTERP_UNTERMINATED", "error", "lexer",
            "unterminated string interpolation",
            span_for_token_slice(token, cursor, content_end)});
        expr_end = content_end;
      }
      if (string_is_blank(
              token.lexeme.substr(expr_begin, expr_end - expr_begin))) {
        diagnostics_.push_back(lexer::Diagnostic{
            "AMB_STRING_INTERP_EMPTY", "error", "parser",
            "empty string interpolation",
            span_for_token_slice(token, cursor, expr_end + 1U)});
        cursor = expr_end + 1U;
        text_begin = cursor;
        continue;
      }
      const std::string expr_source =
          token.lexeme.substr(expr_begin, expr_end - expr_begin);
      const lexer::Position expr_base =
          offset_position_in_token(token, expr_begin);
      amber::lexer::Lexer lexer(expr_source, token.span.file);
      amber::lexer::LexResult lex_result = lexer.lex();
      shift_token_spans_from_interpolation(&lex_result.tokens, expr_base);
      if (!lex_result.ok()) {
        for (lexer::Diagnostic diagnostic : lex_result.diagnostics) {
          shift_position_from_interpolation(&diagnostic.span.start, expr_base);
          shift_position_from_interpolation(&diagnostic.span.end, expr_base);
          diagnostics_.push_back(std::move(diagnostic));
        }
        cursor = expr_end + 1U;
        text_begin = cursor;
        continue;
      }
      Parser nested(lex_result.tokens);
      ParseResult parsed = nested.parse_expression_unit();
      if (!parsed.ok()) {
        for (lexer::Diagnostic diagnostic : parsed.diagnostics) {
          diagnostic.code = "AMB_STRING_INTERP_PARSE_ERROR";
          diagnostic.message = "invalid expression in string interpolation: " +
                               diagnostic.message;
          diagnostics_.push_back(std::move(diagnostic));
        }
        cursor = expr_end + 1U;
        text_begin = cursor;
        continue;
      }
      auto part = ast::make_expr(
          "AstStringExpr", span_for_token_slice(token, cursor, expr_end + 1U));
      part->node_field("expr", std::move(parsed.expr));
      parts.push_back(std::move(part));
      cursor = expr_end + 1U;
      text_begin = cursor;
      continue;
    }
    ++cursor;
  }

  push_text_part(text_begin, content_end);

  auto expr = ast::make_expr("AstStringLiteral", token.span);
  expr->string_field("quote_kind", "double");
  expr->bool_field("interpolation", saw_interpolation);
  expr->list_field("parts", std::move(parts));
  return expr;
}

std::unique_ptr<ast::Expr>
Parser::parse_postfix(std::unique_ptr<ast::Expr> expr, StopMode stop_mode) {
  if (check(lexer::TokenKind::Dot) || check(lexer::TokenKind::ChainDot)) {
    const lexer::Token dot = advance();
    const lexer::Token name = consume_identifier_like(
        "expected method or field name");
    auto tail = ast::make_expr("AstTailDotMember",
                               ast::join_spans(dot.span, name.span));
    tail->string_field("name", name.lexeme);
    tail->bool_field("chain_boundary", dot.kind == lexer::TokenKind::ChainDot);
    auto chain = ensure_postfix_chain(std::move(expr));
    append_postfix_tail(*chain, std::move(tail));
    return chain;
  }
  if (check(lexer::TokenKind::SafeDot)) {
    const lexer::Token dot = advance();
    if (match(lexer::TokenKind::LParen)) {
      std::vector<std::unique_ptr<ast::Expr>> args =
          parse_expr_list(lexer::TokenKind::RParen, stop_mode);
      const lexer::Token close = previous();
      auto tail = ast::make_expr("AstTailSafeCall",
                                 ast::join_spans(dot.span, close.span));
      tail->string_field("call_style", "paren");
      tail->list_field("args", std::move(args));
      auto chain = ensure_postfix_chain(std::move(expr));
      append_postfix_tail(*chain, std::move(tail));
      if (check(lexer::TokenKind::Colon) || check(lexer::TokenKind::Pipe)) {
        auto block = parse_block_suffix(stop_mode);
        auto block_tail = ast::make_expr("AstTailBlockSuffix", block->span);
        block_tail->node_field("block", std::move(block));
        append_postfix_tail(*chain, std::move(block_tail));
      }
      return chain;
    }
    if (match(lexer::TokenKind::LBracket)) {
      std::unique_ptr<ast::Expr> index = parse_expression(1, stop_mode);
      const lexer::Token close =
          consume(lexer::TokenKind::RBracket, "expected ']' after index");
      auto tail = ast::make_expr("AstTailSafeIndex",
                                 ast::join_spans(dot.span, close.span));
      tail->node_field("index_expr", std::move(index));
      auto chain = ensure_postfix_chain(std::move(expr));
      append_postfix_tail(*chain, std::move(tail));
      return chain;
    }
    const lexer::Token name = consume_identifier_like(
        "expected method or field name");
    auto tail = ast::make_expr("AstTailSafeMember",
                               ast::join_spans(dot.span, name.span));
    tail->string_field("name", name.lexeme);
    auto chain = ensure_postfix_chain(std::move(expr));
    append_postfix_tail(*chain, std::move(tail));
    return chain;
  }
  if (match(lexer::TokenKind::LParen)) {
    const lexer::Token open = previous();
    std::vector<std::unique_ptr<ast::Expr>> args =
        parse_expr_list(lexer::TokenKind::RParen, stop_mode);
    const lexer::Token close = previous();
    auto tail =
        ast::make_expr("AstTailCall", ast::join_spans(open.span, close.span));
    tail->string_field("call_style", "paren");
    tail->list_field("args", std::move(args));
    auto chain = ensure_postfix_chain(std::move(expr));
    append_postfix_tail(*chain, std::move(tail));
    if (check(lexer::TokenKind::Colon) || check(lexer::TokenKind::Pipe)) {
      auto block = parse_block_suffix(stop_mode);
      auto block_tail = ast::make_expr("AstTailBlockSuffix", block->span);
      block_tail->node_field("block", std::move(block));
      append_postfix_tail(*chain, std::move(block_tail));
    }
    return chain;
  }
  if (match(lexer::TokenKind::LBracket)) {
    const lexer::Token open = previous();
    std::unique_ptr<ast::Expr> index = parse_expression(1, stop_mode);
    const lexer::Token close =
        consume(lexer::TokenKind::RBracket, "expected ']' after index");
    auto tail =
        ast::make_expr("AstTailIndex", ast::join_spans(open.span, close.span));
    tail->node_field("index_expr", std::move(index));
    auto chain = ensure_postfix_chain(std::move(expr));
    append_postfix_tail(*chain, std::move(tail));
    return chain;
  }
  if ((check(lexer::TokenKind::Colon) || check(lexer::TokenKind::Pipe)) &&
      can_accept_direct_block_suffix(*expr)) {
    auto block = parse_block_suffix(stop_mode);
    auto block_tail = ast::make_expr("AstTailBlockSuffix", block->span);
    block_tail->node_field("block", std::move(block));
    auto chain = ensure_postfix_chain(std::move(expr));
    append_postfix_tail(*chain, std::move(block_tail));
    return chain;
  }
  if (starts_bare_arg() && can_accept_bare_call(*expr)) {
    std::vector<std::unique_ptr<ast::Expr>> args = parse_bare_args(stop_mode);
    lexer::Span tail_span = args.empty() ? current().span : args.front()->span;
    if (!args.empty()) {
      tail_span = ast::join_spans(args.front()->span, args.back()->span);
    }
    auto tail = ast::make_expr("AstTailCall", tail_span);
    tail->string_field("call_style", "bare");
    tail->list_field("args", std::move(args));
    auto chain = ensure_postfix_chain(std::move(expr));
    append_postfix_tail(*chain, std::move(tail));
    if (check(lexer::TokenKind::Colon) || check(lexer::TokenKind::Pipe)) {
      auto block = parse_block_suffix(stop_mode);
      auto block_tail = ast::make_expr("AstTailBlockSuffix", block->span);
      block_tail->node_field("block", std::move(block));
      append_postfix_tail(*chain, std::move(block_tail));
    }
    return chain;
  }
  return expr;
}

std::unique_ptr<ast::Expr> Parser::parse_block_suffix(StopMode stop_mode) {
  (void)stop_mode;
  std::vector<std::unique_ptr<ast::Expr>> params;
  lexer::Span start_span = current().span;
  if (match(lexer::TokenKind::Pipe)) {
    params = parse_block_params();
    consume(lexer::TokenKind::Pipe, "expected '|' after block parameters");
  }
  consume(lexer::TokenKind::Colon, "expected ':' before block body");

  if (check(lexer::TokenKind::Newline)) {
    error(current(),
          "indented block suffix parsing is not implemented in W1.2");
    auto block = ast::make_expr("AstBlock", start_span);
    block->list_field("params", std::move(params));
    return block;
  }

  std::unique_ptr<ast::Expr> body = parse_expression(1, StopMode::InlineBlock);
  auto block =
      ast::make_expr("AstBlock", ast::join_spans(start_span, body->span));
  block->list_field("params", std::move(params));
  block->node_field("body", std::move(body));
  return block;
}

std::vector<std::unique_ptr<ast::Expr>>
Parser::parse_paren_args(StopMode stop_mode) {
  return parse_expr_list(lexer::TokenKind::RParen, stop_mode);
}

std::vector<std::unique_ptr<ast::Expr>>
Parser::parse_bare_args(StopMode stop_mode) {
  std::vector<std::unique_ptr<ast::Expr>> args;
  while (starts_bare_arg()) {
    if (check(lexer::TokenKind::Identifier) &&
        peek().kind == lexer::TokenKind::Colon) {
      const lexer::Token name = advance();
      advance();
      std::unique_ptr<ast::Expr> value = parse_expression(1, stop_mode);
      auto keyword = ast::make_expr("AstKeywordArg",
                                    ast::join_spans(name.span, value->span));
      keyword_arg_field(*keyword, name);
      keyword->node_field("value", std::move(value));
      args.push_back(std::move(keyword));
    } else {
      args.push_back(parse_expression(1, stop_mode));
    }
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  return args;
}

std::vector<std::unique_ptr<ast::Expr>>
Parser::parse_expr_list(lexer::TokenKind closing_kind, StopMode stop_mode) {
  std::vector<std::unique_ptr<ast::Expr>> values;
  if (match(closing_kind)) {
    return values;
  }
  while (!check(closing_kind) && !at_end()) {
    if (check(lexer::TokenKind::Identifier) &&
        peek().kind == lexer::TokenKind::Colon) {
      const lexer::Token name = advance();
      advance();
      std::unique_ptr<ast::Expr> value = parse_expression(1, stop_mode);
      auto keyword = ast::make_expr("AstKeywordArg",
                                    ast::join_spans(name.span, value->span));
      keyword_arg_field(*keyword, name);
      keyword->node_field("value", std::move(value));
      values.push_back(std::move(keyword));
    } else {
      values.push_back(parse_expression(1, stop_mode));
    }
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
    if (check(closing_kind)) {
      break;
    }
  }
  consume(closing_kind, "expected closing delimiter");
  return values;
}

bool Parser::is_stop_token(StopMode stop_mode) const {
  switch (current().kind) {
  case lexer::TokenKind::Eof:
  case lexer::TokenKind::Newline:
  case lexer::TokenKind::Dedent:
  case lexer::TokenKind::Comma:
  case lexer::TokenKind::RParen:
  case lexer::TokenKind::RBracket:
  case lexer::TokenKind::RBrace:
    return true;
  case lexer::TokenKind::Colon:
    return true;
  case lexer::TokenKind::ChainDot:
    return stop_mode == StopMode::InlineBlock;
  case lexer::TokenKind::KeywordElse:
    return stop_mode == StopMode::InlineIfBranch;
  default:
    return false;
  }
}

bool Parser::starts_primary() const {
  switch (current().kind) {
  case lexer::TokenKind::Identifier:
  case lexer::TokenKind::KeywordAttr:
  case lexer::TokenKind::KeywordProp:
  case lexer::TokenKind::KeywordClassProp:
  case lexer::TokenKind::Placeholder:
  case lexer::TokenKind::LastValue:
  case lexer::TokenKind::Integer:
  case lexer::TokenKind::Float:
  case lexer::TokenKind::String:
  case lexer::TokenKind::Colon:
  case lexer::TokenKind::KeywordTrue:
  case lexer::TokenKind::KeywordFalse:
  case lexer::TokenKind::KeywordNull:
  case lexer::TokenKind::At:
  case lexer::TokenKind::AtAt:
  case lexer::TokenKind::LParen:
  case lexer::TokenKind::LBracket:
  case lexer::TokenKind::LBrace:
  case lexer::TokenKind::Plus:
  case lexer::TokenKind::Minus:
  case lexer::TokenKind::KeywordNot:
    return true;
  default:
    return false;
  }
}

bool Parser::starts_bare_arg() const {
  switch (current().kind) {
  case lexer::TokenKind::Identifier:
  case lexer::TokenKind::KeywordAttr:
  case lexer::TokenKind::KeywordProp:
  case lexer::TokenKind::KeywordClassProp:
  case lexer::TokenKind::Placeholder:
  case lexer::TokenKind::LastValue:
  case lexer::TokenKind::Integer:
  case lexer::TokenKind::Float:
  case lexer::TokenKind::String:
  case lexer::TokenKind::Colon:
  case lexer::TokenKind::KeywordTrue:
  case lexer::TokenKind::KeywordFalse:
  case lexer::TokenKind::KeywordNull:
  case lexer::TokenKind::At:
  case lexer::TokenKind::AtAt:
  case lexer::TokenKind::LParen:
  case lexer::TokenKind::LBracket:
  case lexer::TokenKind::LBrace:
    return true;
  default:
    return false;
  }
}

bool Parser::starts_indented_postfix_continuation() const {
  if (!check(lexer::TokenKind::Newline) ||
      peek().kind != lexer::TokenKind::Indent) {
    return false;
  }
  const lexer::TokenKind next = peek(2).kind;
  return next == lexer::TokenKind::Dot || next == lexer::TokenKind::ChainDot ||
         next == lexer::TokenKind::SafeDot;
}

bool Parser::starts_same_indent_postfix_continuation() const {
  if (!check(lexer::TokenKind::Newline)) {
    return false;
  }
  const lexer::TokenKind next = peek().kind;
  return next == lexer::TokenKind::Dot || next == lexer::TokenKind::ChainDot ||
         next == lexer::TokenKind::SafeDot;
}

bool Parser::starts_map_literal_entry() const {
  if (current().kind == lexer::TokenKind::Identifier ||
      current().kind == lexer::TokenKind::String) {
    return peek().kind == lexer::TokenKind::Colon;
  }
  if (current().kind == lexer::TokenKind::Colon &&
      peek().kind == lexer::TokenKind::Identifier) {
    return peek(2).kind == lexer::TokenKind::Colon;
  }
  return false;
}

bool Parser::is_contextual_at(std::size_t index, const char *text) const {
  return index < tokens_.size() &&
         tokens_[index].kind == lexer::TokenKind::Identifier &&
         tokens_[index].lexeme == text;
}

bool Parser::token_slice_parses_expression(std::size_t begin,
                                           std::size_t end) const {
  if (end <= begin) {
    return false;
  }
  const std::vector<lexer::Token> slice =
      expression_slice_tokens(tokens_, begin, end);
  Parser parser(slice);
  ParseResult result = parser.parse_expression_unit();
  return result.ok();
}

std::size_t Parser::find_inline_then_delimiter(std::size_t begin) const {
  int bracket_depth = 0;
  for (std::size_t index = begin; index < tokens_.size(); ++index) {
    const lexer::TokenKind kind = tokens_[index].kind;
    if (bracket_depth == 0) {
      if (kind == lexer::TokenKind::Colon ||
          kind == lexer::TokenKind::Newline ||
          kind == lexer::TokenKind::Dedent || kind == lexer::TokenKind::Comma ||
          kind == lexer::TokenKind::RParen ||
          kind == lexer::TokenKind::RBracket ||
          kind == lexer::TokenKind::RBrace || kind == lexer::TokenKind::Eof) {
        break;
      }
      if (is_contextual_at(index, "then") &&
          token_slice_parses_expression(begin, index)) {
        return index;
      }
    }

    if (kind == lexer::TokenKind::LParen ||
        kind == lexer::TokenKind::LBracket ||
        kind == lexer::TokenKind::LBrace) {
      ++bracket_depth;
    } else if ((kind == lexer::TokenKind::RParen ||
                kind == lexer::TokenKind::RBracket ||
                kind == lexer::TokenKind::RBrace) &&
               bracket_depth > 0) {
      --bracket_depth;
    }
  }
  return tokens_.size();
}

bool Parser::can_accept_bare_call(const ast::Expr &expr) const {
  if (expr.kind == "AstName") {
    return true;
  }
  const ast::Expr *tail = last_postfix_tail(expr);
  if (tail == nullptr) {
    return false;
  }
  return tail->kind == "AstTailDotMember" || tail->kind == "AstTailSafeMember";
}

bool Parser::can_accept_direct_block_suffix(const ast::Expr &expr) const {
  const ast::Expr *tail = last_postfix_tail(expr);
  if (tail == nullptr) {
    return false;
  }
  return tail->kind == "AstTailDotMember" || tail->kind == "AstTailSafeMember";
}

bool Parser::is_assignable(const ast::Expr &expr) const {
  if (expr.kind == "AstName" || expr.kind == "AstIvar" ||
      expr.kind == "AstCvar") {
    return true;
  }
  const ast::Expr *tail = last_postfix_tail(expr);
  if (tail == nullptr) {
    return false;
  }
  return tail->kind == "AstTailDotMember" || tail->kind == "AstTailIndex";
}

bool Parser::infix_info(lexer::TokenKind kind, InfixInfo *info) const {
  switch (kind) {
  case lexer::TokenKind::Equal:
    *info = InfixInfo{1, Assoc::Right, "="};
    return true;
  case lexer::TokenKind::KeywordOr:
    *info = InfixInfo{2, Assoc::Left, "or"};
    return true;
  case lexer::TokenKind::KeywordAnd:
    *info = InfixInfo{3, Assoc::Left, "and"};
    return true;
  case lexer::TokenKind::EqualEqual:
    *info = InfixInfo{4, Assoc::Left, "=="};
    return true;
  case lexer::TokenKind::EqualEqualEqual:
    *info = InfixInfo{4, Assoc::Left, "==="};
    return true;
  case lexer::TokenKind::BangEqual:
    *info = InfixInfo{4, Assoc::Left, "!="};
    return true;
  case lexer::TokenKind::Less:
    *info = InfixInfo{4, Assoc::Left, "<"};
    return true;
  case lexer::TokenKind::LessEqual:
    *info = InfixInfo{4, Assoc::Left, "<="};
    return true;
  case lexer::TokenKind::Greater:
    *info = InfixInfo{4, Assoc::Left, ">"};
    return true;
  case lexer::TokenKind::GreaterEqual:
    *info = InfixInfo{4, Assoc::Left, ">="};
    return true;
  case lexer::TokenKind::LessEqualGreater:
    *info = InfixInfo{4, Assoc::Left, "<=>"};
    return true;
  case lexer::TokenKind::KeywordIn:
    *info = InfixInfo{4, Assoc::Left, "in"};
    return true;
  case lexer::TokenKind::DotDot:
    *info = InfixInfo{5, Assoc::Left, ".."};
    return true;
  case lexer::TokenKind::Plus:
    *info = InfixInfo{6, Assoc::Left, "+"};
    return true;
  case lexer::TokenKind::Minus:
    *info = InfixInfo{6, Assoc::Left, "-"};
    return true;
  case lexer::TokenKind::Star:
    *info = InfixInfo{7, Assoc::Left, "*"};
    return true;
  case lexer::TokenKind::Slash:
    *info = InfixInfo{7, Assoc::Left, "/"};
    return true;
  case lexer::TokenKind::SlashSlash:
    *info = InfixInfo{7, Assoc::Left, "//"};
    return true;
  case lexer::TokenKind::Percent:
    *info = InfixInfo{7, Assoc::Left, "%"};
    return true;
  default:
    return false;
  }
}

bool Parser::is_method_name_token(const lexer::Token &token) const {
  return token.kind == lexer::TokenKind::Identifier ||
         is_operator_method_name(token.kind);
}

void Parser::error(const lexer::Token &token, const std::string &message) {
  diagnostics_.push_back(lexer::Diagnostic{kParseErrorCode, "error", "parser",
                                           message, token.span});
}

void Parser::error_code(const lexer::Token &token, const std::string &code,
                        const std::string &message) {
  diagnostics_.push_back(
      lexer::Diagnostic{code, "error", "parser", message, token.span});
}

lexer::Span Parser::current_zero_width_span() const {
  lexer::Span span = current().span;
  span.end = span.start;
  return span;
}

} // namespace amber::parser
