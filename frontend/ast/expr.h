#pragma once

#include "frontend/lexer/token.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace amber::ast {

struct Expr;

struct NodeField {
  std::string name;
  std::unique_ptr<Expr> value;
};

struct ListField {
  std::string name;
  std::vector<std::unique_ptr<Expr>> values;
};

struct StringField {
  std::string name;
  std::string value;
};

struct BoolField {
  std::string name;
  bool value;
};

struct Expr {
  std::string kind;
  lexer::Span span;
  std::vector<StringField> string_fields;
  std::vector<BoolField> bool_fields;
  std::vector<NodeField> node_fields;
  std::vector<ListField> list_fields;

  Expr(std::string kind, lexer::Span span);

  Expr &string_field(std::string name, std::string value);
  Expr &bool_field(std::string name, bool value);
  Expr &node_field(std::string name, std::unique_ptr<Expr> value);
  Expr &list_field(std::string name, std::vector<std::unique_ptr<Expr>> values);
};

std::unique_ptr<Expr> make_expr(std::string kind, lexer::Span span);
// Deep-copy an AST node (all fields + child subtrees). Used by the macro `Ast`
// value builder so a freshly constructed parent gets sole ownership of child
// subnodes drawn from other (shared) Ast values.
std::unique_ptr<Expr> clone_expr(const Expr &expr);
// Macro expansion (partial F1.5): rewrite `quote:` / `unquote` nodes across a
// parsed module into ordinary builder-call AST (`Ast.node("Kind", {fields})`),
// so downstream compilation produces code that constructs live `Ast` values at
// runtime. `unquote(e)` holes become the raw expression `e` (which evaluates to
// an Ast value and is spliced as a child). Runs after parsing, before binding.
// `unquote_splice` is left intact (rejected later) — not supported yet.
void expand_quotes(std::vector<std::unique_ptr<Expr>> &items);
lexer::Span join_spans(const lexer::Span &start, const lexer::Span &end);
std::string ast_module_to_json(const Expr &expr,
                               const std::string &source_hash);
std::string ast_module_to_json(const std::vector<std::unique_ptr<Expr>> &items,
                               const std::string &module_name,
                               const std::string &source_hash);

} // namespace amber::ast
