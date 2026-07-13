#include "frontend/pattern/pattern.h"

#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace amber::pattern {
namespace {

using Node = ast::Expr;

std::unique_ptr<Node> make_node(const std::string &kind,
                                const lexer::Span &span) {
  return ast::make_expr(kind, span);
}

std::string string_value(const ast::Expr &expr, const std::string &name) {
  for (const ast::StringField &field : expr.string_fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  return "";
}

const ast::Expr *node_field(const ast::Expr &expr, const std::string &name) {
  for (const ast::NodeField &field : expr.node_fields) {
    if (field.name == name) {
      return field.value.get();
    }
  }
  return nullptr;
}

const ast::ListField *list_field(const ast::Expr &expr,
                                 const std::string &name) {
  for (const ast::ListField &field : expr.list_fields) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

bool is_layout_token(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::Newline ||
         kind == lexer::TokenKind::Indent || kind == lexer::TokenKind::Dedent ||
         kind == lexer::TokenKind::Eof;
}

bool is_ascii_uppercase_identifier(const std::string &text) {
  return !text.empty() &&
         std::isupper(static_cast<unsigned char>(text.front())) != 0;
}

std::string trim_ascii(const std::string &value) {
  const std::size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const std::size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string sequence_rest_binding_name(const std::string &rest_mode) {
  static const std::string prefix = "bind_rest(";
  if (rest_mode.size() <= prefix.size() + 1 ||
      rest_mode.compare(0, prefix.size(), prefix) != 0 ||
      rest_mode.back() != ')') {
    return "";
  }
  return rest_mode.substr(prefix.size(), rest_mode.size() - prefix.size() - 1);
}

bool is_ignore_sequence_rest_mode(const std::string &rest_mode) {
  return rest_mode == "ignore_rest";
}

std::string normalized_sequence_rest_kind(const std::string &rest_mode) {
  if (is_ignore_sequence_rest_mode(rest_mode)) {
    return "ignore";
  }
  if (!sequence_rest_binding_name(rest_mode).empty()) {
    return "capture";
  }
  return "none";
}

std::string map_rest_binding_name(const std::string &rest_mode) {
  static const std::string prefix = "bind_rest(";
  if (rest_mode.size() <= prefix.size() + 1 ||
      rest_mode.compare(0, prefix.size(), prefix) != 0 ||
      rest_mode.back() != ')') {
    return "";
  }
  return rest_mode.substr(prefix.size(), rest_mode.size() - prefix.size() - 1);
}

bool is_strict_null_rest_mode(const std::string &rest_mode) {
  return rest_mode == "strict_null";
}

bool is_ignore_rest_mode(const std::string &rest_mode) {
  return rest_mode == "ignore_rest";
}

std::string normalized_rest_kind(const std::string &rest_mode) {
  if (is_strict_null_rest_mode(rest_mode)) {
    return "strict_null";
  }
  if (is_ignore_rest_mode(rest_mode)) {
    return "ignore";
  }
  if (!map_rest_binding_name(rest_mode).empty()) {
    return "capture";
  }
  return "extra_ok";
}

std::vector<std::string>
collect_named_fields(const ast::Expr &pattern,
                     const std::string &field_list_name) {
  std::vector<std::string> names;
  if (const ast::ListField *list = list_field(pattern, field_list_name)) {
    for (const std::unique_ptr<ast::Expr> &field : list->values) {
      const std::string name = string_value(*field, "name");
      if (!name.empty()) {
        names.push_back(name);
      }
    }
  }
  return names;
}

void add_requested_keys(ast::Expr &node, const std::vector<std::string> &names,
                        const lexer::Span &span) {
  if (names.empty()) {
    return;
  }
  std::vector<std::unique_ptr<Node>> values;
  values.reserve(names.size());
  for (const std::string &name : names) {
    auto key = make_node("PIrRequestedKey", span);
    key->string_field("name", name);
    values.push_back(std::move(key));
  }
  node.list_field("requested_keys", std::move(values));
}

void add_named_entries(ast::Expr &node, const std::string &field_name,
                       const std::string &entry_kind,
                       const std::vector<std::string> &names,
                       const lexer::Span &span) {
  if (names.empty()) {
    return;
  }
  std::vector<std::unique_ptr<Node>> values;
  values.reserve(names.size());
  for (const std::string &name : names) {
    auto entry = make_node(entry_kind, span);
    entry->string_field("name", name);
    values.push_back(std::move(entry));
  }
  node.list_field(field_name, std::move(values));
}

std::string head_destructure_mode(const ast::Expr &pattern) {
  const ast::ListField *pos_args = list_field(pattern, "pos_args");
  const ast::ListField *kw_fields = list_field(pattern, "kw_fields");
  const bool has_positional = pos_args != nullptr && !pos_args->values.empty();
  const bool has_keywords = kw_fields != nullptr && !kw_fields->values.empty();
  if (has_positional && has_keywords) {
    return "MIXED";
  }
  if (has_positional) {
    return "POSITIONAL";
  }
  if (has_keywords) {
    return "KEYS";
  }
  return "HEAD_ONLY";
}

bool context_allows_bare_matcher(PatternContext context) {
  return context == PatternContext::Case;
}

bool context_allows_dynamic_pattern(PatternContext context) {
  return context == PatternContext::General ||
         context == PatternContext::Clause || context == PatternContext::Case;
}

class Parser {
public:
  Parser(const std::string &text, const lexer::Span &span)
      : text_(text), span_(span) {
    lexer::Lexer lexer(text, span.file.empty() ? "<pattern>" : span.file);
    lexer::LexResult result = lexer.lex();
    if (!result.ok()) {
      return;
    }
    for (const lexer::Token &token : result.tokens) {
      if (!is_layout_token(token.kind)) {
        tokens_.push_back(token);
      }
    }
  }

  std::unique_ptr<Node> parse() {
    if (tokens_.empty()) {
      return make_matcher_expr(text_);
    }
    std::unique_ptr<Node> pattern = parse_pattern();
    if (pattern == nullptr || !at_end()) {
      return make_matcher_expr(text_);
    }
    return pattern;
  }

private:
  const std::string &text_;
  lexer::Span span_;
  std::vector<lexer::Token> tokens_;
  std::size_t index_ = 0;

  bool at_end() const { return index_ >= tokens_.size(); }

  const lexer::Token *peek(std::size_t distance = 0) const {
    const std::size_t target = index_ + distance;
    return target < tokens_.size() ? &tokens_[target] : nullptr;
  }

  bool match(lexer::TokenKind kind) {
    const lexer::Token *token = peek();
    if (token == nullptr || token->kind != kind) {
      return false;
    }
    ++index_;
    return true;
  }

  bool match_identifier_text(const std::string &text) {
    const lexer::Token *token = peek();
    if (token == nullptr || token->kind != lexer::TokenKind::Identifier ||
        token->lexeme != text) {
      return false;
    }
    ++index_;
    return true;
  }

  std::unique_ptr<Node> parse_pattern() { return parse_or_pattern(); }

  std::unique_ptr<Node> parse_or_pattern() {
    std::vector<std::unique_ptr<Node>> alternatives;
    std::unique_ptr<Node> first = parse_as_pattern();
    if (first == nullptr) {
      return nullptr;
    }
    alternatives.push_back(std::move(first));
    while (match(lexer::TokenKind::Pipe)) {
      std::unique_ptr<Node> alt = parse_as_pattern();
      if (alt == nullptr) {
        return nullptr;
      }
      alternatives.push_back(std::move(alt));
    }
    if (alternatives.size() == 1) {
      return std::move(alternatives.front());
    }
    auto node = make_node("PatOr", span_);
    node->list_field("alternatives", std::move(alternatives));
    return node;
  }

  std::unique_ptr<Node> parse_as_pattern() {
    std::unique_ptr<Node> base = parse_term_pattern();
    if (base == nullptr) {
      return nullptr;
    }
    if (base->kind == "PatBind" && match_identifier_text("as")) {
      auto node = make_node("PatAs", span_);
      node->string_field("bind_name", string_value(*base, "name"));
      std::unique_ptr<Node> inner = parse_term_pattern();
      if (inner == nullptr) {
        return nullptr;
      }
      node->node_field("inner", std::move(inner));
      return node;
    }
    return base;
  }

  std::unique_ptr<Node> parse_term_pattern() {
    if (const lexer::Token *token = peek()) {
      if (token->kind == lexer::TokenKind::LParen) {
        return parse_tuple();
      }
      if (token->kind == lexer::TokenKind::LBracket) {
        return parse_list();
      }
      if (token->kind == lexer::TokenKind::LBrace) {
        return parse_map();
      }
      if (token->kind == lexer::TokenKind::Caret) {
        return parse_pin();
      }
    }
    std::unique_ptr<Node> base = parse_primary();
    if (base == nullptr) {
      return nullptr;
    }
    if (base->kind == "PatHeadSeed" && match(lexer::TokenKind::LParen)) {
      return finish_head(std::move(base));
    }
    if (base->kind == "PatHeadSeed") {
      return make_matcher_expr(text_);
    }
    return base;
  }

  std::unique_ptr<Node> parse_primary() {
    const lexer::Token *token = peek();
    if (token == nullptr) {
      return nullptr;
    }

    if ((token->kind == lexer::TokenKind::Minus ||
         token->kind == lexer::TokenKind::Plus) &&
        peek(1) != nullptr &&
        (peek(1)->kind == lexer::TokenKind::Integer ||
         peek(1)->kind == lexer::TokenKind::Float)) {
      const std::string sign = token->lexeme;
      const lexer::Token number = *peek(1);
      index_ += 2;
      auto node = make_node("PatLiteral", span_);
      node->string_field("token", number.kind == lexer::TokenKind::Integer
                                      ? "INTEGER"
                                      : "FLOAT");
      node->string_field("value", sign + number.lexeme);
      return node;
    }

    if (token->kind == lexer::TokenKind::Integer ||
        token->kind == lexer::TokenKind::Float ||
        token->kind == lexer::TokenKind::String ||
        token->kind == lexer::TokenKind::KeywordTrue ||
        token->kind == lexer::TokenKind::KeywordFalse ||
        token->kind == lexer::TokenKind::KeywordNull) {
      ++index_;
      auto node = make_node("PatLiteral", span_);
      if (token->kind == lexer::TokenKind::Integer) {
        node->string_field("token", "INTEGER");
      } else if (token->kind == lexer::TokenKind::Float) {
        node->string_field("token", "FLOAT");
      } else if (token->kind == lexer::TokenKind::String) {
        node->string_field("token", "STRING");
      } else if (token->kind == lexer::TokenKind::KeywordTrue) {
        node->string_field("token", "KEYWORD_TRUE");
      } else if (token->kind == lexer::TokenKind::KeywordFalse) {
        node->string_field("token", "KEYWORD_FALSE");
      } else {
        node->string_field("token", "KEYWORD_NULL");
      }
      node->string_field("value", token->lexeme);
      return node;
    }

    if (token->kind == lexer::TokenKind::Colon && peek(1) != nullptr &&
        peek(1)->kind == lexer::TokenKind::Identifier) {
      const lexer::Token symbol = *peek(1);
      index_ += 2;
      auto node = make_node("PatLiteral", span_);
      node->string_field("token", "SYMBOL");
      node->string_field("value", symbol.lexeme);
      return node;
    }

    if (token->kind != lexer::TokenKind::Identifier) {
      return nullptr;
    }

    std::vector<std::string> path;
    path.push_back(token->lexeme);
    ++index_;
    while (match(lexer::TokenKind::Dot)) {
      const lexer::Token *segment = peek();
      if (segment == nullptr || segment->kind != lexer::TokenKind::Identifier) {
        return nullptr;
      }
      path.push_back(segment->lexeme);
      ++index_;
    }

    std::string path_text = path.front();
    for (std::size_t i = 1; i < path.size(); ++i) {
      path_text += ".";
      path_text += path[i];
    }

    if (path_text == "pattern" && peek() != nullptr &&
        peek()->kind == lexer::TokenKind::LParen) {
      return parse_dynamic_pattern();
    }
    if (peek() != nullptr && peek()->kind == lexer::TokenKind::LParen) {
      auto node = make_node("PatHeadSeed", span_);
      node->string_field("head", path_text);
      return node;
    }
    if (is_ascii_uppercase_identifier(path.back())) {
      auto node = make_node("PatConst", span_);
      node->string_field("path", path_text);
      return node;
    }
    if (path.size() != 1) {
      return make_matcher_expr(text_);
    }
    if (path_text == "_") {
      return make_node("PatIgnore", span_);
    }
    auto node = make_node("PatBind", span_);
    node->string_field("name", path_text);
    return node;
  }

  std::unique_ptr<Node> parse_pin() {
    if (!match(lexer::TokenKind::Caret)) {
      return nullptr;
    }
    const lexer::Token *name = peek();
    if (name == nullptr || name->kind != lexer::TokenKind::Identifier) {
      return nullptr;
    }
    ++index_;
    auto node = make_node("PatPin", span_);
    node->string_field("name", name->lexeme);
    return node;
  }

  std::unique_ptr<Node> parse_dynamic_pattern() {
    const lexer::Token *lparen = peek();
    if (lparen == nullptr || lparen->kind != lexer::TokenKind::LParen) {
      return nullptr;
    }
    ++index_;

    const std::size_t matcher_start = lparen->span.end.offset;
    int paren_depth = 1;
    const lexer::Token *rparen = nullptr;
    while (!at_end()) {
      const lexer::Token *token = peek();
      if (token->kind == lexer::TokenKind::LParen) {
        ++paren_depth;
      } else if (token->kind == lexer::TokenKind::RParen) {
        --paren_depth;
        if (paren_depth == 0) {
          rparen = token;
          ++index_;
          break;
        }
      }
      ++index_;
    }
    if (rparen == nullptr) {
      return nullptr;
    }

    auto node = make_node("PatDynamic", span_);
    node->string_field(
        "matcher_text",
        trim_ascii(text_.substr(matcher_start,
                                rparen->span.start.offset - matcher_start)));
    if (match_identifier_text("with")) {
      std::unique_ptr<Node> export_map_pattern = parse_map();
      if (export_map_pattern == nullptr) {
        return nullptr;
      }
      node->node_field("export_map_pattern", std::move(export_map_pattern));
    }
    return node;
  }

  std::unique_ptr<Node> finish_head(std::unique_ptr<Node> head_seed) {
    auto node = make_node("PatHead", span_);
    node->string_field("head", string_value(*head_seed, "head"));

    std::vector<std::unique_ptr<Node>> pos_args;
    std::vector<std::unique_ptr<Node>> kw_fields;
    if (match(lexer::TokenKind::RParen)) {
      node->list_field("pos_args", std::move(pos_args));
      node->list_field("kw_fields", std::move(kw_fields));
      return node;
    }

    while (true) {
      if (peek() != nullptr && peek()->kind == lexer::TokenKind::Identifier &&
          peek(1) != nullptr && peek(1)->kind == lexer::TokenKind::Colon) {
        const std::string field_name = peek()->lexeme;
        index_ += 2;
        std::unique_ptr<Node> value;
        if (peek() == nullptr || peek()->kind == lexer::TokenKind::Comma ||
            peek()->kind == lexer::TokenKind::RParen) {
          value = make_node("PatBind", span_);
          value->string_field("name", field_name);
        } else {
          value = parse_pattern();
        }
        if (value == nullptr) {
          return nullptr;
        }
        auto field = make_node("PatKwField", span_);
        field->string_field("name", field_name);
        field->node_field("value", std::move(value));
        kw_fields.push_back(std::move(field));
      } else {
        std::unique_ptr<Node> value = parse_pattern();
        if (value == nullptr) {
          return nullptr;
        }
        pos_args.push_back(std::move(value));
      }

      if (match(lexer::TokenKind::Comma)) {
        if (match(lexer::TokenKind::RParen)) {
          break;
        }
        continue;
      }
      if (match(lexer::TokenKind::RParen)) {
        break;
      }
      return nullptr;
    }

    node->list_field("pos_args", std::move(pos_args));
    node->list_field("kw_fields", std::move(kw_fields));
    return node;
  }

  std::string parse_sequence_rest_mode() {
    if (!match(lexer::TokenKind::Star)) {
      return "";
    }
    const lexer::Token *rest = peek();
    if (rest == nullptr) {
      return "";
    }
    if (rest->kind == lexer::TokenKind::Identifier && rest->lexeme == "_") {
      ++index_;
      return "ignore_rest";
    }
    if (rest->kind == lexer::TokenKind::Identifier) {
      ++index_;
      return "bind_rest(" + rest->lexeme + ")";
    }
    return "";
  }

  std::unique_ptr<Node> parse_sequence_pattern(const std::string &kind,
                                               lexer::TokenKind open_kind,
                                               lexer::TokenKind close_kind,
                                               bool allow_grouping) {
    if (!match(open_kind)) {
      return nullptr;
    }

    std::vector<std::unique_ptr<Node>> items;
    std::string rest_mode = "none";
    bool rest_seen = false;
    bool multiple_rest = false;
    std::size_t rest_index = 0;
    bool saw_comma = false;
    if (!match(close_kind)) {
      while (true) {
        const std::size_t before = index_;
        std::string current_rest_mode = parse_sequence_rest_mode();
        if (!current_rest_mode.empty()) {
          // A second rest in one sequence is illegal; a single rest may sit
          // anywhere — fixed patterns after it bind from the end.
          if (rest_seen) {
            multiple_rest = true;
          } else {
            rest_index = items.size();
          }
          rest_seen = true;
          rest_mode = current_rest_mode;
        } else {
          index_ = before;
          std::unique_ptr<Node> value = parse_pattern();
          if (value == nullptr) {
            return nullptr;
          }
          items.push_back(std::move(value));
        }

        if (match(lexer::TokenKind::Comma)) {
          saw_comma = true;
          if (match(close_kind)) {
            break;
          }
          continue;
        }
        if (!match(close_kind)) {
          return nullptr;
        }
        break;
      }
    }

    if (allow_grouping && items.size() == 1 && !saw_comma &&
        rest_mode == "none") {
      return std::move(items.front());
    }
    auto node = make_node(kind, span_);
    node->string_field("rest_mode", rest_mode);
    node->bool_field("rest_tail_valid", !multiple_rest);
    node->string_field("rest_index", std::to_string(rest_index));
    node->list_field("items", std::move(items));
    return node;
  }

  std::unique_ptr<Node> parse_tuple() {
    return parse_sequence_pattern("PatTuple", lexer::TokenKind::LParen,
                                  lexer::TokenKind::RParen, true);
  }

  std::unique_ptr<Node> parse_list() {
    return parse_sequence_pattern("PatList", lexer::TokenKind::LBracket,
                                  lexer::TokenKind::RBracket, false);
  }

  std::unique_ptr<Node> parse_map() {
    if (!match(lexer::TokenKind::LBrace)) {
      return nullptr;
    }

    auto node = make_node("PatMap", span_);
    std::vector<std::unique_ptr<Node>> fields;
    std::string rest_mode = "extra_ok";
    bool rest_tail_valid = true;
    bool rest_seen = false;

    if (!match(lexer::TokenKind::RBrace)) {
      while (true) {
        if (peek() != nullptr &&
            peek()->kind == lexer::TokenKind::StarStar &&
            peek(1) != nullptr) {
          ++index_;
          if (rest_seen) {
            rest_tail_valid = false;
          }
          rest_seen = true;
          if (peek()->kind == lexer::TokenKind::KeywordNull) {
            ++index_;
            rest_mode = "strict_null";
          } else if (peek()->kind == lexer::TokenKind::Identifier &&
                     peek()->lexeme == "_") {
            ++index_;
            rest_mode = "ignore_rest";
          } else if (peek()->kind == lexer::TokenKind::Identifier) {
            rest_mode = "bind_rest(" + peek()->lexeme + ")";
            ++index_;
          } else {
            return nullptr;
          }
        } else {
          if (rest_seen) {
            rest_tail_valid = false;
          }
          const lexer::Token *key = peek();
          if (key == nullptr || key->kind != lexer::TokenKind::Identifier) {
            return nullptr;
          }
          const std::string field_name = key->lexeme;
          ++index_;
          if (!match(lexer::TokenKind::Colon)) {
            return nullptr;
          }
          std::unique_ptr<Node> value;
          if (peek() == nullptr || peek()->kind == lexer::TokenKind::Comma ||
              peek()->kind == lexer::TokenKind::RBrace) {
            value = make_node("PatBind", span_);
            value->string_field("name", field_name);
          } else {
            value = parse_pattern();
          }
          if (value == nullptr) {
            return nullptr;
          }
          auto field = make_node("PatMapField", span_);
          field->string_field("name", field_name);
          field->node_field("value", std::move(value));
          fields.push_back(std::move(field));
        }

        if (match(lexer::TokenKind::Comma)) {
          if (match(lexer::TokenKind::RBrace)) {
            break;
          }
          continue;
        }
        if (!match(lexer::TokenKind::RBrace)) {
          return nullptr;
        }
        break;
      }
    }

    node->string_field("rest_mode", rest_mode);
    node->bool_field("rest_tail_valid", rest_tail_valid);
    node->list_field("fields", std::move(fields));
    return node;
  }

  std::unique_ptr<Node> make_matcher_expr(const std::string &text) const {
    auto node = make_node("PatMatcherExpr", span_);
    node->string_field("expr_text", trim_ascii(text));
    return node;
  }
};

void collect_binding_names_impl(const ast::Expr &pattern,
                                std::vector<std::string> *names) {
  if (pattern.kind == "PatBind") {
    const std::string *value = nullptr;
    for (const ast::StringField &field : pattern.string_fields) {
      if (field.name == "name") {
        value = &field.value;
        break;
      }
    }
    if (value != nullptr) {
      for (const std::string &existing : *names) {
        if (existing == *value) {
          return;
        }
      }
      names->push_back(*value);
    }
    return;
  }
  if (pattern.kind == "PatAs") {
    const std::string bind_name = string_value(pattern, "bind_name");
    if (!bind_name.empty()) {
      bool exists = false;
      for (const std::string &existing : *names) {
        if (existing == bind_name) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        names->push_back(bind_name);
      }
    }
  }
  if (pattern.kind == "PatMap") {
    const std::string rest_name =
        map_rest_binding_name(string_value(pattern, "rest_mode"));
    if (!rest_name.empty()) {
      bool exists = false;
      for (const std::string &existing : *names) {
        if (existing == rest_name) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        names->push_back(rest_name);
      }
    }
  }
  if (pattern.kind == "PatTuple" || pattern.kind == "PatList") {
    const std::string rest_name =
        sequence_rest_binding_name(string_value(pattern, "rest_mode"));
    if (!rest_name.empty()) {
      bool exists = false;
      for (const std::string &existing : *names) {
        if (existing == rest_name) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        names->push_back(rest_name);
      }
    }
  }
  for (const ast::NodeField &field : pattern.node_fields) {
    if (field.value != nullptr) {
      collect_binding_names_impl(*field.value, names);
    }
  }
  for (const ast::ListField &field : pattern.list_fields) {
    for (const std::unique_ptr<ast::Expr> &value : field.values) {
      if (value != nullptr) {
        collect_binding_names_impl(*value, names);
      }
    }
  }
}

void collect_binding_counts(const ast::Expr &pattern,
                            std::map<std::string, int> *counts) {
  if (pattern.kind == "PatBind") {
    const std::string name = string_value(pattern, "name");
    if (!name.empty()) {
      ++(*counts)[name];
    }
    return;
  }
  if (pattern.kind == "PatAs") {
    const std::string bind_name = string_value(pattern, "bind_name");
    if (!bind_name.empty()) {
      ++(*counts)[bind_name];
    }
  }
  if (pattern.kind == "PatMap") {
    const std::string rest_name =
        map_rest_binding_name(string_value(pattern, "rest_mode"));
    if (!rest_name.empty()) {
      ++(*counts)[rest_name];
    }
  }
  if (pattern.kind == "PatTuple" || pattern.kind == "PatList") {
    const std::string rest_name =
        sequence_rest_binding_name(string_value(pattern, "rest_mode"));
    if (!rest_name.empty()) {
      ++(*counts)[rest_name];
    }
  }
  if (pattern.kind == "PatOr") {
    const ast::ListField *alternatives = list_field(pattern, "alternatives");
    if (alternatives != nullptr && !alternatives->values.empty()) {
      collect_binding_counts(*alternatives->values.front(), counts);
    }
    return;
  }
  for (const ast::NodeField &field : pattern.node_fields) {
    if (field.value != nullptr) {
      collect_binding_counts(*field.value, counts);
    }
  }
  for (const ast::ListField &field : pattern.list_fields) {
    for (const std::unique_ptr<ast::Expr> &value : field.values) {
      if (value != nullptr) {
        collect_binding_counts(*value, counts);
      }
    }
  }
}

void append_unique_name(std::vector<std::string> *names,
                        const std::string &name) {
  if (name.empty()) {
    return;
  }
  if (std::find(names->begin(), names->end(), name) == names->end()) {
    names->push_back(name);
  }
}

std::unique_ptr<Node> compile_pattern_node(const ast::Expr &pattern) {
  if (pattern.kind == "PatMatcherExpr") {
    auto node = make_node("PIrMatcherExpr", pattern.span);
    node->string_field("expr_text", string_value(pattern, "expr_text"));
    node->string_field("protocol", "triple_eq");
    return node;
  }
  if (pattern.kind == "PatIgnore") {
    return make_node("PIrIgnore", pattern.span);
  }
  if (pattern.kind == "PatBind") {
    auto node = make_node("PIrBind", pattern.span);
    node->string_field("name", string_value(pattern, "name"));
    return node;
  }
  if (pattern.kind == "PatPin") {
    auto node = make_node("PIrPin", pattern.span);
    node->string_field("name", string_value(pattern, "name"));
    return node;
  }
  if (pattern.kind == "PatLiteral") {
    auto node = make_node("PIrLiteral", pattern.span);
    node->string_field("token", string_value(pattern, "token"));
    node->string_field("value", string_value(pattern, "value"));
    return node;
  }
  if (pattern.kind == "PatConst") {
    auto node = make_node("PIrConst", pattern.span);
    node->string_field("path", string_value(pattern, "path"));
    return node;
  }
  if (pattern.kind == "PatAs") {
    auto node = make_node("PIrAs", pattern.span);
    node->string_field("bind_name", string_value(pattern, "bind_name"));
    if (const ast::Expr *inner = node_field(pattern, "inner")) {
      node->node_field("inner", compile_pattern_node(*inner));
    }
    return node;
  }
  if (pattern.kind == "PatOr") {
    auto node = make_node("PIrOr", pattern.span);
    std::vector<std::unique_ptr<Node>> alternatives;
    if (const ast::ListField *list = list_field(pattern, "alternatives")) {
      for (const std::unique_ptr<ast::Expr> &alt : list->values) {
        alternatives.push_back(compile_pattern_node(*alt));
      }
    }
    node->list_field("alternatives", std::move(alternatives));
    return node;
  }
  if (pattern.kind == "PatTuple") {
    auto node = make_node("PIrTuple", pattern.span);
    const std::string rest_mode = string_value(pattern, "rest_mode");
    const std::string rest_binding = sequence_rest_binding_name(rest_mode);
    const ast::ListField *list = list_field(pattern, "items");
    const std::size_t fixed_count = list == nullptr ? 0 : list->values.size();
    node->string_field("rest_mode", rest_mode);
    node->string_field("rest_kind", normalized_sequence_rest_kind(rest_mode));
    node->bool_field("capture_rest", !rest_binding.empty());
    node->bool_field("exact_arity", rest_mode == "none");
    node->string_field("min_arity", std::to_string(fixed_count));
    if (!rest_binding.empty()) {
      node->string_field("rest_binding", rest_binding);
    }
    std::vector<std::unique_ptr<Node>> items;
    if (list != nullptr) {
      for (const std::unique_ptr<ast::Expr> &item : list->values) {
        items.push_back(compile_pattern_node(*item));
      }
    }
    node->list_field("items", std::move(items));
    return node;
  }
  if (pattern.kind == "PatList") {
    auto node = make_node("PIrList", pattern.span);
    const std::string rest_mode = string_value(pattern, "rest_mode");
    const std::string rest_binding = sequence_rest_binding_name(rest_mode);
    const ast::ListField *list = list_field(pattern, "items");
    const std::size_t fixed_count = list == nullptr ? 0 : list->values.size();
    node->string_field("rest_mode", rest_mode);
    node->string_field("rest_kind", normalized_sequence_rest_kind(rest_mode));
    node->bool_field("capture_rest", !rest_binding.empty());
    node->bool_field("exact_arity", rest_mode == "none");
    node->string_field("min_arity", std::to_string(fixed_count));
    if (!rest_binding.empty()) {
      node->string_field("rest_binding", rest_binding);
    }
    std::vector<std::unique_ptr<Node>> items;
    if (list != nullptr) {
      for (const std::unique_ptr<ast::Expr> &item : list->values) {
        items.push_back(compile_pattern_node(*item));
      }
    }
    node->list_field("items", std::move(items));
    return node;
  }
  if (pattern.kind == "PatMapField") {
    auto node = make_node("PIrMapField", pattern.span);
    node->string_field("name", string_value(pattern, "name"));
    if (const ast::Expr *value = node_field(pattern, "value")) {
      node->node_field("value", compile_pattern_node(*value));
    }
    return node;
  }
  if (pattern.kind == "PatMap") {
    auto node = make_node("PIrMap", pattern.span);
    const std::string rest_mode = string_value(pattern, "rest_mode");
    const std::string rest_binding = map_rest_binding_name(rest_mode);
    const std::vector<std::string> requested_keys =
        collect_named_fields(pattern, "fields");
    node->string_field("rest_mode", rest_mode);
    node->string_field("rest_kind", normalized_rest_kind(rest_mode));
    node->bool_field("strict_map", is_strict_null_rest_mode(rest_mode));
    node->bool_field("capture_rest", !rest_binding.empty());
    node->bool_field("ignore_rest", is_ignore_rest_mode(rest_mode));
    node->bool_field("needs_full_map", is_strict_null_rest_mode(rest_mode) ||
                                           !rest_binding.empty());
    if (!rest_binding.empty()) {
      node->string_field("rest_binding", rest_binding);
    }
    add_requested_keys(*node, requested_keys, pattern.span);
    std::vector<std::unique_ptr<Node>> fields;
    if (const ast::ListField *list = list_field(pattern, "fields")) {
      for (const std::unique_ptr<ast::Expr> &field : list->values) {
        fields.push_back(compile_pattern_node(*field));
      }
    }
    node->list_field("fields", std::move(fields));
    return node;
  }
  if (pattern.kind == "PatKwField") {
    auto node = make_node("PIrKwField", pattern.span);
    node->string_field("name", string_value(pattern, "name"));
    if (const ast::Expr *value = node_field(pattern, "value")) {
      node->node_field("value", compile_pattern_node(*value));
    }
    return node;
  }
  if (pattern.kind == "PatHead") {
    auto node = make_node("PIrHead", pattern.span);
    const std::string destructure_mode = head_destructure_mode(pattern);
    const std::vector<std::string> requested_keys =
        collect_named_fields(pattern, "kw_fields");
    node->string_field("head", string_value(pattern, "head"));
    node->string_field("destructure_mode", destructure_mode);
    node->bool_field("requires_deconstruct", destructure_mode == "POSITIONAL" ||
                                                 destructure_mode == "MIXED");
    node->bool_field("requires_deconstruct_keys",
                     destructure_mode == "KEYS" || destructure_mode == "MIXED");
    node->bool_field("needs_full_map", false);
    add_requested_keys(*node, requested_keys, pattern.span);
    std::vector<std::unique_ptr<Node>> pos_args;
    std::vector<std::unique_ptr<Node>> kw_fields;
    if (const ast::ListField *list = list_field(pattern, "pos_args")) {
      for (const std::unique_ptr<ast::Expr> &arg : list->values) {
        pos_args.push_back(compile_pattern_node(*arg));
      }
    }
    if (const ast::ListField *list = list_field(pattern, "kw_fields")) {
      for (const std::unique_ptr<ast::Expr> &field : list->values) {
        kw_fields.push_back(compile_pattern_node(*field));
      }
    }
    node->list_field("pos_args", std::move(pos_args));
    node->list_field("kw_fields", std::move(kw_fields));
    return node;
  }
  if (pattern.kind == "PatDynamic") {
    auto node = make_node("PIrDynamic", pattern.span);
    node->string_field("matcher_text", string_value(pattern, "matcher_text"));
    const ast::Expr *export_map_pattern =
        node_field(pattern, "export_map_pattern");
    node->string_field("protocol", "DynamicMatchResult");
    node->string_field("binding_mode", export_map_pattern == nullptr
                                           ? "forbid_bindings"
                                           : "map_pattern");
    node->bool_field("requires_empty_bindings", export_map_pattern == nullptr);
    if (export_map_pattern != nullptr) {
      node->node_field("export_map_pattern",
                       compile_pattern_node(*export_map_pattern));
    }
    return node;
  }
  auto node = make_node("PIrUnsupported", pattern.span);
  node->string_field("source_kind", pattern.kind);
  return node;
}

std::unique_ptr<Node> compile_match_program_node(const ast::Expr &pattern) {
  if (pattern.kind == "PatMatcherExpr") {
    auto node = make_node("PMatcherExpr", pattern.span);
    node->string_field("expr_text", string_value(pattern, "expr_text"));
    node->string_field("protocol", "triple_eq");
    return node;
  }
  if (pattern.kind == "PatIgnore") {
    return make_node("PWildcard", pattern.span);
  }
  if (pattern.kind == "PatBind") {
    auto node = make_node("PBind", pattern.span);
    node->string_field("name", string_value(pattern, "name"));
    return node;
  }
  if (pattern.kind == "PatPin") {
    auto node = make_node("PPin", pattern.span);
    node->string_field("name", string_value(pattern, "name"));
    return node;
  }
  if (pattern.kind == "PatLiteral") {
    auto node = make_node("PLiteral", pattern.span);
    node->string_field("token", string_value(pattern, "token"));
    node->string_field("value", string_value(pattern, "value"));
    return node;
  }
  if (pattern.kind == "PatConst") {
    auto node = make_node("PConstMatch", pattern.span);
    node->string_field("path", string_value(pattern, "path"));
    node->string_field("protocol", "triple_eq");
    return node;
  }
  if (pattern.kind == "PatAs") {
    auto node = make_node("PAs", pattern.span);
    node->string_field("bind_name", string_value(pattern, "bind_name"));
    if (const ast::Expr *inner = node_field(pattern, "inner")) {
      node->node_field("inner", compile_match_program_node(*inner));
    }
    return node;
  }
  if (pattern.kind == "PatOr") {
    auto node = make_node("POr", pattern.span);
    std::vector<std::unique_ptr<Node>> alternatives;
    if (const ast::ListField *list = list_field(pattern, "alternatives")) {
      for (const std::unique_ptr<ast::Expr> &alt : list->values) {
        alternatives.push_back(compile_match_program_node(*alt));
      }
    }
    node->list_field("alternatives", std::move(alternatives));
    return node;
  }
  if (pattern.kind == "PatTuple" || pattern.kind == "PatList") {
    auto node = make_node(pattern.kind == "PatTuple" ? "PSeqTuple" : "PSeqList",
                          pattern.span);
    const std::string rest_mode = string_value(pattern, "rest_mode");
    const std::string rest_binding = sequence_rest_binding_name(rest_mode);
    const ast::ListField *list = list_field(pattern, "items");
    const std::size_t fixed_count = list == nullptr ? 0 : list->values.size();
    node->string_field("rest_mode", rest_mode);
    node->string_field("rest_kind", normalized_sequence_rest_kind(rest_mode));
    node->bool_field("capture_rest", !rest_binding.empty());
    node->bool_field("exact_arity", rest_mode == "none");
    node->string_field("min_arity", std::to_string(fixed_count));
    if (!rest_binding.empty()) {
      node->string_field("rest_binding", rest_binding);
    }
    const std::string rest_index_str = string_value(pattern, "rest_index");
    const std::size_t rest_index =
        rest_index_str.empty()
            ? 0U
            : static_cast<std::size_t>(std::stoul(rest_index_str));
    const bool has_rest = rest_mode != "none";
    const std::size_t total = list == nullptr ? 0U : list->values.size();
    std::vector<std::unique_ptr<Node>> items;
    if (list != nullptr) {
      for (std::size_t p = 0; p < list->values.size(); ++p) {
        auto item = make_node("PSeqItem", list->values[p]->span);
        if (has_rest && p >= rest_index) {
          // Fixed pattern after the rest (`[a, *b, c]`): bind from the end so
          // the rest captures the variable-length middle.
          item->string_field("index", std::to_string(total - 1U - p));
          item->bool_field("from_end", true);
        } else {
          item->string_field("index", std::to_string(p));
          item->bool_field("from_end", false);
        }
        item->node_field("pattern", compile_match_program_node(*list->values[p]));
        items.push_back(std::move(item));
      }
    }
    node->list_field("items", std::move(items));
    return node;
  }
  if (pattern.kind == "PatMap") {
    auto node = make_node("PMap", pattern.span);
    const std::string rest_mode = string_value(pattern, "rest_mode");
    const std::string rest_binding = map_rest_binding_name(rest_mode);
    const std::vector<std::string> requested_keys =
        collect_named_fields(pattern, "fields");
    node->string_field("rest_mode", rest_mode);
    node->string_field("rest_kind", normalized_rest_kind(rest_mode));
    node->bool_field("strict_map", is_strict_null_rest_mode(rest_mode));
    node->bool_field("capture_rest", !rest_binding.empty());
    node->bool_field("ignore_rest", is_ignore_rest_mode(rest_mode));
    node->bool_field("needs_full_map", is_strict_null_rest_mode(rest_mode) ||
                                           !rest_binding.empty());
    if (!rest_binding.empty()) {
      node->string_field("rest_binding", rest_binding);
    }
    add_named_entries(*node, "requested_keys", "PRequestedKey", requested_keys,
                      pattern.span);
    std::vector<std::unique_ptr<Node>> fields;
    if (const ast::ListField *list = list_field(pattern, "fields")) {
      for (const std::unique_ptr<ast::Expr> &field : list->values) {
        auto field_node = make_node("PMapField", field->span);
        field_node->string_field("name", string_value(*field, "name"));
        if (const ast::Expr *value = node_field(*field, "value")) {
          field_node->node_field("pattern", compile_match_program_node(*value));
        }
        fields.push_back(std::move(field_node));
      }
    }
    node->list_field("fields", std::move(fields));
    return node;
  }
  if (pattern.kind == "PatHead") {
    auto node = make_node("PHead", pattern.span);
    const std::string destructure_mode = head_destructure_mode(pattern);
    const std::vector<std::string> requested_keys =
        collect_named_fields(pattern, "kw_fields");
    node->string_field("head", string_value(pattern, "head"));
    node->string_field("destructure_mode", destructure_mode);
    node->bool_field("requires_deconstruct", destructure_mode == "POSITIONAL" ||
                                                 destructure_mode == "MIXED");
    node->bool_field("requires_deconstruct_keys",
                     destructure_mode == "KEYS" || destructure_mode == "MIXED");
    node->bool_field("needs_full_map", false);
    add_named_entries(*node, "requested_keys", "PRequestedKey", requested_keys,
                      pattern.span);

    std::vector<std::unique_ptr<Node>> pos_args;
    if (const ast::ListField *list = list_field(pattern, "pos_args")) {
      for (std::size_t index = 0; index < list->values.size(); ++index) {
        auto item = make_node("PSeqItem", list->values[index]->span);
        item->string_field("index", std::to_string(index));
        item->node_field("pattern",
                         compile_match_program_node(*list->values[index]));
        pos_args.push_back(std::move(item));
      }
    }
    std::vector<std::unique_ptr<Node>> kw_fields;
    if (const ast::ListField *list = list_field(pattern, "kw_fields")) {
      for (const std::unique_ptr<ast::Expr> &field : list->values) {
        auto field_node = make_node("PMapField", field->span);
        field_node->string_field("name", string_value(*field, "name"));
        if (const ast::Expr *value = node_field(*field, "value")) {
          field_node->node_field("pattern", compile_match_program_node(*value));
        }
        kw_fields.push_back(std::move(field_node));
      }
    }
    node->list_field("pos_args", std::move(pos_args));
    node->list_field("kw_fields", std::move(kw_fields));
    return node;
  }
  if (pattern.kind == "PatDynamic") {
    auto node = make_node("PDynamic", pattern.span);
    const ast::Expr *export_map_pattern =
        node_field(pattern, "export_map_pattern");
    node->string_field("matcher_text", string_value(pattern, "matcher_text"));
    node->string_field("protocol", "DynamicMatchResult");
    node->string_field("binding_mode", export_map_pattern == nullptr
                                           ? "forbid_bindings"
                                           : "map_pattern");
    node->bool_field("requires_empty_bindings", export_map_pattern == nullptr);
    if (export_map_pattern != nullptr) {
      node->node_field("export_map_program",
                       compile_match_program_node(*export_map_pattern));
    }
    return node;
  }
  auto node = make_node("PUnsupported", pattern.span);
  node->string_field("source_kind", pattern.kind);
  return node;
}

std::vector<std::string> sorted_binding_names(const ast::Expr &pattern) {
  std::vector<std::string> names = collect_binding_names(pattern);
  std::sort(names.begin(), names.end());
  return names;
}

std::unique_ptr<ast::Expr>
parse_matcher_expression_impl(const std::string &text,
                              const lexer::Span &span) {
  lexer::Lexer lexer(text, span.file.empty() ? "<pattern>" : span.file);
  lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    return nullptr;
  }

  parser::Parser parser(lex_result.tokens);
  parser::ParseResult parse_result = parser.parse_expression_unit();
  if (!parse_result.ok()) {
    return nullptr;
  }
  return std::move(parse_result.expr);
}

void collect_name_references(const ast::Expr &expr,
                             std::vector<std::string> *references) {
  if (expr.kind == "AstName") {
    const std::string name = string_value(expr, "name");
    append_unique_name(references, name);
    return;
  }

  for (const ast::NodeField &field : expr.node_fields) {
    if (field.value != nullptr) {
      collect_name_references(*field.value, references);
    }
  }
  for (const ast::ListField &field : expr.list_fields) {
    for (const std::unique_ptr<ast::Expr> &value : field.values) {
      if (value != nullptr) {
        collect_name_references(*value, references);
      }
    }
  }
}

void collect_pattern_references_impl(const ast::Expr &pattern,
                                     std::vector<std::string> *references) {
  if (pattern.kind == "PatPin") {
    append_unique_name(references, string_value(pattern, "name"));
    return;
  }
  if (pattern.kind == "PatDynamic" || pattern.kind == "PatMatcherExpr") {
    const std::string text = pattern.kind == "PatDynamic"
                                 ? string_value(pattern, "matcher_text")
                                 : string_value(pattern, "expr_text");
    std::unique_ptr<ast::Expr> matcher_expr =
        parse_matcher_expression_impl(text, pattern.span);
    if (matcher_expr != nullptr) {
      collect_name_references(*matcher_expr, references);
    }
  }

  for (const ast::NodeField &field : pattern.node_fields) {
    if (field.value != nullptr) {
      collect_pattern_references_impl(*field.value, references);
    }
  }
  for (const ast::ListField &field : pattern.list_fields) {
    for (const std::unique_ptr<ast::Expr> &value : field.values) {
      if (value != nullptr) {
        collect_pattern_references_impl(*value, references);
      }
    }
  }
}

void validate_pattern_impl(const ast::Expr &pattern, PatternContext context,
                           const std::vector<std::string> &root_bindings,
                           std::vector<lexer::Diagnostic> *diagnostics) {
  if ((pattern.kind == "PatMap" || pattern.kind == "PatTuple" ||
       pattern.kind == "PatList") &&
      !pattern.bool_fields.empty()) {
    for (const ast::BoolField &field : pattern.bool_fields) {
      if (field.name == "rest_tail_valid" && !field.value) {
        diagnostics->push_back(lexer::Diagnostic{
            "E1003", "error", "pattern", "rest pattern outside tail position",
            pattern.span});
        break;
      }
    }
  }

  if (pattern.kind == "PatMatcherExpr" &&
      !context_allows_bare_matcher(context)) {
    diagnostics->push_back(lexer::Diagnostic{
        "E1008", "error", "pattern",
        "bare matcher expression outside case or case!", pattern.span});
  }

  if (pattern.kind == "PatDynamic" &&
      !context_allows_dynamic_pattern(context)) {
    diagnostics->push_back(lexer::Diagnostic{
        "E1009", "error", "pattern",
        "dynamic pattern object in block params or pattern assignment",
        pattern.span});
  }

  if (pattern.kind == "PatOr") {
    if (const ast::ListField *alternatives =
            list_field(pattern, "alternatives");
        alternatives != nullptr && !alternatives->values.empty()) {
      const std::vector<std::string> expected =
          sorted_binding_names(*alternatives->values.front());
      for (std::size_t index = 1; index < alternatives->values.size();
           ++index) {
        if (sorted_binding_names(*alternatives->values[index]) != expected) {
          diagnostics->push_back(lexer::Diagnostic{
              "E1002", "error", "pattern",
              "different binding sets across OR-pattern alternatives",
              pattern.span});
          break;
        }
      }
    }
  }

  if (pattern.kind == "PatDynamic") {
    std::unique_ptr<ast::Expr> matcher_expr = parse_matcher_expression_impl(
        string_value(pattern, "matcher_text"), pattern.span);
    if (matcher_expr != nullptr) {
      std::vector<std::string> matcher_refs;
      collect_name_references(*matcher_expr, &matcher_refs);
      for (const std::string &name : matcher_refs) {
        if (std::find(root_bindings.begin(), root_bindings.end(), name) !=
            root_bindings.end()) {
          diagnostics->push_back(
              lexer::Diagnostic{"E1011", "error", "pattern",
                                "pattern(expr) references binding introduced "
                                "by same enclosing pattern",
                                pattern.span});
          break;
        }
      }
    }
  }

  for (const ast::NodeField &field : pattern.node_fields) {
    if (field.value != nullptr) {
      validate_pattern_impl(*field.value, context, root_bindings, diagnostics);
    }
  }
  for (const ast::ListField &field : pattern.list_fields) {
    for (const std::unique_ptr<ast::Expr> &value : field.values) {
      if (value != nullptr) {
        validate_pattern_impl(*value, context, root_bindings, diagnostics);
      }
    }
  }
}

} // namespace

std::unique_ptr<ast::Expr> parse_pattern_text(const std::string &text,
                                              const lexer::Span &span) {
  return Parser(text, span).parse();
}

std::unique_ptr<ast::Expr> parse_matcher_expression(const std::string &text,
                                                    const lexer::Span &span) {
  return parse_matcher_expression_impl(text, span);
}

std::vector<std::string> collect_binding_names(const ast::Expr &pattern) {
  std::vector<std::string> names;
  collect_binding_names_impl(pattern, &names);
  return names;
}

std::vector<std::string> collect_reference_names(const ast::Expr &pattern) {
  std::vector<std::string> names;
  collect_pattern_references_impl(pattern, &names);
  return names;
}

std::vector<lexer::Diagnostic> validate_pattern(const ast::Expr &pattern,
                                                PatternContext context) {
  std::vector<lexer::Diagnostic> diagnostics;
  std::map<std::string, int> counts;
  const std::vector<std::string> root_bindings = collect_binding_names(pattern);
  collect_binding_counts(pattern, &counts);
  for (const auto &entry : counts) {
    if (entry.second > 1) {
      diagnostics.push_back(lexer::Diagnostic{
          "E1001", "error", "pattern", "duplicate binding names in one pattern",
          pattern.span});
      break;
    }
  }
  validate_pattern_impl(pattern, context, root_bindings, &diagnostics);
  return diagnostics;
}

std::unique_ptr<ast::Expr> compile_pattern_ir(const ast::Expr &pattern) {
  auto node = make_node("HCompiledPattern", pattern.span);
  node->node_field("pattern_ir", compile_pattern_node(pattern));
  auto match_program = make_node("PProgram", pattern.span);
  match_program->node_field("root", compile_match_program_node(pattern));
  add_named_entries(*match_program, "binding_order", "PBindingName",
                    collect_binding_names(pattern), pattern.span);
  match_program->bool_field("requires_commit",
                            !collect_binding_names(pattern).empty());
  node->node_field("match_program", std::move(match_program));
  return node;
}

bool is_map_subject_pattern(const ast::Expr &pattern) {
  return pattern.kind == "PatMap";
}

bool is_tuple_subject_pattern(const ast::Expr &pattern) {
  return pattern.kind == "PatTuple";
}

} // namespace amber::pattern
