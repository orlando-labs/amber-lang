#include "frontend/parser/parser.h"
#include "frontend/lexer/lexer.h"
#include "frontend/pattern/pattern.h"

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
  return kind == lexer::TokenKind::EqualEqual ||
         kind == lexer::TokenKind::EqualEqualEqual;
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
  case lexer::TokenKind::KeywordClassMethod: {
    const lexer::Token start = advance();
    consume(lexer::TokenKind::KeywordDef, "expected 'def' after class_method");
    return parse_def_stmt(true, &start);
  }
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

std::unique_ptr<ast::Expr> Parser::parse_if_expr() {
  const lexer::Token start = advance();
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
      else_body.push_back(parse_if_expr());
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
  if (check(lexer::TokenKind::Identifier) ||
      is_operator_method_name(current().kind)) {
    return advance().lexeme;
  }
  error(current(), message);
  return current().lexeme;
}

std::string Parser::consume_identifier_text(const std::string &message) {
  const lexer::Token token = consume(lexer::TokenKind::Identifier, message);
  return token.lexeme;
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

  while (true) {
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

    InfixInfo info{};
    if (!infix_info(current().kind, &info) ||
        info.precedence < min_precedence) {
      break;
    }
    const lexer::Token op_token = advance();
    if (op_token.kind == lexer::TokenKind::Equal && !is_assignable(*left)) {
      error(op_token, "left side of assignment is not assignable");
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

  return left;
}

std::unique_ptr<ast::Expr> Parser::parse_prefix(StopMode stop_mode) {
  const lexer::Token token = advance();
  if (token.kind == lexer::TokenKind::Identifier) {
    auto expr = ast::make_expr("AstName", token.span);
    expr->string_field("name", token.lexeme);
    return expr;
  }
  if (token.kind == lexer::TokenKind::KeywordIf) {
    --current_;
    return parse_if_expr();
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
    std::unique_ptr<ast::Expr> inner = parse_expression(1, stop_mode);
    const lexer::Token close =
        consume(lexer::TokenKind::RParen, "expected ')' after expression");
    auto expr =
        ast::make_expr("AstGroup", ast::join_spans(token.span, close.span));
    expr->node_field("expr", std::move(inner));
    return expr;
  }
  if (token.kind == lexer::TokenKind::LBracket) {
    std::vector<std::unique_ptr<ast::Expr>> elements =
        parse_expr_list(lexer::TokenKind::RBracket, stop_mode);
    const lexer::Token close = previous();
    auto expr = ast::make_expr("AstListLiteral",
                               ast::join_spans(token.span, close.span));
    expr->list_field("elements", std::move(elements));
    return expr;
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
  std::size_t literal_begin = 1U;
  std::size_t cursor = 1U;
  bool saw_interpolation = false;

  auto push_literal_part = [&](std::size_t begin, std::size_t end) {
    if (end <= begin) {
      return;
    }
    auto part =
        ast::make_expr("AstInterpolationLiteral",
                       span_for_token_slice(token, begin, end));
    part->string_field("token", "STRING");
    part->string_field("value",
                       "\"" + token.lexeme.substr(begin, end - begin) + "\"");
    parts.push_back(std::move(part));
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
      cursor += 2;
      continue;
    }
    if (token.lexeme[cursor] == '#' && cursor + 1U < content_end &&
        token.lexeme[cursor + 1U] == '{') {
      saw_interpolation = true;
      push_literal_part(literal_begin, cursor);
      const std::size_t expr_begin = cursor + 2U;
      std::size_t expr_end = expr_begin;
      if (!find_interpolation_end(expr_begin, &expr_end)) {
        error(token, "unterminated string interpolation");
        return literal();
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
        return literal();
      }
      Parser nested(lex_result.tokens);
      ParseResult parsed = nested.parse_expression_unit();
      if (!parsed.ok()) {
        for (lexer::Diagnostic diagnostic : parsed.diagnostics) {
          diagnostics_.push_back(std::move(diagnostic));
        }
        return literal();
      }
      auto part =
          ast::make_expr("AstInterpolationExpr",
                         span_for_token_slice(token, cursor, expr_end + 1U));
      part->node_field("expr", std::move(parsed.expr));
      parts.push_back(std::move(part));
      cursor = expr_end + 1U;
      literal_begin = cursor;
      continue;
    }
    ++cursor;
  }

  if (!saw_interpolation) {
    return literal();
  }
  push_literal_part(literal_begin, content_end);

  auto expr = ast::make_expr("AstInterpolatedString", token.span);
  expr->list_field("parts", std::move(parts));
  return expr;
}

std::unique_ptr<ast::Expr>
Parser::parse_postfix(std::unique_ptr<ast::Expr> expr, StopMode stop_mode) {
  if (check(lexer::TokenKind::Dot) || check(lexer::TokenKind::ChainDot)) {
    const lexer::Token dot = advance();
    const lexer::Token name =
        consume(lexer::TokenKind::Identifier, "expected method or field name");
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
    const lexer::Token name =
        consume(lexer::TokenKind::Identifier, "expected method or field name");
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
  default:
    return false;
  }
}

bool Parser::starts_primary() const {
  switch (current().kind) {
  case lexer::TokenKind::Identifier:
  case lexer::TokenKind::Placeholder:
  case lexer::TokenKind::LastValue:
  case lexer::TokenKind::Integer:
  case lexer::TokenKind::Float:
  case lexer::TokenKind::String:
  case lexer::TokenKind::KeywordTrue:
  case lexer::TokenKind::KeywordFalse:
  case lexer::TokenKind::KeywordNull:
  case lexer::TokenKind::At:
  case lexer::TokenKind::AtAt:
  case lexer::TokenKind::LParen:
  case lexer::TokenKind::LBracket:
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
  case lexer::TokenKind::Placeholder:
  case lexer::TokenKind::LastValue:
  case lexer::TokenKind::Integer:
  case lexer::TokenKind::Float:
  case lexer::TokenKind::String:
  case lexer::TokenKind::KeywordTrue:
  case lexer::TokenKind::KeywordFalse:
  case lexer::TokenKind::KeywordNull:
  case lexer::TokenKind::At:
  case lexer::TokenKind::AtAt:
  case lexer::TokenKind::LParen:
  case lexer::TokenKind::LBracket:
    return true;
  default:
    return false;
  }
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
