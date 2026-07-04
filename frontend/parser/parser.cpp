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

std::unique_ptr<ast::Expr> make_same_name_value(const lexer::Token &name) {
  auto value = ast::make_expr("AstName", name.span);
  value->string_field("name", name.lexeme);
  return value;
}

bool is_same_name_map_value_boundary(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::Comma || kind == lexer::TokenKind::RBrace;
}

bool is_same_name_call_value_boundary(lexer::TokenKind kind,
                                      lexer::TokenKind closing_kind) {
  return kind == lexer::TokenKind::Comma || kind == closing_kind;
}

bool is_same_name_bare_arg_value_boundary(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::Comma || kind == lexer::TokenKind::Newline ||
         kind == lexer::TokenKind::Dedent || kind == lexer::TokenKind::Eof;
}

bool is_keyword_spread_start(const lexer::Token &first,
                             const lexer::Token &second) {
  (void)second;
  return first.kind == lexer::TokenKind::StarStar;
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

// The lexeme of a String token retains its surrounding quotes (the lexer stores
// the raw source slice). A native `from "binding"` clause carries a plain
// logical name with no escapes/interpolation, so dropping the matched quote
// pair recovers the binding text.
std::string strip_binding_quotes(const std::string &lexeme) {
  if (lexeme.size() >= 2 && (lexeme.front() == '"' || lexeme.front() == '\'') &&
      lexeme.back() == lexeme.front()) {
    return lexeme.substr(1, lexeme.size() - 2);
  }
  return lexeme;
}

// Ownership markers on a `native class` header are contextual: ordinary
// identifiers everywhere except this one slot (see the native-packages design).
bool is_ownership_marker_text(const std::string &text) {
  return text == "owned" || text == "borrowed" || text == "collected";
}

// A native-only leaf has no Amber implementation. In a bytecode build, calling
// it must fail closed, so we synthesize a fallback body that raises
// NativeRequiredError (the same body-synthesis approach the attr accessors use).
// A native build instead routes `binding` to its symbol and ignores this body.
std::vector<std::unique_ptr<ast::Expr>>
synthesize_native_required_body(const std::string &def_name,
                                const std::string &binding) {
  const std::string message = "native binding '" + binding + "' for '" +
                              def_name + "' requires a native build";
  std::string escaped;
  for (const char c : message) {
    if (c == '"' || c == '\\') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  const std::string snippet =
      "raise NativeRequiredError(\"" + escaped + "\")\n";
  amber::lexer::Lexer lexer(snippet, "<native-leaf>");
  amber::lexer::LexResult lex_result = lexer.lex();
  Parser sub_parser(lex_result.tokens);
  ParseModuleResult module = sub_parser.parse_module_unit();
  return std::move(module.items);
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
         kind == lexer::TokenKind::DotDotDot ||
         kind == lexer::TokenKind::LBracket ||
         kind == lexer::TokenKind::LBrace ||
         kind == lexer::TokenKind::Question ||
         kind == lexer::TokenKind::RParen ||
         kind == lexer::TokenKind::RBracket || kind == lexer::TokenKind::RBrace;
}

bool no_space_after_pattern_token(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::Colon || kind == lexer::TokenKind::Dot ||
         kind == lexer::TokenKind::DotDot ||
         kind == lexer::TokenKind::DotDotDot ||
         kind == lexer::TokenKind::Caret ||
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
         kind == lexer::TokenKind::StarStar ||
         kind == lexer::TokenKind::Slash ||
         kind == lexer::TokenKind::SlashSlash ||
         kind == lexer::TokenKind::Percent ||
         kind == lexer::TokenKind::Ampersand ||
         kind == lexer::TokenKind::Pipe ||
         kind == lexer::TokenKind::Caret ||
         kind == lexer::TokenKind::LessLess ||
         kind == lexer::TokenKind::GreaterGreater ||
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

bool token_can_precede_assignment_delimiter(lexer::TokenKind kind) {
  switch (kind) {
  case lexer::TokenKind::Plus:
  case lexer::TokenKind::Minus:
  case lexer::TokenKind::Star:
  case lexer::TokenKind::StarStar:
  case lexer::TokenKind::Slash:
  case lexer::TokenKind::SlashSlash:
  case lexer::TokenKind::Percent:
  case lexer::TokenKind::Ampersand:
  case lexer::TokenKind::Pipe:
  case lexer::TokenKind::Caret:
  case lexer::TokenKind::LessLess:
  case lexer::TokenKind::GreaterGreater:
    return false;
  default:
    return true;
  }
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
  bool saw_numeric_profile = false;
  bool saw_non_package_item = false;

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
      } else if (item->kind == "AstNumericProfile") {
        if (saw_numeric_profile) {
          diagnostics_.push_back({"P0001", "error", "parser",
                                  "duplicate `numeric:` preamble directive",
                                  item->span});
        }
        if (saw_non_package_item) {
          diagnostics_.push_back(
              {"P0001", "error", "parser",
               "`numeric:` preamble must appear before imports, exports, "
               "declarations, and statements",
               item->span});
        }
        saw_numeric_profile = true;
      } else {
        saw_non_package_item = true;
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

  if (context == BodyContext::Module &&
      current().kind == lexer::TokenKind::Identifier &&
      current().lexeme == "numeric" &&
      peek(1).kind == lexer::TokenKind::Colon) {
    return parse_numeric_directive();
  }

  // `next` is a contextual loop/block control keyword. At statement-leading
  // position it is the next-iteration statement when it stands alone (followed
  // by a statement terminator) or is immediately followed by a value expression
  // (`next 42`, `next x`, `next @f`). It stays an ordinary identifier when
  // followed by a postfix/assignment/infix continuation (`.`, `(`, `[`, `=`,
  // operators, `:`), so `.next`, `def next`, `next = …`, `next[i]`, `next(x)`,
  // and `:next` keep working. (`next (expr)`, `next [..]`, `next -1` therefore
  // read as identifier forms; bind the value first to yield those.)
  if (current().kind == lexer::TokenKind::Identifier &&
      current().lexeme == "next") {
    const lexer::TokenKind after = peek(1).kind;
    const bool bare = after == lexer::TokenKind::Newline ||
                      after == lexer::TokenKind::Dedent ||
                      after == lexer::TokenKind::Eof;
    const bool valued = after == lexer::TokenKind::Integer ||
                        after == lexer::TokenKind::Float ||
                        after == lexer::TokenKind::String ||
                        after == lexer::TokenKind::Identifier ||
                        after == lexer::TokenKind::At ||
                        after == lexer::TokenKind::AtAt ||
                        after == lexer::TokenKind::LastValue ||
                        after == lexer::TokenKind::KeywordTrue ||
                        after == lexer::TokenKind::KeywordFalse ||
                        after == lexer::TokenKind::KeywordNull;
    if (bare || valued) {
      return parse_next_expr();
    }
  }

  // `native` is a contextual definition modifier: it leads a `native def` or
  // `native class` only when immediately followed by `def`/`class`. Anywhere
  // else (`native = 3`, `obj.native`, `def native()`) it stays an identifier.
  if (current().kind == lexer::TokenKind::Identifier &&
      current().lexeme == "native") {
    const lexer::TokenKind after = peek(1).kind;
    if (after == lexer::TokenKind::KeywordDef) {
      const lexer::Token start = advance();
      consume(lexer::TokenKind::KeywordDef, "expected 'def' after native");
      return parse_def_stmt(false, &start, /*is_native=*/true);
    }
    if (after == lexer::TokenKind::KeywordClass) {
      const lexer::Token start = advance();
      return parse_class_def(&start);
    }
  }

  // `macro` is a contextual definition modifier: `macro def` declares a
  // compile-time macro (F1.5 expansion). Like `native`, it only leads a
  // definition when immediately followed by `def`; anywhere else (`macro = 3`,
  // `obj.macro`, `def macro()`) it stays an ordinary identifier.
  if (current().kind == lexer::TokenKind::Identifier &&
      current().lexeme == "macro") {
    const lexer::TokenKind after = peek(1).kind;
    if (after == lexer::TokenKind::KeywordDef) {
      const lexer::Token start = advance();
      consume(lexer::TokenKind::KeywordDef, "expected 'def' after macro");
      return parse_def_stmt(false, &start, /*is_native=*/false,
                            /*is_macro=*/true);
    }
  }

  // Paren-less block-suffix statement `name:` + INDENT — the block-macro DSL
  // entry (DESIGN-macro-system §8.2, e.g. `routes:`). Statement position only
  // and newline-gated (same gate as `quote:`), so control headers (`if x:`),
  // map keys, and inline `name:` labels are unaffected. `quote` keeps its
  // dedicated quasiquote node. F1.5 expands the chain when the name resolves
  // to a macro; a leftover reaching the binder is rejected there.
  if (current().kind == lexer::TokenKind::Identifier &&
      current().lexeme != "quote" &&
      peek(1).kind == lexer::TokenKind::Colon &&
      peek(2).kind == lexer::TokenKind::Newline) {
    const lexer::Token name = advance();
    auto base = ast::make_expr("AstName", name.span);
    base->string_field("name", name.lexeme);
    std::unique_ptr<ast::Expr> block = parse_block_suffix(StopMode::Normal);
    auto block_tail = ast::make_expr("AstTailBlockSuffix", block->span);
    const lexer::Span end_span = block->span;
    block_tail->node_field("block", std::move(block));
    std::vector<std::unique_ptr<ast::Expr>> tails;
    tails.push_back(std::move(block_tail));
    auto chain = ast::make_expr("AstPostfixChain",
                                ast::join_spans(name.span, end_span));
    chain->node_field("base", std::move(base));
    chain->list_field("tails", std::move(tails));
    auto stmt = ast::make_expr("AstExprStmt", chain->span);
    stmt->node_field("expr", std::move(chain));
    return stmt;
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
  case lexer::TokenKind::KeywordRescue:
    return parse_invalid_handler_stmt(true);
  case lexer::TokenKind::KeywordEnsure:
    return parse_invalid_handler_stmt(false);
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

std::unique_ptr<ast::Expr> Parser::parse_numeric_directive() {
  const lexer::Token start = advance();
  consume(lexer::TokenKind::Colon, "expected ':' after numeric");
  consume(lexer::TokenKind::Newline, "expected newline after `numeric:`");
  consume(lexer::TokenKind::Indent,
          "expected indented block after `numeric:`");

  static const char *kIntTypes[] = {"Int8",  "Int16",  "Int32",  "Int64",
                                    "UInt8", "UInt16", "UInt32", "UInt64",
                                    "BigInt"};
  std::string int_type;
  std::string overflow;
  while (!check(lexer::TokenKind::Dedent) && !at_end()) {
    while (match(lexer::TokenKind::Newline)) {
    }
    if (check(lexer::TokenKind::Dedent) || at_end()) {
      break;
    }
    const lexer::Token key = consume(
        lexer::TokenKind::Identifier,
        "expected numeric profile entry name (`int` or `overflow`)");
    consume(lexer::TokenKind::Colon,
            "expected ':' after numeric profile entry name");
    const lexer::Token value = consume(
        lexer::TokenKind::Identifier, "expected numeric profile entry value");
    if (key.lexeme == "int") {
      if (!int_type.empty()) {
        error(key, "duplicate numeric profile entry `int`");
      }
      bool known = false;
      for (const char *candidate : kIntTypes) {
        known = known || value.lexeme == candidate;
      }
      if (!known) {
        error(value, "unknown numeric profile `int` value `" + value.lexeme +
                         "` (expected Int8/Int16/Int32/Int64, "
                         "UInt8/UInt16/UInt32/UInt64, or BigInt)");
      }
      int_type = value.lexeme;
    } else if (key.lexeme == "overflow") {
      if (!overflow.empty()) {
        error(key, "duplicate numeric profile entry `overflow`");
      }
      if (value.lexeme != "checked" && value.lexeme != "wrapping" &&
          value.lexeme != "saturating") {
        error(value, "unknown numeric profile `overflow` value `" +
                         value.lexeme +
                         "` (expected checked, wrapping, or saturating)");
      }
      overflow = value.lexeme;
    } else {
      error(key, "unknown numeric profile entry `" + key.lexeme +
                     "` (expected `int` or `overflow`)");
    }
    if (!match(lexer::TokenKind::Newline) &&
        !check(lexer::TokenKind::Dedent)) {
      break;
    }
  }
  consume(lexer::TokenKind::Dedent,
          "expected dedent after numeric profile block");

  auto node = ast::make_expr("AstNumericProfile",
                             ast::join_spans(start.span, previous().span));
  node->string_field("int_type", int_type.empty() ? "Int64" : int_type);
  node->string_field("overflow", overflow.empty() ? "checked" : overflow);
  return node;
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
    // `export macro name [as public]` marks a compile-time macro export
    // (DESIGN-macro-system §11). The marker is load-bearing: an importer's
    // F1.5 classifies imported names as macros from this statically readable
    // table, without compiling the provider's runtime. `macro` stays an
    // ordinary exportable identifier when it is the exported name itself
    // (`export macro` alone or `export macro as m`).
    bool is_macro = false;
    if (current().kind == lexer::TokenKind::Identifier &&
        current().lexeme == "macro" &&
        peek(1).kind == lexer::TokenKind::Identifier &&
        peek(1).lexeme != "as") {
      advance();
      is_macro = true;
    }
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
    if (is_macro) {
      item->bool_field("is_macro", true);
    }
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
Parser::parse_def_stmt(bool class_method, const lexer::Token *start_override,
                       bool is_native, bool is_macro) {
  const lexer::Token start =
      start_override != nullptr ? *start_override : advance();
  const std::string name_text =
      consume_method_name_text("expected function name");
  std::unique_ptr<ast::Expr> signature;
  // A `macro def` always takes the standard single-signature path so the
  // emitted def node is uniformly tagged `is_macro`; multi-clause macro
  // arities are a later-milestone concern.
  if (!class_method && !is_native && !is_macro && is_simple_many_def_header()) {
    lexer::Span signature_span{};
    std::vector<std::string> patterns =
        parse_many_def_patterns(&signature_span);
    signature = make_synthetic_signature(signature_span, patterns.size());
    std::unique_ptr<ast::Expr> guard;
    if (match(lexer::TokenKind::KeywordIf)) {
      guard = parse_expression(1, StopMode::ControlHeader);
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

  // Optional native binding clause: `from "logical.name"`. Permitted on any
  // `def` (a plain `def ... from` is a native-class method binding; a `native
  // def ... from` is an accelerated free function). A binding with no body is a
  // native-only leaf.
  std::string native_binding;
  bool has_binding = false;
  if (match(lexer::TokenKind::KeywordFrom)) {
    has_binding = true;
    if (check(lexer::TokenKind::String)) {
      native_binding = strip_binding_quotes(advance().lexeme);
    } else {
      error(current(), "expected \"binding\" string after 'from'");
    }
  }
  // A `from` binding is only meaningful on a `native def` or on a method inside
  // a `native class`. A plain `def ... from` anywhere else is malformed.
  if (has_binding && !is_native && !in_native_class_body_) {
    error_code(start, "E_NATIVE_BINDING_CONTEXT",
               "a `from \"...\"` native binding requires `native def` or a "
               "method inside a `native class`");
  }
  if (has_binding && !check(lexer::TokenKind::Colon)) {
    auto node = ast::make_expr(class_method ? "AstClassMethodDef" : "AstDefStmt",
                               ast::join_spans(start.span, previous().span));
    node->string_field("name", name_text);
    node->node_field("signature", std::move(signature));
    node->list_field("body",
                     synthesize_native_required_body(name_text, native_binding));
    node->bool_field("is_native", is_native);
    node->bool_field("native_only", true);
    node->string_field("native_binding", native_binding);
    return node;
  }

  consume(lexer::TokenKind::Colon, "expected ':' after function signature");
  if (!class_method && !is_native && !is_macro && starts_clause_body()) {
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

  // A `macro def` body is a template by default (implicit quote, M4), so the
  // splice vocabulary (`unquote` / `unquote_splice` / `unhygienic`) is live in
  // it exactly as inside an explicit `quote:` block.
  if (is_macro) {
    ++quote_depth_;
  }
  std::vector<std::unique_ptr<ast::Expr>> body = parse_body(BodyContext::Def);
  if (is_macro) {
    --quote_depth_;
  }
  const lexer::Span body_end_span =
      body.empty() ? previous().span : body.back()->span;
  HandlerSuffix handlers = parse_handler_suffix(body_end_span, BodyContext::Def);
  const lexer::Span end_span =
      handlers.end_span.end.offset >= body_end_span.end.offset
          ? handlers.end_span
          : body_end_span;

  auto node = ast::make_expr(class_method ? "AstClassMethodDef" : "AstDefStmt",
                             ast::join_spans(start.span, end_span));
  node->string_field("name", name_text);
  node->node_field("signature", std::move(signature));
  node->list_field("body", std::move(body));
  if (!handlers.rescues.empty()) {
    node->list_field("rescues", std::move(handlers.rescues));
  }
  if (handlers.has_ensure) {
    node->bool_field("has_ensure", true);
    node->list_field("ensure_body", std::move(handlers.ensure_body));
  }
  if (is_native || has_binding) {
    node->bool_field("is_native", is_native);
    node->bool_field("native_only", false);
    node->string_field("native_binding", native_binding);
  }
  // Only stamped when true, so ordinary def AST dumps (goldens) are unchanged.
  if (is_macro) {
    node->bool_field("is_macro", true);
  }
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

std::unique_ptr<ast::Expr> Parser::parse_class_def(
    const lexer::Token *native_start) {
  const lexer::Token class_kw = advance();
  const lexer::Token start =
      native_start != nullptr ? *native_start : class_kw;
  const bool is_native = native_start != nullptr;
  const lexer::Token name =
      consume(lexer::TokenKind::Identifier, "expected class name");
  std::string superclass;
  if (match(lexer::TokenKind::Less)) {
    superclass = parse_module_path();
  }

  std::string native_binding;
  std::string ownership;
  if (is_native) {
    if (match(lexer::TokenKind::KeywordFrom)) {
      if (check(lexer::TokenKind::String)) {
        native_binding = strip_binding_quotes(advance().lexeme);
      } else {
        error(current(), "expected \"binding\" string after 'from'");
      }
    } else {
      error(current(), "native class requires a `from \"binding\"` clause");
    }
    // The ownership marker is required; foreign-resource ownership is never
    // decided by omission (native-packages design §4.4).
    if (check(lexer::TokenKind::Identifier) &&
        is_ownership_marker_text(current().lexeme)) {
      ownership = advance().lexeme;
    } else {
      error_code(current(), "E_NATIVE_CLASS_OWNERSHIP_REQUIRED",
                 "native class requires an ownership marker: owned, borrowed, "
                 "or collected");
    }
  }

  consume(lexer::TokenKind::Colon, "expected ':' after class header");
  const bool prev_in_native_class = in_native_class_body_;
  in_native_class_body_ = is_native;
  std::vector<std::unique_ptr<ast::Expr>> body = parse_body(BodyContext::Class);
  in_native_class_body_ = prev_in_native_class;
  const lexer::Span end_span =
      body.empty() ? previous().span : body.back()->span;

  if (is_native) {
    // Ownership and the destructor binding must agree: `owned`/`collected` own
    // the foreign handle and require a `destroy!` reclaim; `borrowed` never
    // frees it and must not declare one (native-packages design §7.1).
    bool has_destroy_binding = false;
    for (const std::unique_ptr<ast::Expr> &member : body) {
      const std::string *member_name = find_string_field(*member, "name");
      if (member_name != nullptr && *member_name == "destroy!" &&
          find_string_field(*member, "native_binding") != nullptr) {
        has_destroy_binding = true;
        break;
      }
    }
    if ((ownership == "owned" || ownership == "collected") &&
        !has_destroy_binding) {
      error_code(name, "E_NATIVE_OWNED_REQUIRES_DESTRUCTOR",
                 "a `" + ownership +
                     "` native class requires a `def destroy!() from \"...\"` "
                     "reclaim binding");
    }
    if (ownership == "borrowed" && has_destroy_binding) {
      error_code(name, "E_NATIVE_BORROWED_FORBIDS_DESTRUCTOR",
                 "a `borrowed` native class must not declare a `destroy!` "
                 "binding; the provider owns the lifetime");
    }
  }

  auto node =
      ast::make_expr("AstClassDef", ast::join_spans(start.span, end_span));
  node->string_field("name", name.lexeme);
  node->string_field("superclass", superclass);
  node->list_field("body", std::move(body));
  if (is_native) {
    node->bool_field("is_native", true);
    node->string_field("native_binding", native_binding);
    node->string_field("ownership", ownership);
  }
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

std::unique_ptr<ast::Expr> Parser::parse_invalid_handler_stmt(bool rescue) {
  const lexer::Token token = advance();
  error_code(token, rescue ? "E_RESCUE_WITHOUT_BODY" : "E_ENSURE_WITHOUT_BODY",
             rescue ? "`rescue` must follow a `try` body or function/method body"
                    : "`ensure` must follow a `try` body or function/method body");
  while (!at_end() && !check(lexer::TokenKind::Newline) &&
         !check(lexer::TokenKind::Dedent)) {
    advance();
  }
  return ast::make_expr("AstError", token.span);
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

Parser::HandlerSuffix
Parser::parse_handler_suffix(const lexer::Span &fallback_span,
                             BodyContext context) {
  HandlerSuffix suffix;
  suffix.end_span = fallback_span;
  while (!at_end()) {
    while (match(lexer::TokenKind::Newline)) {
    }
    if (check(lexer::TokenKind::KeywordRescue)) {
      if (suffix.has_ensure) {
        error_code(current(), "E_RESCUE_AFTER_ENSURE",
                   "`rescue` clauses must appear before `ensure`");
      }
      std::unique_ptr<ast::Expr> clause = parse_rescue_clause(context);
      suffix.end_span = clause == nullptr ? previous().span : clause->span;
      if (!suffix.has_ensure && clause != nullptr) {
        suffix.rescues.push_back(std::move(clause));
      }
      continue;
    }
    if (check(lexer::TokenKind::KeywordEnsure)) {
      if (suffix.has_ensure) {
        error_code(current(), "E_DUPLICATE_ENSURE",
                   "only one `ensure` clause is allowed");
      }
      std::vector<std::unique_ptr<ast::Expr>> body =
          parse_ensure_clause(context);
      suffix.end_span = body.empty() ? previous().span : body.back()->span;
      if (!suffix.has_ensure) {
        suffix.has_ensure = true;
        suffix.ensure_body = std::move(body);
      }
      continue;
    }
    break;
  }
  return suffix;
}

std::unique_ptr<ast::Expr> Parser::parse_rescue_clause(BodyContext context) {
  const lexer::Token start = advance();
  std::vector<std::unique_ptr<ast::Expr>> matchers;
  std::string binding;

  if (check(lexer::TokenKind::Pipe)) {
    binding = parse_exception_binding();
  } else if (!check(lexer::TokenKind::Colon)) {
    while (!at_end() && !check(lexer::TokenKind::Colon) &&
           !check(lexer::TokenKind::Newline)) {
      const lexer::Span matcher_start = current().span;
      std::string matcher = parse_rescue_matcher_text();
      if (!matcher.empty()) {
        auto matcher_node = ast::make_expr(
            "AstRescueMatcher", ast::join_spans(matcher_start, previous().span));
        matcher_node->string_field("type_expr", matcher);
        matchers.push_back(std::move(matcher_node));
      } else if (!check(lexer::TokenKind::Pipe)) {
        error_code(current(), "E_INVALID_RESCUE_MATCHER",
                   "rescue matcher must be an exception class/type term");
        break;
      }
      if (match(lexer::TokenKind::Comma)) {
        if (check(lexer::TokenKind::Colon) || check(lexer::TokenKind::Pipe)) {
          error_code(previous(), "E_INVALID_RESCUE_MATCHER",
                     "rescue matcher must be an exception class/type term");
        }
        continue;
      }
      if (check(lexer::TokenKind::Pipe)) {
        binding = parse_exception_binding();
        if (!check(lexer::TokenKind::Colon)) {
          error_code(previous(), "E_RESCUE_PIPE_UNION_FORBIDDEN",
                     "use comma-separated rescue matcher list: `rescue "
                     "TypeError, ArgumentError |e|:`");
          while (!at_end() && !check(lexer::TokenKind::Colon) &&
                 !check(lexer::TokenKind::Newline)) {
            advance();
          }
        }
      }
      break;
    }
  }

  consume(lexer::TokenKind::Colon, "expected ':' after rescue clause");
  std::vector<std::unique_ptr<ast::Expr>> body = parse_control_body(context);
  if (body.empty()) {
    error_code(start, "E_RESCUE_WITHOUT_BODY",
               "empty rescue body is not allowed; use `pass` or `noop`");
  }

  const lexer::Span end_span =
      body.empty() ? previous().span : body.back()->span;
  auto node =
      ast::make_expr("AstRescueClause", ast::join_spans(start.span, end_span));
  node->string_field("binding", binding);
  node->list_field("matchers", std::move(matchers));
  node->list_field("body", std::move(body));
  return node;
}

std::string Parser::parse_rescue_matcher_text() {
  std::string text;
  const lexer::Token *previous_token = nullptr;
  int bracket_depth = 0;
  while (!at_end()) {
    const lexer::TokenKind kind = current().kind;
    if (bracket_depth == 0 &&
        (kind == lexer::TokenKind::Comma || kind == lexer::TokenKind::Pipe ||
         kind == lexer::TokenKind::Colon ||
         kind == lexer::TokenKind::Newline ||
         kind == lexer::TokenKind::Eof)) {
      break;
    }
    const lexer::Token consumed = advance();
    append_pattern_token(&text, consumed, previous_token);
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
  return text;
}

std::string Parser::parse_exception_binding() {
  const lexer::Token open =
      consume(lexer::TokenKind::Pipe, "expected '|' before exception binding");
  std::string binding;
  if (check(lexer::TokenKind::Identifier)) {
    binding = advance().lexeme;
  } else {
    error_code(current(), "E_INVALID_RESCUE_BINDING",
               "exception binding must use `|name|`");
  }
  if (!match(lexer::TokenKind::Pipe)) {
    error_code(open, "E_INVALID_RESCUE_BINDING",
               "exception binding must use `|name|`");
  }
  return binding;
}

std::vector<std::unique_ptr<ast::Expr>>
Parser::parse_ensure_clause(BodyContext context) {
  const lexer::Token start = advance();
  consume(lexer::TokenKind::Colon, "expected ':' after ensure clause");
  std::vector<std::unique_ptr<ast::Expr>> body = parse_control_body(context);
  if (body.empty()) {
    error_code(start, "E_ENSURE_WITHOUT_BODY",
               "empty ensure body is not allowed; use `pass` or `noop`");
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
  // RFC block-parameters §4.2: at most one block parameter, and it must be the
  // final parameter. Any block parameter that is not the last element is
  // flagged — this catches both "not last" and "more than one" (an earlier
  // duplicate is necessarily not last).
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (string_value(*params[i], "param_kind") == "block" &&
        i + 1 != params.size()) {
      diagnostics_.push_back(lexer::Diagnostic{
          "AMB_BLOCK_PARAM_NOT_LAST", "error", "parser",
          "a block parameter must be the last parameter, and at most one is "
          "allowed",
          params[i]->span});
    }
  }
  auto signature =
      ast::make_expr("AstSignature", ast::join_spans(start.span, close.span));
  signature->list_field("params", std::move(params));
  return signature;
}

std::unique_ptr<ast::Expr> Parser::parse_param() {
  const lexer::Token start = current();
  // RFC block-parameters §4.1: a trailing `&name` (or `&@field`) block
  // parameter binds the frame's block channel. `&` is the callable-channel
  // sigil already established for callable references (§4.6).
  const bool is_block = match(lexer::TokenKind::Ampersand);
  std::string auto_assign_kind = "none";
  if (match(lexer::TokenKind::At)) {
    auto_assign_kind = "@";
  } else if (match(lexer::TokenKind::AtAt)) {
    auto_assign_kind = "@@";
  }
  // A `*name` rest parameter collects surplus positional arguments into an
  // immutable Tuple. It cannot combine with `&` (block) or `@`/`@@`
  // (auto-assign), and takes neither a keyword `:` nor a default `=`.
  const bool is_rest =
      !is_block && auto_assign_kind == "none" && match(lexer::TokenKind::Star);
  // A `**name` keyword-rest parameter collects unmatched keyword arguments
  // into a Map. Same exclusivity rules as `*name`.
  const bool is_kw_rest = !is_block && !is_rest && auto_assign_kind == "none" &&
                          match(lexer::TokenKind::StarStar);
  // §4.2: a block parameter is always a single name, never a pattern.
  if (is_block && !check(lexer::TokenKind::Identifier)) {
    error_code(current(), "AMB_BLOCK_PARAM_PATTERN",
               "a block parameter must be a single name, not a pattern");
  }
  const lexer::Token name =
      consume(lexer::TokenKind::Identifier, "expected parameter name");
  std::string kind = is_block      ? "block"
                     : is_rest      ? "rest"
                     : is_kw_rest   ? "kw_rest"
                                    : "positional";
  std::string type_expr;
  std::unique_ptr<ast::Expr> default_expr;

  if (match_contextual("as")) {
    type_expr = parse_type_term_text_until_param_boundary();
  }

  // Block parameters take neither a keyword `:` nor a default `=` in v1
  // (default deferred per RFC §10.1); leaving them unconsumed lets the
  // signature's `)` expectation flag a malformed `&blk: …` / `&blk = …`.
  if (!is_block && !is_rest && !is_kw_rest && match(lexer::TokenKind::Colon)) {
    kind = "keyword";
    if (!check(lexer::TokenKind::Comma) && !check(lexer::TokenKind::RParen)) {
      default_expr = parse_expression(1, StopMode::Normal);
    }
  } else if (!is_block && !is_rest && !is_kw_rest &&
             match(lexer::TokenKind::Equal)) {
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
         kind == lexer::TokenKind::KeywordFrom ||
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
    guard = parse_expression(1, StopMode::ControlHeader);
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
  // A `&` block-parameter sigil only appears in an ordinary signature, never in
  // the many-def pattern sugar (§10.4 is positional-only). Its presence forces
  // normal signature parsing so block params bind and `&(pattern)` reaches the
  // precise AMB_BLOCK_PARAM_PATTERN diagnostic (RFC block-parameters).
  for (std::size_t i = current_ + 1; i < close_index; ++i) {
    if (tokens_[i].kind == lexer::TokenKind::Ampersand) {
      return false;
    }
  }
  // A top-level `*name` / `**name` rest sigil is likewise a signature construct
  // (it collects surplus positional/keyword arguments into a Tuple/Map), not a
  // destructuring pattern, so its presence forces normal signature parsing.
  // Tracked at bracket depth 0 only, so a nested rest-binding pattern such as
  // `def f((a, *rest)):` still reaches the many-def pattern path.
  {
    int rest_depth = 0;
    for (std::size_t i = current_ + 1; i < close_index; ++i) {
      const lexer::TokenKind kind = tokens_[i].kind;
      if (kind == lexer::TokenKind::LParen ||
          kind == lexer::TokenKind::LBracket ||
          kind == lexer::TokenKind::LBrace) {
        ++rest_depth;
      } else if (kind == lexer::TokenKind::RParen ||
                 kind == lexer::TokenKind::RBracket ||
                 kind == lexer::TokenKind::RBrace) {
        --rest_depth;
      } else if (rest_depth == 0 &&
                 (kind == lexer::TokenKind::Star ||
                  kind == lexer::TokenKind::StarStar)) {
        return false;
      }
    }
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

  std::unique_ptr<ast::Expr> cond =
      parse_expression(1, StopMode::ControlHeader);
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
        parse_expression(1, StopMode::ControlHeader);
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
  std::unique_ptr<ast::Expr> cond =
      parse_expression(1, StopMode::ControlHeader);
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
    cond = parse_expression(1, StopMode::ControlHeader);
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
  std::unique_ptr<ast::Expr> cond =
      parse_expression(1, StopMode::ControlHeader);

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

std::unique_ptr<ast::Expr> Parser::parse_next_expr() {
  const lexer::Token start = advance();
  std::unique_ptr<ast::Expr> value;
  if (!is_stop_token(StopMode::Normal)) {
    value = parse_expression(1, StopMode::Normal);
  }
  auto node = ast::make_expr("AstNext",
                             value ? ast::join_spans(start.span, value->span)
                                   : start.span);
  if (value) {
    node->node_field("value", std::move(value));
  }
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_return_expr() {
  const lexer::Token start = advance();
  std::unique_ptr<ast::Expr> value;
  if (!is_stop_token(StopMode::Normal)) {
    value = parse_expression(1, StopMode::Normal);
  }
  auto node = ast::make_expr("AstReturn",
                             value ? ast::join_spans(start.span, value->span)
                                   : start.span);
  if (value) {
    node->node_field("value", std::move(value));
  }
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_throw_expr() {
  const lexer::Token start = advance();
  std::unique_ptr<ast::Expr> tag;
  std::unique_ptr<ast::Expr> value;
  if (!starts_primary()) {
    error(start, "`throw` requires a tag expression");
    tag = ast::make_expr("AstError", start.span);
  } else {
    tag = parse_expression(1, StopMode::Normal);
  }
  if (match(lexer::TokenKind::Comma)) {
    value = parse_expression(1, StopMode::Normal);
  }

  const lexer::Span end_span =
      value != nullptr ? value->span : (tag != nullptr ? tag->span : start.span);
  auto node =
      ast::make_expr("AstThrow", ast::join_spans(start.span, end_span));
  if (tag != nullptr) {
    node->node_field("tag", std::move(tag));
  }
  if (value != nullptr) {
    node->node_field("value", std::move(value));
  }
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_catch_expr() {
  const lexer::Token start = advance();
  std::unique_ptr<ast::Expr> tag;
  if (match(lexer::TokenKind::LParen)) {
    tag = parse_expression(1, StopMode::Normal);
    consume(lexer::TokenKind::RParen, "expected ')' after catch tag");
  } else {
    tag = parse_expression(1, StopMode::ControlHeader);
  }
  consume(lexer::TokenKind::Colon, "expected ':' after catch tag");
  std::vector<std::unique_ptr<ast::Expr>> body =
      parse_control_body(BodyContext::Def);
  const lexer::Span end_span =
      body.empty() ? previous().span : body.back()->span;
  auto node =
      ast::make_expr("AstCatch", ast::join_spans(start.span, end_span));
  node->node_field("tag", std::move(tag));
  node->list_field("body", std::move(body));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_raise_expr() {
  const lexer::Token start = advance();
  std::unique_ptr<ast::Expr> value;
  if (is_stop_token(StopMode::Normal)) {
    error(start, "`raise` requires an exception expression in v1");
    value = ast::make_expr("AstError", start.span);
  } else {
    value = parse_expression(1, StopMode::Normal);
  }
  auto node = ast::make_expr("AstRaise",
                             value != nullptr
                                 ? ast::join_spans(start.span, value->span)
                                 : start.span);
  if (value) {
    node->node_field("expr", std::move(value));
  }
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_try_expr() {
  const lexer::Token start = advance();
  consume(lexer::TokenKind::Colon, "expected ':' after try");
  std::vector<std::unique_ptr<ast::Expr>> body =
      parse_control_body(BodyContext::Def);
  const lexer::Span body_end_span =
      body.empty() ? previous().span : body.back()->span;
  HandlerSuffix handlers = parse_handler_suffix(body_end_span, BodyContext::Def);
  if (handlers.rescues.empty() && !handlers.has_ensure) {
    error_code(start, "E_TRY_WITHOUT_HANDLER",
               "`try` must have at least one `rescue` or `ensure` clause");
  }
  const lexer::Span end_span =
      handlers.end_span.end.offset >= body_end_span.end.offset
          ? handlers.end_span
          : body_end_span;
  auto node = ast::make_expr("AstTry", ast::join_spans(start.span, end_span));
  node->list_field("body", std::move(body));
  node->list_field("rescues", std::move(handlers.rescues));
  if (handlers.has_ensure) {
    node->bool_field("has_ensure", true);
  }
  node->list_field("ensure_body", std::move(handlers.ensure_body));
  return node;
}

std::unique_ptr<ast::Expr> Parser::parse_case_expr(bool strict) {
  const lexer::Token start = advance();
  std::unique_ptr<ast::Expr> scrutinee =
      parse_expression(1, StopMode::ControlHeader);
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
    guard = parse_expression(1, StopMode::ControlHeader);
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
  if (equal_index == tokens_.size() || equal_index == current_ ||
      !token_can_precede_assignment_delimiter(tokens_[equal_index - 1U].kind)) {
    return nullptr;
  }

  // A top-level (depth-0) comma in the left-hand side marks a bare destructuring
  // target list (`a, b = …` / `a, *b = …`). Treat it as a parenthesized tuple
  // pattern so the surrounding parens are optional sugar, lowering `a, *b = e`
  // to `(a, *b) = e`.
  bool lhs_has_top_comma = false;
  for (std::size_t i = current_, depth = 0; i < equal_index; ++i) {
    const lexer::TokenKind k = tokens_[i].kind;
    if (k == lexer::TokenKind::LParen || k == lexer::TokenKind::LBracket ||
        k == lexer::TokenKind::LBrace) {
      ++depth;
    } else if (k == lexer::TokenKind::RParen ||
               k == lexer::TokenKind::RBracket ||
               k == lexer::TokenKind::RBrace) {
      if (depth > 0) {
        --depth;
      }
    } else if (k == lexer::TokenKind::Comma && depth == 0) {
      lhs_has_top_comma = true;
      break;
    }
  }

  if (!lhs_has_top_comma) {
    const std::vector<lexer::Token> left_tokens =
        expression_slice_tokens(tokens_, current_, equal_index);
    Parser left_parser(left_tokens);
    ParseResult left_parse = left_parser.parse_expression_unit();
    if (left_parse.ok() && left_parse.expr != nullptr &&
        (is_assignable(*left_parse.expr) ||
         is_optional_bracket_access(*left_parse.expr))) {
      return nullptr;
    }
  }

  std::string pattern_text =
      pattern_text_from_tokens(tokens_, current_, equal_index);
  if (lhs_has_top_comma) {
    pattern_text = "(" + pattern_text + ")";
  }
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

const lexer::Token &Parser::consume_member_name(const std::string &message) {
  // After a member-access dot, the boolean operator-word keywords are valid
  // method names (Result#or, etc.). They are unambiguous here: a binary
  // `or`/`and`/`not` never legally follows a member-access dot.
  const lexer::TokenKind kind = current().kind;
  if (kind == lexer::TokenKind::KeywordOr ||
      kind == lexer::TokenKind::KeywordAnd ||
      kind == lexer::TokenKind::KeywordNot) {
    return advance();
  }
  return consume_identifier_like(message);
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
  const bool header_mode = stop_mode == StopMode::ControlHeader;

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
                                      !header_mode &&
                                      can_accept_direct_block_suffix(*left))) {
      break;
    }

    if (check(lexer::TokenKind::Dot) || check(lexer::TokenKind::ChainDot) ||
        check(lexer::TokenKind::SafeDot) || check(lexer::TokenKind::LParen) ||
        check(lexer::TokenKind::LBracket) ||
        (!header_mode && check(lexer::TokenKind::Colon)) ||
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
    const char *compound_op = nullptr;
    if (min_precedence <= 1 &&
        compound_assignment_op(current().kind, &compound_op) &&
        peek().kind == lexer::TokenKind::Equal) {
      const lexer::Token op_token = advance();
      advance();
      if (is_optional_bracket_access(*left)) {
        error_code(op_token, "E_OPTIONAL_BRACKET_ASSIGNMENT",
                   "optional bracket access is read-only; use strict "
                   "`receiver[key] = value`");
      } else if (!is_assignable(*left)) {
        error(op_token, "left side of assignment is not assignable");
      }
      std::unique_ptr<ast::Expr> right = parse_expression(1, stop_mode);
      lexer::Span span = ast::join_spans(left->span, right->span);
      auto assign = ast::make_expr("AstAssign", span);
      assign->string_field("op", compound_op);
      assign->node_field("left", std::move(left));
      assign->node_field("right", std::move(right));
      left = std::move(assign);
      continue;
    }

    if (!infix_info(current().kind, &info) ||
        info.precedence < min_precedence) {
      break;
    }
    const lexer::Token op_token = advance();
    const bool range_op = op_token.kind == lexer::TokenKind::DotDot ||
                          op_token.kind == lexer::TokenKind::DotDotDot;
    if (op_token.kind == lexer::TokenKind::Equal) {
      if (is_optional_bracket_access(*left)) {
        error_code(op_token, "E_OPTIONAL_BRACKET_ASSIGNMENT",
                   "optional bracket access is read-only; use strict "
                   "`receiver[key] = value`");
      } else if (!is_assignable(*left)) {
        error(op_token, "left side of assignment is not assignable");
      }
    }
    if (is_chain_comparison_op(info.op)) {
      left = parse_comparison_chain(std::move(left), info, op_token, stop_mode);
      continue;
    }
    const int next_min =
        info.assoc == Assoc::Left ? info.precedence + 1 : info.precedence;
    bool open_ended_range = false;
    std::unique_ptr<ast::Expr> right;
    if (range_op &&
        (check(lexer::TokenKind::Colon) || is_stop_token(stop_mode))) {
      open_ended_range = true;
      if (op_token.kind == lexer::TokenKind::DotDotDot) {
        error_code(op_token, "E_RANGE_EXCLUSIVE_OPEN_ENDED",
                   "exclusive open-ended ranges are not supported");
      }
      right = make_null_literal(current_zero_width_span());
    } else {
      right = parse_expression(next_min, stop_mode);
    }
    const bool range_left_is_float =
        range_op && is_literal_float_expr(*left);
    const bool range_right_is_float =
        range_op && !open_ended_range && is_literal_float_expr(*right);
    lexer::Span span = ast::join_spans(left->span, right->span);
    auto binary = ast::make_expr(
        op_token.kind == lexer::TokenKind::Equal ? "AstAssign" : "AstBinary",
        span);
    binary->string_field("op", info.op);
    if (range_op) {
      binary->bool_field("inclusive_end",
                         op_token.kind == lexer::TokenKind::DotDot);
      if (open_ended_range) {
        binary->bool_field("open_ended", true);
      }
    }
    binary->node_field("left", std::move(left));
    binary->node_field("right", std::move(right));
    if (range_op && stop_mode != StopMode::ControlHeader &&
        match(lexer::TokenKind::Colon)) {
      const lexer::Token colon = previous();
      std::unique_ptr<ast::Expr> step = parse_expression(1, stop_mode);
      if (is_literal_zero_expr(*step)) {
        error_code(colon, "E_RANGE_ZERO_STEP",
                   "range step must not be zero");
      }
      binary->span = ast::join_spans(binary->span, step->span);
      binary->bool_field("has_step", true);
      binary->node_field("step", std::move(step));
    } else if (range_left_is_float || range_right_is_float) {
      error_code(op_token, "E_RANGE_FLOAT_STEP_REQUIRED",
                 "float ranges require an explicit step");
    }
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
  // Template splice hole in a `macro def` body (M4 surface): `#{expr}` is
  // sugar for `unquote(expr)`, `#{*expr}` for `unquote_splice(expr)`. The
  // lexer emits HASH_LBRACE only inside macro-def bodies, so no gating is
  // needed here; the nodes are consumed by the implicit template quote.
  if (token.kind == lexer::TokenKind::HashLBrace) {
    const bool splice = match(lexer::TokenKind::Star);
    std::unique_ptr<ast::Expr> inner = parse_expression(1, StopMode::Normal);
    const lexer::Token close = consume(
        lexer::TokenKind::RBrace, "expected '}' after template splice hole");
    auto node = ast::make_expr(splice ? "AstUnquoteSplice" : "AstUnquote",
                               ast::join_spans(token.span, close.span));
    if (inner) {
      node->node_field("expr", std::move(inner));
    }
    return node;
  }
  if (is_identifier_like_token(token.kind)) {
    // Macro quasiquote surface (macro.v1 profile). `quote:` in block form
    // (a newline must follow the colon) yields an AstQuote whose body is
    // unevaluated template AST. Requiring the newline keeps `{quote: 1}` map
    // keys and inline `quote:`-labelled forms unaffected; `quote` stays an
    // ordinary identifier everywhere else.
    if (token.lexeme == "quote" && check(lexer::TokenKind::Colon) &&
        peek(1).kind == lexer::TokenKind::Newline) {
      advance(); // consume ':'
      ++quote_depth_;
      std::vector<std::unique_ptr<ast::Expr>> body =
          parse_body(BodyContext::Def);
      --quote_depth_;
      const lexer::Span end_span =
          body.empty() ? previous().span : body.back()->span;
      auto node =
          ast::make_expr("AstQuote", ast::join_spans(token.span, end_span));
      node->list_field("body", std::move(body));
      return node;
    }
    // Inside a quote, `unquote(expr)` / `unquote_splice(expr)` are splice holes
    // — they escape the quote and are evaluated at expansion time. Outside any
    // quote they remain ordinary calls (so user code may still name a function
    // `unquote`).
    if (quote_depth_ > 0 &&
        (token.lexeme == "unquote" || token.lexeme == "unquote_splice") &&
        check(lexer::TokenKind::LParen)) {
      const bool splice = token.lexeme == "unquote_splice";
      advance(); // consume '('
      std::unique_ptr<ast::Expr> inner =
          parse_expression(1, StopMode::Normal);
      const lexer::Token close = consume(lexer::TokenKind::RParen,
                                         "expected ')' after unquote argument");
      auto node = ast::make_expr(splice ? "AstUnquoteSplice" : "AstUnquote",
                                 ast::join_spans(token.span, close.span));
      if (inner) {
        node->node_field("expr", std::move(inner));
      }
      return node;
    }
    // `unhygienic(expr)` inside a quote opts the enclosed identifiers out of
    // hygiene so they bind into the caller's context (Elixir `var!`). Consumed
    // by quote lowering into a plain unmarked name; never reaches the binder.
    if (quote_depth_ > 0 && token.lexeme == "unhygienic" &&
        check(lexer::TokenKind::LParen)) {
      advance(); // consume '('
      std::unique_ptr<ast::Expr> inner = parse_expression(1, StopMode::Normal);
      const lexer::Token close = consume(
          lexer::TokenKind::RParen, "expected ')' after unhygienic argument");
      auto node = ast::make_expr("AstUnhygienic",
                                 ast::join_spans(token.span, close.span));
      if (inner) {
        node->node_field("expr", std::move(inner));
      }
      return node;
    }
    const bool typed_map = token.lexeme == "Map" || token.lexeme == "HashMap" ||
                           token.lexeme == "StrictMap" ||
                           token.lexeme == "StrictHashMap";
    const bool typed_set = token.lexeme == "Set" || token.lexeme == "HashSet";
    if ((typed_map || typed_set) && check(lexer::TokenKind::LBrace)) {
      const lexer::Token open = advance();
      if (typed_map && !check(lexer::TokenKind::RBrace) &&
          !starts_map_literal_entry()) {
        error_code(open, "E_COLLECTION_LITERAL_KIND",
                   "Map literal constructor requires key-value entries");
      }
      if (typed_set && starts_map_literal_entry()) {
        error_code(open, "E_COLLECTION_LITERAL_KIND",
                   "Set literal constructor requires elements, not key-value "
                   "entries");
      }
      std::unique_ptr<ast::Expr> expr =
          typed_map ? parse_map_literal(open, stop_mode)
                    : parse_set_literal(open, stop_mode);
      expr->string_field("collection_type", token.lexeme);
      expr->span = ast::join_spans(token.span, expr->span);
      return expr;
    }
    auto expr = ast::make_expr("AstName", token.span);
    expr->string_field("name", token.lexeme);
    return expr;
  }
  if (token.kind == lexer::TokenKind::Pipe) {
    // Standalone lambda literal: `|params|: body` (or `||: body`) in expression
    // position. A leading `|` cannot be infix bitwise-or, so this is
    // unambiguous. Reuses the block parser; lowers to a closure (HClosure)
    // exactly like a call-site block, producing a first-class callable value.
    --current_;
    return parse_block_suffix(stop_mode);
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
  if (token.kind == lexer::TokenKind::KeywordReturn) {
    --current_;
    return parse_return_expr();
  }
  if (token.kind == lexer::TokenKind::KeywordThrow) {
    --current_;
    return parse_throw_expr();
  }
  if (token.kind == lexer::TokenKind::KeywordCatch) {
    --current_;
    return parse_catch_expr();
  }
  if (token.kind == lexer::TokenKind::KeywordRaise) {
    --current_;
    return parse_raise_expr();
  }
  if (token.kind == lexer::TokenKind::KeywordTry) {
    --current_;
    return parse_try_expr();
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
    std::unique_ptr<ast::Expr> operand = parse_expression(10, stop_mode);
    auto expr =
        ast::make_expr("AstUnary", ast::join_spans(token.span, operand->span));
    expr->string_field("op", token.lexeme);
    expr->node_field("operand", std::move(operand));
    return expr;
  }
  if (token.kind == lexer::TokenKind::Star) {
    error_code(token, "E_SPREAD_POSITION",
               "`*` spread is only valid in call arguments and collection "
               "literals");
    auto expr = ast::make_expr("AstError", token.span);
    expr->string_field("token", token.lexeme);
    return expr;
  }
  if (token.kind == lexer::TokenKind::StarStar) {
    error_code(token, "E_KWARG_SPREAD_POSITION",
               "`**` spread is only valid in call arguments and map literals");
    auto expr = ast::make_expr("AstError", token.span);
    expr->string_field("token", token.lexeme);
    return expr;
  }
  if ((token.kind == lexer::TokenKind::Dot ||
       token.kind == lexer::TokenKind::SafeDot) &&
      check(lexer::TokenKind::LParen)) {
    error_code(token, "AMB_DOT_CALL_TARGET",
               "`.()` must follow an expression; it cannot start an "
               "expression");
    auto expr = ast::make_expr("AstError", token.span);
    expr->string_field("token", token.lexeme);
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
  if (is_keyword_spread_start(current(), peek())) {
    const lexer::Token spread = advance();
    error_code(spread, "E_KWARG_SPREAD_POSITION",
               "`**` spread is only valid in call arguments and map literals");
    std::unique_ptr<ast::Expr> value = parse_expression(1, stop_mode);
    auto error_node = ast::make_expr(
        "AstError", value == nullptr ? spread.span
                                     : ast::join_spans(spread.span, value->span));
    error_node->string_field("token", "**");
    return error_node;
  }
  if (match(lexer::TokenKind::Star)) {
    const lexer::Token star = previous();
    std::unique_ptr<ast::Expr> value = parse_expression(1, stop_mode);
    std::unique_ptr<ast::Expr> condition = parse_collection_condition();
    const char *spread_kind = std::string(conditional_kind) == "AstSetElement"
                                  ? "AstSetSpread"
                                  : "AstArraySpread";
    auto spread = ast::make_expr(
        spread_kind,
        condition != nullptr
            ? ast::join_spans(star.span, condition->span)
            : (value == nullptr ? star.span
                                : ast::join_spans(star.span, value->span)));
    spread->node_field("expr", std::move(value));
    if (condition != nullptr) {
      spread->node_field("condition", std::move(condition));
    }
    return spread;
  }
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
    if (is_keyword_spread_start(current(), peek())) {
      const lexer::Token spread_token = advance();
      std::unique_ptr<ast::Expr> value = parse_expression(1, stop_mode);
      std::unique_ptr<ast::Expr> condition = parse_collection_condition();
      auto spread = ast::make_expr(
          "AstMapSpread",
          condition != nullptr
              ? ast::join_spans(spread_token.span, condition->span)
              : (value == nullptr ? spread_token.span
                                  : ast::join_spans(spread_token.span, value->span)));
      spread->node_field("expr", std::move(value));
      if (condition != nullptr) {
        spread->node_field("condition", std::move(condition));
      }
      entries.push_back(std::move(spread));

      if (!match(lexer::TokenKind::Comma)) {
        break;
      }
      if (check(lexer::TokenKind::RBrace)) {
        break;
      }
      continue;
    }

    lexer::Token key = current();
    std::string key_kind = "symbol";
    std::string key_value;
    lexer::Span key_span = key.span;
    std::unique_ptr<ast::Expr> key_expr;
    bool allow_same_name_value = false;

    if (match(lexer::TokenKind::Identifier)) {
      key_value = key.lexeme;
      allow_same_name_value = true;
    } else if (match(lexer::TokenKind::String)) {
      key_kind = "string";
      key_value = key.lexeme;
      key_expr = parse_string_literal_expr(key);
    } else if (match(lexer::TokenKind::Colon)) {
      const lexer::Token name = consume(lexer::TokenKind::Identifier,
                                        "expected symbol key name after ':'");
      key_value = name.lexeme;
      key_span = ast::join_spans(key.span, name.span);
    } else {
      key_kind = "expression";
      key_expr = parse_expression(1, stop_mode);
      if (key_expr == nullptr) {
        error(key, "expected map literal key");
        advance();
        continue;
      }
      key_span = key_expr->span;
    }

    consume(lexer::TokenKind::Colon, "expected ':' after map literal key");
    std::unique_ptr<ast::Expr> value =
        allow_same_name_value &&
                is_same_name_map_value_boundary(current().kind)
            ? make_same_name_value(key)
            : parse_expression(1, stop_mode);
    std::unique_ptr<ast::Expr> condition = parse_collection_condition();
    auto entry = ast::make_expr(
        "AstMapEntry",
        condition != nullptr
            ? ast::join_spans(key_span, condition->span)
            : (value == nullptr ? key_span
                                : ast::join_spans(key_span, value->span)));
    entry->string_field("key_kind", key_kind);
    entry->string_field("key", key_value);
    if (key_expr != nullptr) {
      entry->node_field("key_expr", std::move(key_expr));
    }
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

  // A multiline text block arrives as a `"""`-delimited lexeme whose content
  // is already dedented by the lexer; the same parts scanning applies over
  // the wider content window, and quote_kind records the block form.
  const bool block =
      token.lexeme.size() >= 6U &&
      token.lexeme.compare(0, 3, "\"\"\"") == 0 &&
      token.lexeme.compare(token.lexeme.size() - 3U, 3, "\"\"\"") == 0;
  const std::size_t content_begin = block ? 3U : 1U;
  const std::size_t content_end = token.lexeme.size() - (block ? 3U : 1U);
  std::vector<std::unique_ptr<ast::Expr>> parts;
  std::size_t text_begin = content_begin;
  std::size_t cursor = content_begin;
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
  expr->string_field("quote_kind", block ? "block" : "double");
  expr->bool_field("interpolation", saw_interpolation);
  expr->list_field("parts", std::move(parts));
  return expr;
}

std::unique_ptr<ast::Expr>
Parser::parse_postfix(std::unique_ptr<ast::Expr> expr, StopMode stop_mode) {
  if (check(lexer::TokenKind::Dot) || check(lexer::TokenKind::ChainDot)) {
    const lexer::Token dot = advance();
    if (match(lexer::TokenKind::LParen)) {
      std::vector<std::unique_ptr<ast::Expr>> args =
          parse_call_arg_list(lexer::TokenKind::RParen, stop_mode);
      const lexer::Token close = previous();
      auto tail = ast::make_expr("AstTailDotCall",
                                 ast::join_spans(dot.span, close.span));
      tail->string_field("call_style", "paren");
      tail->bool_field("chain_boundary",
                       dot.kind == lexer::TokenKind::ChainDot);
      tail->list_field("args", std::move(args));
      auto chain = ensure_postfix_chain(std::move(expr));
      append_postfix_tail(*chain, std::move(tail));
      return chain;
    }
    const lexer::Token name = consume_member_name(
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
          parse_call_arg_list(lexer::TokenKind::RParen, stop_mode);
      const lexer::Token close = previous();
      auto tail = ast::make_expr("AstTailSafeCall",
                                 ast::join_spans(dot.span, close.span));
      tail->string_field("call_style", "paren");
      tail->list_field("args", std::move(args));
      auto chain = ensure_postfix_chain(std::move(expr));
      append_postfix_tail(*chain, std::move(tail));
      if (stop_mode != StopMode::ControlHeader &&
          (check(lexer::TokenKind::Colon) || check(lexer::TokenKind::Pipe))) {
        auto block = parse_block_suffix(stop_mode);
        auto block_tail = ast::make_expr("AstTailBlockSuffix", block->span);
        block_tail->node_field("block", std::move(block));
        append_postfix_tail(*chain, std::move(block_tail));
      }
      return chain;
    }
    if (match(lexer::TokenKind::LBracket)) {
      const bool optional = match(lexer::TokenKind::Question);
      std::unique_ptr<ast::Expr> index = parse_expression(1, stop_mode);
      const lexer::Token close =
          consume(lexer::TokenKind::RBracket, "expected ']' after index");
      auto tail = ast::make_expr("AstTailSafeIndex",
                                 ast::join_spans(dot.span, close.span));
      if (optional) {
        tail->bool_field("optional", true);
      }
      tail->node_field("index_expr", std::move(index));
      auto chain = ensure_postfix_chain(std::move(expr));
      append_postfix_tail(*chain, std::move(tail));
      return chain;
    }
    const lexer::Token name = consume_member_name(
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
        parse_call_arg_list(lexer::TokenKind::RParen, stop_mode);
    const lexer::Token close = previous();
    auto tail =
        ast::make_expr("AstTailCall", ast::join_spans(open.span, close.span));
    tail->string_field("call_style", "paren");
    tail->list_field("args", std::move(args));
    auto chain = ensure_postfix_chain(std::move(expr));
    append_postfix_tail(*chain, std::move(tail));
    if (stop_mode != StopMode::ControlHeader &&
        (check(lexer::TokenKind::Colon) || check(lexer::TokenKind::Pipe))) {
      auto block = parse_block_suffix(stop_mode);
      auto block_tail = ast::make_expr("AstTailBlockSuffix", block->span);
      block_tail->node_field("block", std::move(block));
      append_postfix_tail(*chain, std::move(block_tail));
    }
    return chain;
  }
  if (match(lexer::TokenKind::LBracket)) {
    const lexer::Token open = previous();
    const bool optional = match(lexer::TokenKind::Question);
    std::unique_ptr<ast::Expr> index = parse_expression(1, stop_mode);
    const lexer::Token close =
        consume(lexer::TokenKind::RBracket, "expected ']' after index");
    auto tail =
        ast::make_expr("AstTailIndex", ast::join_spans(open.span, close.span));
    if (optional) {
      tail->bool_field("optional", true);
    }
    tail->node_field("index_expr", std::move(index));
    auto chain = ensure_postfix_chain(std::move(expr));
    append_postfix_tail(*chain, std::move(tail));
    return chain;
  }
  if (stop_mode != StopMode::ControlHeader &&
      (check(lexer::TokenKind::Colon) || check(lexer::TokenKind::Pipe)) &&
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
    if (stop_mode != StopMode::ControlHeader &&
        (check(lexer::TokenKind::Colon) || check(lexer::TokenKind::Pipe))) {
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

  if (match(lexer::TokenKind::Newline)) {
    std::vector<std::unique_ptr<ast::Expr>> body;
    consume(lexer::TokenKind::Indent, "expected indented block suffix body");
    while (!at_end() && !check(lexer::TokenKind::Dedent)) {
      std::unique_ptr<ast::Expr> item = parse_statement(BodyContext::Def);
      if (item) {
        append_item_or_merge_clause_def(&body, std::move(item));
      }
      while (match(lexer::TokenKind::Newline)) {
      }
    }
    consume(lexer::TokenKind::Dedent, "expected block suffix dedent");
    lexer::Span end_span = previous().span;
    if (!body.empty()) {
      end_span = body.back()->span;
    }
    auto block =
        ast::make_expr("AstBlock", ast::join_spans(start_span, end_span));
    block->list_field("params", std::move(params));
    block->list_field("body", std::move(body));
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
  return parse_call_arg_list(lexer::TokenKind::RParen, stop_mode);
}

std::unique_ptr<ast::Expr>
Parser::parse_call_arg(StopMode stop_mode, lexer::TokenKind closing_kind) {
  // RFC block-parameters §6: a trailing `&name` argument routes the local's
  // callable into the callee's block channel. `&local` is not otherwise a
  // legal expression (§4.6 restricts prefix `&` to static reference targets),
  // so this is a conflict-free slot. v1 accepts only a bare local name.
  if (check(lexer::TokenKind::Ampersand)) {
    if (peek(1).kind == lexer::TokenKind::Identifier &&
        (peek(2).kind == lexer::TokenKind::Comma ||
         peek(2).kind == lexer::TokenKind::RParen)) {
      const lexer::Token amp = advance();
      const lexer::Token name = advance();
      auto value = ast::make_expr("AstName", name.span);
      value->string_field("name", name.lexeme);
      auto arg = ast::make_expr("AstBlockPass",
                                ast::join_spans(amp.span, name.span));
      arg->string_field("name", name.lexeme);
      arg->node_field("value", std::move(value));
      return arg;
    }
    const lexer::Token amp = advance();
    error_code(amp, "AMB_BLOCK_PASS_TARGET",
               "v1 block-pass accepts only a bare local name (`&name`); bind a "
               "callable reference first");
    auto expr = ast::make_expr("AstError", amp.span);
    expr->string_field("token", amp.lexeme);
    return expr;
  }
  if (is_keyword_spread_start(current(), peek())) {
    const lexer::Token spread = advance();
    std::unique_ptr<ast::Expr> value = parse_expression(1, stop_mode);
    auto arg = ast::make_expr(
        "AstKeywordSpreadArg",
        value == nullptr ? spread.span
                         : ast::join_spans(spread.span, value->span));
    arg->node_field("expr", std::move(value));
    return arg;
  }
  if (match(lexer::TokenKind::Star)) {
    const lexer::Token star = previous();
    std::unique_ptr<ast::Expr> value = parse_expression(1, stop_mode);
    auto arg = ast::make_expr(
        "AstSpreadArg",
        value == nullptr ? star.span : ast::join_spans(star.span, value->span));
    arg->node_field("expr", std::move(value));
    return arg;
  }
  if (check(lexer::TokenKind::Identifier) &&
      peek().kind == lexer::TokenKind::Colon) {
    const lexer::Token name = advance();
    advance();
    std::unique_ptr<ast::Expr> value =
        is_same_name_call_value_boundary(current().kind, closing_kind)
            ? make_same_name_value(name)
            : parse_expression(1, stop_mode);
    auto keyword = ast::make_expr("AstKeywordArg",
                                  ast::join_spans(name.span, value->span));
    keyword_arg_field(*keyword, name);
    keyword->node_field("value", std::move(value));
    return keyword;
  }
  return parse_expression(1, stop_mode);
}

std::vector<std::unique_ptr<ast::Expr>>
Parser::parse_call_arg_list(lexer::TokenKind closing_kind,
                            StopMode stop_mode) {
  std::vector<std::unique_ptr<ast::Expr>> values;
  if (match(closing_kind)) {
    return values;
  }

  bool saw_keyword = false;
  bool saw_keyword_spread = false;
  bool saw_block_pass = false;
  while (!check(closing_kind) && !at_end()) {
    if (saw_block_pass) {
      error_code(current(), "AMB_BLOCK_PASS_TARGET",
                 "a `&name` block-pass argument must be the last argument");
    }
    std::unique_ptr<ast::Expr> arg = parse_call_arg(stop_mode, closing_kind);
    if (arg != nullptr && arg->kind == "AstBlockPass") {
      saw_block_pass = true;
    }
    const bool positional =
        arg != nullptr &&
        (arg->kind != "AstKeywordArg" && arg->kind != "AstKeywordSpreadArg");
    const bool keyword =
        arg != nullptr &&
        (arg->kind == "AstKeywordArg" || arg->kind == "AstKeywordSpreadArg");
    if (positional && saw_keyword) {
      error_code(current(), "E_ARGUMENT_ORDER",
                 "positional arguments and `*` spreads must appear before "
                 "keyword arguments and `**` spreads");
    }
    if (arg != nullptr && arg->kind == "AstKeywordArg" &&
        saw_keyword_spread) {
      error_code(current(), "E_ARGUMENT_ORDER",
                 "ordinary keyword arguments must appear before `**` keyword "
                 "spread");
    }
    if (keyword) {
      saw_keyword = true;
    }
    if (arg != nullptr && arg->kind == "AstKeywordSpreadArg") {
      saw_keyword_spread = true;
    }
    values.push_back(std::move(arg));
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

std::vector<std::unique_ptr<ast::Expr>>
Parser::parse_bare_args(StopMode stop_mode) {
  std::vector<std::unique_ptr<ast::Expr>> args;
  while (starts_bare_arg()) {
    if (check(lexer::TokenKind::Identifier) &&
        peek().kind == lexer::TokenKind::Colon) {
      const lexer::Token name = advance();
      advance();
      std::unique_ptr<ast::Expr> value =
          is_same_name_bare_arg_value_boundary(current().kind)
              ? make_same_name_value(name)
              : parse_expression(1, stop_mode);
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
      std::unique_ptr<ast::Expr> value =
          is_same_name_call_value_boundary(current().kind, closing_kind)
              ? make_same_name_value(name)
              : parse_expression(1, stop_mode);
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
  case lexer::TokenKind::KeywordCatch:
  case lexer::TokenKind::KeywordThrow:
  case lexer::TokenKind::KeywordRaise:
  case lexer::TokenKind::KeywordTry:
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
  case lexer::TokenKind::KeywordCatch:
  case lexer::TokenKind::KeywordThrow:
  case lexer::TokenKind::KeywordRaise:
  case lexer::TokenKind::KeywordTry:
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
  if (is_keyword_spread_start(current(), peek())) {
    return true;
  }
  if (current().kind == lexer::TokenKind::Identifier ||
      current().kind == lexer::TokenKind::String ||
      current().kind == lexer::TokenKind::Integer ||
      current().kind == lexer::TokenKind::Float ||
      current().kind == lexer::TokenKind::KeywordTrue ||
      current().kind == lexer::TokenKind::KeywordFalse ||
      current().kind == lexer::TokenKind::KeywordNull) {
    return peek().kind == lexer::TokenKind::Colon;
  }
  if (current().kind == lexer::TokenKind::Colon &&
      peek().kind == lexer::TokenKind::Identifier) {
    return peek(2).kind == lexer::TokenKind::Colon;
  }
  if (current().kind == lexer::TokenKind::LParen ||
      current().kind == lexer::TokenKind::LBracket) {
    int depth = 0;
    for (std::size_t index = current_; index < tokens_.size(); ++index) {
      const lexer::TokenKind kind = tokens_[index].kind;
      if (kind == lexer::TokenKind::LParen ||
          kind == lexer::TokenKind::LBracket ||
          kind == lexer::TokenKind::LBrace) {
        ++depth;
        continue;
      }
      if (kind == lexer::TokenKind::RParen ||
          kind == lexer::TokenKind::RBracket ||
          kind == lexer::TokenKind::RBrace) {
        if (depth == 0) {
          return false;
        }
        --depth;
        continue;
      }
      if (depth == 0 && kind == lexer::TokenKind::Colon) {
        return true;
      }
      if (depth == 0 &&
          (kind == lexer::TokenKind::Comma ||
           kind == lexer::TokenKind::Newline ||
           kind == lexer::TokenKind::Eof)) {
        return false;
      }
    }
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
  // `unhygienic(target)` inside a quote is an assignable macro-hygiene escape
  // hatch; quote lowering rewrites it to a plain assignable name. A `#{a}` /
  // `unquote(a)` splice hole is assignable for the same reason: expansion
  // substitutes an assignable target (`swap`-style macros assign through it).
  if (expr.kind == "AstUnhygienic" || expr.kind == "AstUnquote") {
    return true;
  }
  const ast::Expr *tail = last_postfix_tail(expr);
  if (tail == nullptr) {
    return false;
  }
  return tail->kind == "AstTailDotMember" ||
         (tail->kind == "AstTailIndex" && !bool_value(*tail, "optional"));
}

bool Parser::is_optional_bracket_access(const ast::Expr &expr) const {
  const ast::Expr *tail = last_postfix_tail(expr);
  return tail != nullptr && tail->kind == "AstTailIndex" &&
         bool_value(*tail, "optional");
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
  case lexer::TokenKind::DotDotDot:
    *info = InfixInfo{5, Assoc::Left, "..."};
    return true;
  case lexer::TokenKind::Pipe:
    *info = InfixInfo{5, Assoc::Left, "|"};
    return true;
  case lexer::TokenKind::Caret:
    *info = InfixInfo{6, Assoc::Left, "^"};
    return true;
  case lexer::TokenKind::Ampersand:
    *info = InfixInfo{7, Assoc::Left, "&"};
    return true;
  case lexer::TokenKind::Plus:
    *info = InfixInfo{8, Assoc::Left, "+"};
    return true;
  case lexer::TokenKind::Minus:
    *info = InfixInfo{8, Assoc::Left, "-"};
    return true;
  case lexer::TokenKind::LessLess:
    *info = InfixInfo{8, Assoc::Left, "<<"};
    return true;
  case lexer::TokenKind::GreaterGreater:
    *info = InfixInfo{8, Assoc::Left, ">>"};
    return true;
  case lexer::TokenKind::Star:
    *info = InfixInfo{9, Assoc::Left, "*"};
    return true;
  case lexer::TokenKind::Slash:
    *info = InfixInfo{9, Assoc::Left, "/"};
    return true;
  case lexer::TokenKind::SlashSlash:
    *info = InfixInfo{9, Assoc::Left, "//"};
    return true;
  case lexer::TokenKind::Percent:
    *info = InfixInfo{9, Assoc::Left, "%"};
    return true;
  case lexer::TokenKind::StarStar:
    *info = InfixInfo{10, Assoc::Right, "**"};
    return true;
  default:
    return false;
  }
}

bool Parser::compound_assignment_op(lexer::TokenKind kind,
                                    const char **op) const {
  switch (kind) {
  case lexer::TokenKind::Plus:
    *op = "+=";
    return true;
  case lexer::TokenKind::Minus:
    *op = "-=";
    return true;
  case lexer::TokenKind::Star:
    *op = "*=";
    return true;
  case lexer::TokenKind::Slash:
    *op = "/=";
    return true;
  case lexer::TokenKind::SlashSlash:
    *op = "//=";
    return true;
  case lexer::TokenKind::Percent:
    *op = "%=";
    return true;
  default:
    return false;
  }
}

bool Parser::is_method_name_token(const lexer::Token &token) const {
  return token.kind == lexer::TokenKind::Identifier ||
         is_operator_method_name(token.kind);
}

bool Parser::is_literal_float_expr(const ast::Expr &expr) const {
  return expr.kind == "AstLiteral" && string_value(expr, "token") == "FLOAT";
}

bool Parser::is_literal_zero_expr(const ast::Expr &expr) const {
  if (expr.kind != "AstLiteral") {
    return false;
  }
  const std::string token = string_value(expr, "token");
  if (token != "INTEGER" && token != "FLOAT") {
    return false;
  }
  const std::string value = string_value(expr, "value");
  for (char c : value) {
    if (c != '0' && c != '.' && c != '_') {
      return false;
    }
  }
  return true;
}

std::unique_ptr<ast::Expr>
Parser::make_null_literal(const lexer::Span &span) const {
  auto expr = ast::make_expr("AstLiteral", span);
  expr->string_field("token", "KEYWORD_NULL");
  expr->string_field("value", "null");
  return expr;
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
