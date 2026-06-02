#pragma once

#include "frontend/ast/expr.h"
#include "frontend/lexer/token.h"

#include <memory>
#include <string>
#include <vector>

namespace amber::binder {

struct Scope {
  std::string id;
  int parent_index = -1;
  std::string kind;
  std::string owner;
  lexer::Span span;
  std::vector<std::string> bindings;
};

struct Binding {
  std::string id;
  int scope_index = -1;
  std::string name;
  std::string kind;
  std::string role;
  bool read_only = false;
  bool property_has_getter = false;
  bool property_has_setter = false;
  lexer::Span span;
  std::string source;
};

struct Reference {
  std::string id;
  int scope_index = -1;
  std::string name;
  std::string ref_kind;
  bool resolved = false;
  std::string binding_id;
  lexer::Span span;
};

struct Export {
  std::string local_name;
  std::string public_name;
  bool resolved = false;
  std::string binding_id;
  lexer::Span span;
};

struct ParamDescriptor {
  std::string external_name;
  std::string local_name;
  std::string kind;
  std::string auto_assign_kind;
  std::string auto_assign_target;
  std::string type_expr;
  bool has_default = false;
  std::string default_kind;
  lexer::Span span;
};

struct Signature {
  std::string id;
  int scope_index = -1;
  std::string owner;
  std::string return_type_expr;
  std::string effect_row_expr;
  bool has_effect_row = false;
  lexer::Span span;
  std::vector<ParamDescriptor> params;
};

struct CallArgShape {
  std::string keyword_name;
  lexer::Span span;
};

struct BoundCallSlot {
  std::string local_name;
  std::string external_name;
  std::string param_kind;
  std::string source_kind;
  int argument_index = -1;
  std::string keyword_name;
  lexer::Span param_span;
  lexer::Span argument_span;
};

struct PendingAutoAssign {
  int slot_index = -1;
  std::string target_name;
  std::string target_kind;
  lexer::Span span;
};

struct CallSiteShape {
  bool found = false;
  std::string call_kind;
  std::string call_style;
  lexer::Span span;
  std::vector<CallArgShape> args;
};

struct BindGraph {
  std::vector<Scope> scopes;
  std::vector<Binding> bindings;
  std::vector<Reference> references;
  std::vector<Export> exports;
  std::vector<Signature> signatures;
};

struct BindResult {
  BindGraph graph;
  std::vector<lexer::Diagnostic> diagnostics;

  bool ok() const;
};

struct CallBindResult {
  std::vector<BoundCallSlot> slots;
  std::vector<int> default_order;
  std::vector<PendingAutoAssign> pending_auto_assigns;
  std::vector<lexer::Diagnostic> diagnostics;

  bool ok() const;
};

BindResult bind_module(const std::vector<std::unique_ptr<ast::Expr>> &items,
                       const std::string &module_name);
std::vector<lexer::Diagnostic> unresolved_name_diagnostics(
    const std::vector<std::unique_ptr<ast::Expr>> &items,
    const BindGraph &graph);
CallSiteShape extract_call_shape(const ast::Expr &expr);
CallBindResult bind_call_shape(const Signature &signature,
                               const std::vector<CallArgShape> &args);

std::string bind_graph_to_json(const BindGraph &graph,
                               const std::string &module_name,
                               const std::string &source_hash);

} // namespace amber::binder
