#include "frontend/hir/hir.h"
#include "frontend/pattern/pattern.h"

#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace amber::hir {
namespace {

using Node = ast::Expr;

struct RefKey {
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  std::string name;
  std::string ref_kind;

  bool operator<(const RefKey &other) const {
    if (start_offset != other.start_offset) {
      return start_offset < other.start_offset;
    }
    if (end_offset != other.end_offset) {
      return end_offset < other.end_offset;
    }
    if (name != other.name) {
      return name < other.name;
    }
    return ref_kind < other.ref_kind;
  }
};

struct CapturePlan {
  const binder::Binding *binding = nullptr;
  std::string slot;
  std::string source_kind;
  std::string source_slot;
};

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (c < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<int>(c) << std::dec << std::setfill(' ');
      } else {
        out << static_cast<char>(c);
      }
    }
  }
  return out.str();
}

void append_position_json(std::ostringstream &out,
                          const lexer::Position &position) {
  out << "{\"line\":" << position.line << ",\"col\":" << position.col
      << ",\"offset\":" << position.offset << "}";
}

void append_span_json(std::ostringstream &out, const lexer::Span &span) {
  out << "{\"file\":\"" << json_escape(span.file) << "\",\"start\":";
  append_position_json(out, span.start);
  out << ",\"end\":";
  append_position_json(out, span.end);
  out << "}";
}

const std::string *string_field(const ast::Expr &expr,
                                const std::string &name) {
  for (const ast::StringField &field : expr.string_fields) {
    if (field.name == name) {
      return &field.value;
    }
  }
  return nullptr;
}

std::string string_value(const ast::Expr &expr, const std::string &name) {
  const std::string *value = string_field(expr, name);
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

ast::ListField *mutable_list_field(ast::Expr &expr, const std::string &name) {
  for (ast::ListField &field : expr.list_fields) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

std::unique_ptr<Node> make_node(const std::string &kind,
                                const lexer::Span &span) {
  return ast::make_expr(kind, span);
}

bool same_span(const lexer::Span &left, const lexer::Span &right) {
  return left.start.offset == right.start.offset &&
         left.end.offset == right.end.offset && left.file == right.file;
}

std::string procedure_name_for_owner(const std::string &name) {
  return name.empty() ? "__module_init__" : name;
}

bool binding_has_slot(const binder::Binding &binding) {
  return binding.kind == "local" || binding.kind == "import_alias" ||
         binding.kind == "placeholder";
}

bool signature_param_has_auto_assign(const binder::ParamDescriptor &param) {
  return param.auto_assign_kind == "@" || param.auto_assign_kind == "@@";
}

std::unique_ptr<Node> clone_node(const ast::Expr &expr) {
  auto copy = make_node(expr.kind, expr.span);
  for (const ast::StringField &field : expr.string_fields) {
    copy->string_field(field.name, field.value);
  }
  for (const ast::BoolField &field : expr.bool_fields) {
    copy->bool_field(field.name, field.value);
  }
  for (const ast::NodeField &field : expr.node_fields) {
    if (field.value) {
      copy->node_field(field.name, clone_node(*field.value));
    }
  }
  for (const ast::ListField &field : expr.list_fields) {
    std::vector<std::unique_ptr<Node>> values;
    values.reserve(field.values.size());
    for (const std::unique_ptr<ast::Expr> &value : field.values) {
      values.push_back(clone_node(*value));
    }
    copy->list_field(field.name, std::move(values));
  }
  return copy;
}

std::vector<std::unique_ptr<Node>>
clone_node_list(const std::vector<std::unique_ptr<Node>> &nodes) {
  std::vector<std::unique_ptr<Node>> copies;
  copies.reserve(nodes.size());
  for (const std::unique_ptr<Node> &node : nodes) {
    if (node != nullptr) {
      copies.push_back(clone_node(*node));
    }
  }
  return copies;
}

bool is_string_literal(const ast::Expr &expr) {
  return expr.kind == "AstLiteral" && string_value(expr, "token") == "STRING";
}

std::string unquote_string_literal(const std::string &value) {
  if (value.size() >= 2) {
    const char quote = value.front();
    if ((quote == '"' || quote == '\'') && value.back() == quote) {
      return value.substr(1, value.size() - 2);
    }
  }
  return value;
}

void append_node_json(std::ostringstream &out, const ast::Expr &expr) {
  out << "{\"kind\":\"" << json_escape(expr.kind) << "\",\"span\":";
  append_span_json(out, expr.span);
  for (const ast::StringField &field : expr.string_fields) {
    out << ",\"" << json_escape(field.name) << "\":\""
        << json_escape(field.value) << "\"";
  }
  for (const ast::BoolField &field : expr.bool_fields) {
    out << ",\"" << json_escape(field.name)
        << "\":" << (field.value ? "true" : "false");
  }
  for (const ast::NodeField &field : expr.node_fields) {
    out << ",\"" << json_escape(field.name) << "\":";
    if (field.value) {
      append_node_json(out, *field.value);
    } else {
      out << "null";
    }
  }
  for (const ast::ListField &field : expr.list_fields) {
    out << ",\"" << json_escape(field.name) << "\":[";
    for (std::size_t i = 0; i < field.values.size(); ++i) {
      if (i != 0) {
        out << ",";
      }
      append_node_json(out, *field.values[i]);
    }
    out << "]";
  }
  out << "}";
}

class Lowerer {
public:
  Lowerer(const std::vector<std::unique_ptr<ast::Expr>> &items,
          const std::string &module_name, const binder::BindGraph &graph)
      : items_(items), module_name_(module_name), graph_(graph) {
    for (const binder::Binding &binding : graph_.bindings) {
      bindings_by_id_.emplace(binding.id, &binding);
    }
    for (const binder::Reference &ref : graph_.references) {
      refs_by_key_.emplace(RefKey{ref.span.start.offset, ref.span.end.offset,
                                  ref.name, ref.ref_kind},
                           &ref);
      refs_by_scope_[ref.scope_index].push_back(&ref);
    }
    for (const binder::Signature &signature : graph_.signatures) {
      signatures_by_scope_.emplace(signature.scope_index, &signature);
    }
    for (std::size_t i = 0; i < graph_.scopes.size(); ++i) {
      const binder::Scope &scope = graph_.scopes[i];
      if (scope.kind != "block" || scope.parent_index < 0) {
        continue;
      }
      block_children_by_scope_[scope.parent_index].push_back(
          static_cast<int>(i));
    }
  }

  Program lower() {
    Program program;
    const int module_scope = find_scope_index("module", module_span(), "");
    const std::string module_proc =
        lower_module_procedure(module_scope, collect_module_exec_items());

    auto root = make_node("HModule", module_span());
    root->string_field("module_name", module_name_);
    root->string_field("init", module_proc);
    root->list_field("imports", lower_imports());
    root->list_field("exports", lower_exports());
    root->list_field("items", lower_module_items());
    program.root = std::move(root);
    program.procedures = std::move(procedures_);
    return program;
  }

private:
  struct ProcedureContext {
    int scope_index = -1;
    std::size_t procedure_index = 0;
    int next_slot = 0;
    int next_temp_id = 0;
    std::map<std::string, std::string> slot_by_binding_id;
    std::map<std::string, std::string> capture_slot_by_binding_id;
  };

  const std::vector<std::unique_ptr<ast::Expr>> &items_;
  std::string module_name_;
  const binder::BindGraph &graph_;
  std::vector<Procedure> procedures_;
  std::map<std::string, const binder::Binding *> bindings_by_id_;
  std::map<RefKey, const binder::Reference *> refs_by_key_;
  std::map<int, std::vector<const binder::Reference *>> refs_by_scope_;
  std::map<int, std::vector<int>> block_children_by_scope_;
  std::map<int, std::vector<const binder::Binding *>> capture_bindings_cache_;
  std::map<int, const binder::Signature *> signatures_by_scope_;
  ProcedureContext *current_proc_ = nullptr;

  lexer::Span module_span() const {
    if (items_.empty()) {
      return lexer::Span{};
    }
    return ast::join_spans(items_.front()->span, items_.back()->span);
  }

  int find_scope_index(const std::string &kind, const lexer::Span &span,
                       const std::string &owner) const {
    for (std::size_t i = 0; i < graph_.scopes.size(); ++i) {
      const binder::Scope &scope = graph_.scopes[i];
      if (scope.kind == kind && scope.owner == owner &&
          same_span(scope.span, span)) {
        return static_cast<int>(i);
      }
    }
    for (std::size_t i = 0; i < graph_.scopes.size(); ++i) {
      const binder::Scope &scope = graph_.scopes[i];
      if (scope.kind == kind && same_span(scope.span, span)) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  const binder::Signature *signature_for_scope(int scope_index) const {
    const auto found = signatures_by_scope_.find(scope_index);
    return found == signatures_by_scope_.end() ? nullptr : found->second;
  }

  bool scope_is_within(int scope_index, int ancestor_scope_index) const {
    int current = scope_index;
    while (current >= 0) {
      if (current == ancestor_scope_index) {
        return true;
      }
      current = graph_.scopes[current].parent_index;
    }
    return false;
  }

  static bool binding_less(const binder::Binding *left,
                           const binder::Binding *right) {
    if (left->span.start.offset != right->span.start.offset) {
      return left->span.start.offset < right->span.start.offset;
    }
    if (left->span.end.offset != right->span.end.offset) {
      return left->span.end.offset < right->span.end.offset;
    }
    if (left->name != right->name) {
      return left->name < right->name;
    }
    return left->id < right->id;
  }

  const std::vector<const binder::Binding *> &
  capture_bindings_for_scope(int scope_index) {
    const auto cached = capture_bindings_cache_.find(scope_index);
    if (cached != capture_bindings_cache_.end()) {
      return cached->second;
    }

    std::vector<const binder::Binding *> captures;
    std::set<std::string> seen;
    auto add_capture = [&](const binder::Binding *binding) {
      if (binding == nullptr || !binding_has_slot(*binding)) {
        return;
      }
      if (scope_is_within(binding->scope_index, scope_index)) {
        return;
      }
      if (!seen.insert(binding->id).second) {
        return;
      }
      captures.push_back(binding);
    };

    const auto refs_found = refs_by_scope_.find(scope_index);
    if (refs_found != refs_by_scope_.end()) {
      for (const binder::Reference *ref : refs_found->second) {
        if (ref == nullptr || !ref->resolved) {
          continue;
        }
        add_capture(binding_for_id(ref->binding_id));
      }
    }

    const auto children_found = block_children_by_scope_.find(scope_index);
    if (children_found != block_children_by_scope_.end()) {
      for (int child_scope_index : children_found->second) {
        for (const binder::Binding *binding :
             capture_bindings_for_scope(child_scope_index)) {
          add_capture(binding);
        }
      }
    }

    std::sort(captures.begin(), captures.end(), binding_less);
    return capture_bindings_cache_.emplace(scope_index, std::move(captures))
        .first->second;
  }

  std::vector<CapturePlan> build_capture_plans(int scope_index) const {
    std::vector<CapturePlan> plans;
    if (current_proc_ == nullptr) {
      return plans;
    }
    const auto found = capture_bindings_cache_.find(scope_index);
    if (found == capture_bindings_cache_.end()) {
      return plans;
    }
    for (const binder::Binding *binding : found->second) {
      if (binding == nullptr) {
        continue;
      }
      std::string source_kind;
      std::string source_slot = slot_for_binding(binding->id);
      if (!source_slot.empty()) {
        source_kind = "local";
      } else {
        source_slot = capture_slot_for_binding(binding->id);
        if (!source_slot.empty()) {
          source_kind = "capture";
        }
      }
      if (source_slot.empty()) {
        continue;
      }
      plans.push_back(CapturePlan{binding, "u" + std::to_string(plans.size()),
                                  source_kind, source_slot});
    }
    return plans;
  }

  std::vector<const ast::Expr *> collect_module_exec_items() const {
    std::vector<const ast::Expr *> values;
    for (const std::unique_ptr<ast::Expr> &item : items_) {
      if (item->kind == "AstPackageDecl" || item->kind == "AstImportStmt" ||
          item->kind == "AstExportStmt" || item->kind == "AstClassDef" ||
          item->kind == "AstMixinDef" || item->kind == "AstDefStmt" ||
          item->kind == "AstClassMethodDef" || item->kind == "AstClauseDef") {
        continue;
      }
      values.push_back(item.get());
    }
    return values;
  }

  std::vector<std::unique_ptr<Node>> lower_imports() {
    std::vector<std::unique_ptr<Node>> nodes;
    for (const std::unique_ptr<ast::Expr> &item : items_) {
      if (item->kind != "AstImportStmt") {
        continue;
      }
      const std::string import_kind = string_value(*item, "import_kind");
      if (import_kind == "module") {
        auto node = make_node("HImportModule", item->span);
        node->string_field("module_id", string_value(*item, "module_path"));
        node->string_field("local_name", string_value(*item, "alias"));
        if (string_value(*item, "alias").empty()) {
          node->string_field("local_name", last_path_segment(string_value(
                                               *item, "module_path")));
        }
        nodes.push_back(std::move(node));
        continue;
      }
      auto node = make_node("HImportNames", item->span);
      node->string_field("module_id", string_value(*item, "module_path"));
      std::vector<std::unique_ptr<Node>> names;
      if (const ast::ListField *list = list_field(*item, "names")) {
        for (const std::unique_ptr<ast::Expr> &name : list->values) {
          auto name_node = make_node("HImportName", name->span);
          name_node->string_field("source_name",
                                  string_value(*name, "source_name"));
          name_node->string_field("local_name",
                                  string_value(*name, "local_name"));
          names.push_back(std::move(name_node));
        }
      }
      node->list_field("names", std::move(names));
      nodes.push_back(std::move(node));
    }
    return nodes;
  }

  std::vector<std::unique_ptr<Node>> lower_exports() {
    std::vector<std::unique_ptr<Node>> nodes;
    for (const std::unique_ptr<ast::Expr> &item : items_) {
      if (item->kind != "AstExportStmt") {
        continue;
      }
      const ast::ListField *list = list_field(*item, "items");
      if (list == nullptr) {
        continue;
      }
      for (const std::unique_ptr<ast::Expr> &export_item : list->values) {
        auto node = make_node("HExport", export_item->span);
        node->string_field("local_name",
                           string_value(*export_item, "local_name"));
        node->string_field("public_name",
                           string_value(*export_item, "public_name"));
        nodes.push_back(std::move(node));
      }
    }
    return nodes;
  }

  std::vector<std::unique_ptr<Node>> lower_module_items() {
    std::vector<std::unique_ptr<Node>> nodes;
    for (const std::unique_ptr<ast::Expr> &item : items_) {
      if (item->kind == "AstClassDef" || item->kind == "AstMixinDef" ||
          item->kind == "AstDefStmt" || item->kind == "AstClassMethodDef" ||
          item->kind == "AstClauseDef") {
        nodes.push_back(lower_item_decl(*item, "module"));
      }
    }
    return nodes;
  }

  std::unique_ptr<Node> lower_item_decl(const ast::Expr &item,
                                        const std::string &dispatch_side) {
    if (item.kind == "AstClassDef") {
      return lower_class_like(item, "HClass");
    }
    if (item.kind == "AstMixinDef") {
      return lower_class_like(item, "HMixin");
    }
    if (item.kind == "AstDefStmt" || item.kind == "AstClassMethodDef") {
      const std::string method_dispatch_side =
          item.kind == "AstClassMethodDef" ? "class" : dispatch_side;
      return lower_method_decl(item, method_dispatch_side);
    }
    if (item.kind == "AstClauseDef") {
      return lower_clause_method_decl(item, dispatch_side);
    }
    if (item.kind == "AstIncludeStmt") {
      return lower_include_like(item, "HInclude");
    }
    if (item.kind == "AstExtendStmt") {
      return lower_include_like(item, "HExtend");
    }
    return lower_stmt(item);
  }

  std::unique_ptr<Node> lower_class_like(const ast::Expr &item,
                                         const std::string &kind) {
    auto node = make_node(kind, item.span);
    node->string_field("name", string_value(item, "name"));
    if (!string_value(item, "superclass").empty()) {
      node->string_field("superclass", string_value(item, "superclass"));
    }
    std::vector<std::unique_ptr<Node>> body;
    if (const ast::ListField *items = list_field(item, "body")) {
      const std::string dispatch_side =
          kind == "HClass" ? "instance" : "instance";
      for (const std::unique_ptr<ast::Expr> &child : items->values) {
        body.push_back(lower_item_decl(*child, dispatch_side));
      }
    }
    node->list_field("body", std::move(body));
    return node;
  }

  std::unique_ptr<Node> lower_include_like(const ast::Expr &item,
                                           const std::string &kind) {
    auto node = make_node(kind, item.span);
    std::vector<std::unique_ptr<Node>> paths;
    if (const ast::ListField *list = list_field(item, "paths")) {
      for (const std::unique_ptr<ast::Expr> &path : list->values) {
        auto path_node = make_node("HPath", path->span);
        path_node->string_field("path", string_value(*path, "path"));
        paths.push_back(std::move(path_node));
      }
    }
    node->list_field("paths", std::move(paths));
    return node;
  }

  std::unique_ptr<Node> lower_method_decl(const ast::Expr &item,
                                          const std::string &dispatch_side) {
    const std::string scope_kind =
        item.kind == "AstClassMethodDef" ? "class_method" : "function";
    const int scope_index =
        find_scope_index(scope_kind, item.span, string_value(item, "name"));
    const binder::Signature *signature = signature_for_scope(scope_index);
    auto node = make_node("HMethod", item.span);
    node->string_field("name", string_value(item, "name"));
    node->string_field("dispatch_side", dispatch_side);
    node->list_field("auto_assign", build_auto_assign_nodes(signature));
    std::unique_ptr<Node> signature_node =
        build_signature_node(signature, item.span);
    if (scope_index >= 0) {
      const ast::ListField *body = list_field(item, "body");
      std::vector<const ast::Expr *> body_items;
      if (body != nullptr) {
        for (const std::unique_ptr<ast::Expr> &stmt : body->values) {
          body_items.push_back(stmt.get());
        }
      }
      const std::string procedure_id = lower_procedure(
          scope_index, string_value(item, "name"), "method",
          procedure_name_for_owner(string_value(item, "name")), signature,
          body_items, item.span, {}, nullptr, node_field(item, "signature"));
      node->string_field("procedure", procedure_id);
      for (const Procedure &procedure : procedures_) {
        if (procedure.id == procedure_id && procedure.signature != nullptr) {
          signature_node = clone_node(*procedure.signature);
          break;
        }
      }
    }
    node->node_field("signature", std::move(signature_node));
    return node;
  }

  std::unique_ptr<Node>
  lower_clause_method_decl(const ast::Expr &item,
                           const std::string &dispatch_side) {
    const int scope_index =
        find_scope_index("function", item.span, string_value(item, "name"));
    const binder::Signature *signature = signature_for_scope(scope_index);
    auto node = make_node("HMethod", item.span);
    node->string_field("name", string_value(item, "name"));
    node->string_field("dispatch_side", dispatch_side);
    node->list_field("auto_assign", build_auto_assign_nodes(signature));
    std::unique_ptr<Node> signature_node =
        build_signature_node(signature, item.span);

    ProcedureContext context;
    ProcedureContext *saved = current_proc_;
    bool has_context = false;
    if (scope_index >= 0) {
      Procedure procedure;
      procedure.id = "p" + std::to_string(procedures_.size());
      procedure.name = string_value(item, "name");
      procedure.kind = "method";
      procedure.owner = procedure_name_for_owner(string_value(item, "name"));
      procedure.span = item.span;
      procedure.signature = build_signature_node(signature, item.span);
      procedure.body = make_node("HSeq", item.span);
      procedures_.push_back(std::move(procedure));

      context.scope_index = scope_index;
      context.procedure_index = procedures_.size() - 1;
      initialize_locals(&context);
      current_proc_ = &context;
      lower_signature_defaults(
          procedures_[context.procedure_index].signature.get(),
          node_field(item, "base_signature"));
      signature_node =
          clone_node(*procedures_[context.procedure_index].signature);
      has_context = true;
      node->string_field("procedure", procedures_[context.procedure_index].id);
    }

    std::vector<std::unique_ptr<Node>> clauses;
    if (const ast::ListField *list = list_field(item, "clauses")) {
      for (const std::unique_ptr<ast::Expr> &clause : list->values) {
        clauses.push_back(lower_clause_node(*clause, signature));
      }
    }
    node->node_field("signature", std::move(signature_node));
    node->list_field("clauses", std::move(clauses));
    node->node_field("else_body",
                     lower_body(list_field(item, "else_body"), item.span));

    if (has_context) {
      current_proc_ = saved;
    }
    return node;
  }

  std::unique_ptr<Node> lower_clause_node(const ast::Expr &clause,
                                          const binder::Signature *signature) {
    auto node = make_node("HClause", clause.span);
    std::unique_ptr<Node> pattern_node = pattern::parse_pattern_text(
        string_value(clause, "pattern"), clause.span);
    std::unique_ptr<Node> compiled_pattern =
        pattern::compile_pattern_ir(*pattern_node);
    attach_pattern_matcher_exprs(compiled_pattern.get());
    node->string_field("subject_kind",
                       clause_subject_kind(*pattern_node, signature));
    node->node_field("pattern", std::move(pattern_node));
    node->node_field("compiled_pattern", std::move(compiled_pattern));
    if (const ast::Expr *guard = node_field(clause, "guard_expr")) {
      node->node_field("guard", lower_expr(*guard));
    }
    node->node_field("body",
                     lower_body(list_field(clause, "body"), clause.span));
    return node;
  }

  std::string clause_subject_kind(const ast::Expr &pattern,
                                  const binder::Signature *signature) const {
    if (pattern::is_map_subject_pattern(pattern)) {
      return "named_args_map";
    }
    if (pattern::is_tuple_subject_pattern(pattern)) {
      return "positional_tuple";
    }

    if (signature != nullptr && signature->params.size() == 1 &&
        signature->params[0].kind == "positional") {
      return "single_positional";
    }
    return "positional_tuple";
  }

  std::unique_ptr<Node> build_signature_node(const binder::Signature *signature,
                                             const lexer::Span &fallback_span) {
    auto node = make_node("HSignature", fallback_span);
    std::vector<std::unique_ptr<Node>> params;
    if (signature != nullptr) {
      node->span = signature->span;
      if (!signature->return_type_expr.empty()) {
        node->string_field("return_type_expr", signature->return_type_expr);
      }
      for (const binder::ParamDescriptor &param : signature->params) {
        auto param_node = make_node("HParam", param.span);
        param_node->string_field("external_name", param.external_name);
        param_node->string_field("local_name", param.local_name);
        param_node->string_field("kind", param.kind);
        param_node->string_field("auto_assign_kind", param.auto_assign_kind);
        param_node->string_field("auto_assign_target",
                                 param.auto_assign_target);
        param_node->string_field("type_expr", param.type_expr);
        param_node->bool_field("has_default", param.has_default);
        param_node->string_field("default_kind", param.default_kind);
        params.push_back(std::move(param_node));
      }
    }
    node->list_field("params", std::move(params));
    return node;
  }

  void attach_pattern_matcher_exprs(ast::Expr *compiled_pattern) {
    if (compiled_pattern == nullptr) {
      return;
    }
    attach_pattern_matcher_exprs_impl(compiled_pattern);
  }

  void attach_pattern_matcher_exprs_impl(ast::Expr *node) {
    if (node == nullptr) {
      return;
    }
    if (node->kind == "PIrMatcherExpr" || node->kind == "PMatcherExpr") {
      attach_matcher_expr(node, "expr_text");
    } else if (node->kind == "PIrDynamic" || node->kind == "PDynamic") {
      attach_matcher_expr(node, "matcher_text");
    }
    for (ast::NodeField &field : node->node_fields) {
      attach_pattern_matcher_exprs_impl(field.value.get());
    }
    for (ast::ListField &field : node->list_fields) {
      for (std::unique_ptr<ast::Expr> &value : field.values) {
        attach_pattern_matcher_exprs_impl(value.get());
      }
    }
  }

  void attach_matcher_expr(ast::Expr *node, const std::string &field_name) {
    if (node == nullptr) {
      return;
    }
    std::unique_ptr<Node> parsed_expr = pattern::parse_matcher_expression(
        string_value(*node, field_name), node->span);
    if (parsed_expr == nullptr) {
      return;
    }
    node->node_field("matcher_expr", lower_expr(*parsed_expr));
  }

  void lower_signature_defaults(ast::Expr *signature_node,
                                const ast::Expr *ast_signature) {
    if (signature_node == nullptr || ast_signature == nullptr) {
      return;
    }
    ast::ListField *hir_params = mutable_list_field(*signature_node, "params");
    const ast::ListField *ast_params = list_field(*ast_signature, "params");
    if (hir_params == nullptr || ast_params == nullptr) {
      return;
    }
    const std::size_t count =
        std::min(hir_params->values.size(), ast_params->values.size());
    for (std::size_t i = 0; i < count; ++i) {
      ast::Expr *hir_param = hir_params->values[i].get();
      if (hir_param == nullptr) {
        continue;
      }
      const ast::Expr *ast_param = ast_params->values[i].get();
      const ast::Expr *default_expr =
          ast_param == nullptr ? nullptr
                               : node_field(*ast_param, "default_expr");
      if (default_expr != nullptr) {
        hir_param->node_field("default_expr", lower_expr(*default_expr));
      }
    }
  }

  std::vector<std::unique_ptr<Node>>
  build_auto_assign_nodes(const binder::Signature *signature) {
    std::vector<std::unique_ptr<Node>> nodes;
    if (signature == nullptr) {
      return nodes;
    }
    for (const binder::ParamDescriptor &param : signature->params) {
      if (!signature_param_has_auto_assign(param)) {
        continue;
      }
      auto node = make_node("HAutoAssign", param.span);
      node->string_field("local_name", param.local_name);
      node->string_field("target", param.auto_assign_target);
      node->string_field("target_kind",
                         param.auto_assign_kind == "@" ? "ivar" : "cvar");
      nodes.push_back(std::move(node));
    }
    return nodes;
  }

  std::string
  lower_module_procedure(int scope_index,
                         const std::vector<const ast::Expr *> &body_items) {
    return lower_procedure(scope_index, "__module_init__", "module_init",
                           "__module_init__", nullptr, body_items,
                           module_span());
  }

  std::string lower_procedure(
      int scope_index, const std::string &name, const std::string &kind,
      const std::string &owner, const binder::Signature *signature,
      const std::vector<const ast::Expr *> &body_items, const lexer::Span &span,
      const std::vector<CapturePlan> &capture_plans = {},
      const ast::Expr *signature_override = nullptr,
      const ast::Expr *ast_signature = nullptr) {
    Procedure procedure;
    procedure.id = "p" + std::to_string(procedures_.size());
    procedure.name = name;
    procedure.kind = kind;
    procedure.owner = owner;
    procedure.span = span;
    procedure.signature = signature_override != nullptr
                              ? clone_node(*signature_override)
                              : build_signature_node(signature, span);
    procedure.body = make_node("HSeq", span);
    procedures_.push_back(std::move(procedure));

    const std::size_t procedure_index = procedures_.size() - 1;
    ProcedureContext context;
    context.scope_index = scope_index;
    context.procedure_index = procedure_index;
    initialize_locals(&context);
    initialize_captures(&context, capture_plans);

    ProcedureContext *saved = current_proc_;
    current_proc_ = &context;
    lower_signature_defaults(procedures_[procedure_index].signature.get(),
                             ast_signature);
    std::vector<std::unique_ptr<Node>> statements;
    for (const ast::Expr *item : body_items) {
      statements.push_back(lower_stmt(*item));
    }
    procedures_[procedure_index].body->list_field("items",
                                                  std::move(statements));
    current_proc_ = saved;

    return procedures_[procedure_index].id;
  }

  void initialize_locals(ProcedureContext *context) {
    if (context->scope_index < 0) {
      return;
    }
    std::vector<const binder::Binding *> bindings;
    collect_local_bindings_for_procedure(context->scope_index,
                                         context->scope_index, &bindings);
    std::sort(bindings.begin(), bindings.end(), binding_less);
    const ast::ListField *signature_params =
        list_field(*procedures_[context->procedure_index].signature, "params");
    if (signature_params != nullptr) {
      for (const std::unique_ptr<ast::Expr> &param : signature_params->values) {
        const std::string local_name = string_value(*param, "local_name");
        if (local_name.empty()) {
          continue;
        }
        bool has_binding = false;
        for (const binder::Binding *binding_ptr : bindings) {
          if (binding_ptr != nullptr && binding_ptr->name == local_name) {
            has_binding = true;
            break;
          }
        }
        if (has_binding) {
          continue;
        }
        const std::string slot = "l" + std::to_string(context->next_slot++);
        procedures_[context->procedure_index].locals.push_back(ProcedureLocal{
            slot, local_name, "param", "synthetic", param->span});
      }
    }
    for (const binder::Binding *binding_ptr : bindings) {
      if (binding_ptr == nullptr) {
        continue;
      }
      const binder::Binding &binding = *binding_ptr;
      if (!binding_has_slot(binding)) {
        continue;
      }
      const std::string slot = "l" + std::to_string(context->next_slot++);
      context->slot_by_binding_id.emplace(binding.id, slot);
      procedures_[context->procedure_index].locals.push_back(ProcedureLocal{
          slot, binding.name, binding.role, binding.kind, binding.span});
    }
  }

  void collect_local_bindings_for_procedure(
      int root_scope_index, int scope_index,
      std::vector<const binder::Binding *> *bindings) const {
    if (scope_index < 0) {
      return;
    }
    const binder::Scope &scope = graph_.scopes[scope_index];
    for (const std::string &binding_id : scope.bindings) {
      const binder::Binding *binding = binding_for_id(binding_id);
      if (binding != nullptr) {
        bindings->push_back(binding);
      }
    }

    const auto children_found = block_children_by_scope_.find(scope_index);
    if (children_found == block_children_by_scope_.end()) {
      return;
    }
    for (int child_scope_index : children_found->second) {
      if (child_scope_index < 0) {
        continue;
      }
      const binder::Scope &child_scope = graph_.scopes[child_scope_index];
      if (child_scope_index != root_scope_index &&
          child_scope.owner == "block_suffix") {
        continue;
      }
      collect_local_bindings_for_procedure(root_scope_index, child_scope_index,
                                           bindings);
    }
  }

  void initialize_captures(ProcedureContext *context,
                           const std::vector<CapturePlan> &capture_plans) {
    for (const CapturePlan &capture : capture_plans) {
      if (capture.binding == nullptr) {
        continue;
      }
      context->capture_slot_by_binding_id.emplace(capture.binding->id,
                                                  capture.slot);
      procedures_[context->procedure_index].captures.push_back(ProcedureCapture{
          capture.slot, capture.binding->name, capture.source_kind,
          capture.source_slot, capture.binding->name, capture.binding->span});
    }
  }

  std::string allocate_temp_local(const lexer::Span &span) {
    if (current_proc_ == nullptr) {
      return "";
    }
    const std::string slot = "l" + std::to_string(current_proc_->next_slot++);
    const std::string name =
        "__tmp" + std::to_string(current_proc_->next_temp_id++);
    procedures_[current_proc_->procedure_index].locals.push_back(
        ProcedureLocal{slot, name, "temp", "synthetic", span});
    return slot;
  }

  std::unique_ptr<Node> make_load_local(const std::string &slot,
                                        const lexer::Span &span) {
    auto node = make_node("HLoadLocal", span);
    node->string_field("slot", slot);
    return node;
  }

  std::unique_ptr<Node> make_null_const(const lexer::Span &span) {
    auto node = make_node("HConst", span);
    node->string_field("token", "KEYWORD_NULL");
    node->string_field("value", "null");
    return node;
  }

  std::unique_ptr<Node> make_expr_body(std::unique_ptr<Node> expr) {
    auto body = make_node("HSeq", expr->span);
    std::vector<std::unique_ptr<Node>> items;
    auto stmt = make_node("HLastSet", expr->span);
    stmt->node_field("expr", std::move(expr));
    items.push_back(std::move(stmt));
    body->list_field("items", std::move(items));
    return body;
  }

  std::unique_ptr<Node> wrap_safe_guard(std::unique_ptr<Node> current,
                                        const std::string &temp_slot,
                                        std::unique_ptr<Node> else_expr,
                                        const lexer::Span &span) {
    auto store = make_node("HStoreLocal", current->span);
    store->string_field("slot", temp_slot);
    store->node_field("expr", std::move(current));

    auto cond = make_node("HIsNull", store->span);
    cond->node_field("expr", std::move(store));

    auto node = make_node("HIf", span);
    node->node_field("cond", std::move(cond));
    node->node_field("then_body", make_expr_body(make_null_const(span)));
    node->node_field("else_body", make_expr_body(std::move(else_expr)));
    return node;
  }

  std::unique_ptr<Node> lower_stmt(const ast::Expr &item) {
    if (item.kind == "AstExprStmt") {
      auto node = make_node("HLastSet", item.span);
      if (const ast::Expr *expr = node_field(item, "expr")) {
        node->node_field("expr", lower_expr(*expr));
      }
      return node;
    }
    if (item.kind == "AstPassStmt" || item.kind == "AstNoopStmt") {
      return make_node("HSeq", item.span);
    }
    if (item.kind == "AstClassDef" || item.kind == "AstMixinDef" ||
        item.kind == "AstDefStmt" || item.kind == "AstClassMethodDef" ||
        item.kind == "AstClauseDef" || item.kind == "AstIncludeStmt" ||
        item.kind == "AstExtendStmt") {
      return lower_item_decl(item, "module");
    }
    auto node = make_node("HLastSet", item.span);
    node->node_field("expr", lower_expr(item));
    return node;
  }

  std::unique_ptr<Node> lower_expr(const ast::Expr &expr) {
    if (expr.kind == "AstLiteral") {
      auto node = make_node("HConst", expr.span);
      node->string_field("token", string_value(expr, "token"));
      node->string_field("value", string_value(expr, "value"));
      return node;
    }
    if (expr.kind == "AstName") {
      return lower_name(expr, "name");
    }
    if (expr.kind == "AstPlaceholder") {
      return lower_name(expr, "placeholder");
    }
    if (expr.kind == "AstLastValue") {
      return make_node("HLastGet", expr.span);
    }
    if (expr.kind == "AstIvar") {
      auto node = make_node("HLoadIvar", expr.span);
      node->string_field("name", string_value(expr, "name"));
      return node;
    }
    if (expr.kind == "AstCvar") {
      auto node = make_node("HLoadCvar", expr.span);
      node->string_field("name", string_value(expr, "name"));
      return node;
    }
    if (expr.kind == "AstUnary") {
      auto node = make_node("HSend", expr.span);
      node->node_field("receiver",
                       lower_expr(*node_field_required(expr, "operand")));
      node->string_field("selector",
                         map_unary_selector(string_value(expr, "op")));
      node->list_field("pos_args", {});
      node->list_field("kw_args", {});
      return node;
    }
    if (expr.kind == "AstBinary") {
      auto node = make_node("HSend", expr.span);
      node->node_field("receiver",
                       lower_expr(*node_field_required(expr, "left")));
      node->string_field("selector", string_value(expr, "op"));
      std::vector<std::unique_ptr<Node>> pos_args;
      pos_args.push_back(lower_expr(*node_field_required(expr, "right")));
      node->list_field("pos_args", std::move(pos_args));
      node->list_field("kw_args", {});
      return node;
    }
    if (expr.kind == "AstAssign") {
      return lower_assign(expr);
    }
    if (expr.kind == "AstPatternAssign") {
      return lower_pattern_assign(expr);
    }
    if (expr.kind == "AstIf") {
      auto node = make_node("HIf", expr.span);
      node->node_field("cond", lower_expr(*node_field_required(expr, "cond")));
      node->node_field("then_body",
                       lower_body(list_field(expr, "then_body"), expr.span));
      node->node_field("else_body",
                       lower_body(list_field(expr, "else_body"), expr.span));
      return node;
    }
    if (expr.kind == "AstUnless") {
      auto node = make_node("HIf", expr.span);
      auto negated = make_node("HSend", expr.span);
      negated->node_field("receiver",
                          lower_expr(*node_field_required(expr, "cond")));
      negated->string_field("selector", "not");
      negated->list_field("pos_args", {});
      negated->list_field("kw_args", {});
      node->node_field("cond", std::move(negated));
      node->node_field("then_body",
                       lower_body(list_field(expr, "then_body"), expr.span));
      node->node_field("else_body",
                       lower_body(list_field(expr, "else_body"), expr.span));
      return node;
    }
    if (expr.kind == "AstWhile" || expr.kind == "AstUntil" ||
        expr.kind == "AstDoWhile" || expr.kind == "AstLoop") {
      auto node = make_node("HLoop", expr.span);
      node->string_field("kind", map_loop_kind(expr.kind));
      if (const ast::Expr *cond = node_field(expr, "cond")) {
        node->node_field("cond", lower_expr(*cond));
      }
      node->node_field("body", lower_body(list_field(expr, "body"), expr.span));
      return node;
    }
    if (expr.kind == "AstBreak") {
      auto node = make_node("HBreak", expr.span);
      if (const ast::Expr *value = node_field(expr, "value")) {
        node->node_field("value", lower_expr(*value));
      }
      return node;
    }
    if (expr.kind == "AstCase") {
      auto node = make_node("HMatchDispatch", expr.span);
      node->node_field("scrutinee",
                       lower_expr(*node_field_required(expr, "scrutinee")));
      std::vector<std::unique_ptr<Node>> arms;
      if (const ast::ListField *list = list_field(expr, "arms")) {
        for (const std::unique_ptr<ast::Expr> &arm : list->values) {
          arms.push_back(lower_expr(*arm));
        }
      }
      node->list_field("arms", std::move(arms));
      node->node_field("else_body",
                       lower_body(list_field(expr, "else_body"), expr.span));
      node->string_field("fail_mode",
                         bool_value(expr, "strict") ? "match_error" : "null");
      return node;
    }
    if (expr.kind == "AstCaseArm") {
      auto node = make_node("HMatchArm", expr.span);
      std::unique_ptr<Node> pattern_node =
          pattern::parse_pattern_text(string_value(expr, "pattern"), expr.span);
      std::unique_ptr<Node> compiled_pattern =
          pattern::compile_pattern_ir(*pattern_node);
      attach_pattern_matcher_exprs(compiled_pattern.get());
      node->node_field("pattern", std::move(pattern_node));
      node->node_field("compiled_pattern", std::move(compiled_pattern));
      if (const ast::Expr *guard = node_field(expr, "guard_expr")) {
        node->node_field("guard", lower_expr(*guard));
      }
      node->node_field("body", lower_body(list_field(expr, "body"), expr.span));
      return node;
    }
    if (expr.kind == "AstPostfixChain") {
      return lower_postfix_chain(expr);
    }
    if (expr.kind == "AstBlock") {
      return lower_block(expr);
    }
    if (expr.kind == "AstKeywordArg") {
      auto node = make_node("HKeywordArg", expr.span);
      node->string_field("name", string_value(expr, "name"));
      if (const ast::Expr *value = node_field(expr, "value")) {
        node->node_field("value", lower_expr(*value));
      }
      return node;
    }
    auto node = make_node("HUnsupported", expr.span);
    node->string_field("source_kind", expr.kind);
    return node;
  }

  const ast::Expr *node_field_required(const ast::Expr &expr,
                                       const std::string &name) const {
    const ast::Expr *value = node_field(expr, name);
    if (value != nullptr) {
      return value;
    }
    return &expr;
  }

  std::unique_ptr<Node> lower_body(const ast::ListField *items,
                                   const lexer::Span &fallback_span) {
    auto node = make_node("HSeq", fallback_span);
    std::vector<std::unique_ptr<Node>> values;
    if (items != nullptr) {
      for (const std::unique_ptr<ast::Expr> &item : items->values) {
        values.push_back(lower_stmt(*item));
      }
      if (!items->values.empty()) {
        node->span = ast::join_spans(items->values.front()->span,
                                     items->values.back()->span);
      }
    }
    node->list_field("items", std::move(values));
    return node;
  }

  std::unique_ptr<Node> lower_name(const ast::Expr &expr,
                                   const std::string &ref_kind) {
    const binder::Reference *ref =
        find_reference(expr.span, string_value(expr, "name"), ref_kind);
    if (ref == nullptr || !ref->resolved) {
      auto node = make_node("HLoadName", expr.span);
      node->string_field("name", string_value(expr, "name"));
      return node;
    }
    const auto binding_it = bindings_by_id_.find(ref->binding_id);
    if (binding_it == bindings_by_id_.end()) {
      auto node = make_node("HLoadName", expr.span);
      node->string_field("name", string_value(expr, "name"));
      return node;
    }
    const binder::Binding &binding = *binding_it->second;
    if (binding.kind == "constant") {
      auto node = make_node("HLoadConst", expr.span);
      node->string_field("path", binding.name);
      return node;
    }
    const std::string slot = slot_for_binding(binding.id);
    if (!slot.empty()) {
      auto node = make_node("HLoadLocal", expr.span);
      node->string_field("slot", slot);
      return node;
    }
    const std::string capture_slot = capture_slot_for_binding(binding.id);
    if (!capture_slot.empty()) {
      auto node = make_node("HLoadCapture", expr.span);
      node->string_field("slot", capture_slot);
      return node;
    }
    auto node = make_node("HLoadName", expr.span);
    node->string_field("name", string_value(expr, "name"));
    return node;
  }

  std::unique_ptr<Node> lower_assign(const ast::Expr &expr) {
    const ast::Expr *left = node_field(expr, "left");
    const ast::Expr *right = node_field(expr, "right");
    if (left == nullptr || right == nullptr) {
      auto node = make_node("HUnsupported", expr.span);
      node->string_field("source_kind", expr.kind);
      return node;
    }

    if (left->kind == "AstName") {
      const binder::Reference *ref =
          find_reference(left->span, string_value(*left, "name"), "write");
      const std::string slot = ref != nullptr && ref->resolved
                                   ? slot_for_binding(ref->binding_id)
                                   : "";
      const std::string capture_slot =
          ref != nullptr && ref->resolved
              ? capture_slot_for_binding(ref->binding_id)
              : "";
      if (!capture_slot.empty()) {
        auto node = make_node("HStoreCapture", expr.span);
        node->string_field("slot", capture_slot);
        node->node_field("expr", lower_expr(*right));
        return node;
      }
      auto node = make_node("HStoreLocal", expr.span);
      node->string_field("slot",
                         slot.empty() ? string_value(*left, "name") : slot);
      node->node_field("expr", lower_expr(*right));
      return node;
    }
    if (left->kind == "AstIvar") {
      auto node = make_node("HStoreIvar", expr.span);
      node->string_field("name", string_value(*left, "name"));
      node->node_field("expr", lower_expr(*right));
      return node;
    }
    if (left->kind == "AstCvar") {
      auto node = make_node("HStoreCvar", expr.span);
      node->string_field("name", string_value(*left, "name"));
      node->node_field("expr", lower_expr(*right));
      return node;
    }

    auto node = make_node("HUnsupported", expr.span);
    node->string_field("source_kind", "assign:" + left->kind);
    return node;
  }

  std::unique_ptr<Node> lower_pattern_assign(const ast::Expr &expr) {
    auto node = make_node("HPatternAssign", expr.span);
    std::unique_ptr<Node> pattern_node =
        pattern::parse_pattern_text(string_value(expr, "pattern"), expr.span);
    std::unique_ptr<Node> compiled_pattern =
        pattern::compile_pattern_ir(*pattern_node);
    attach_pattern_matcher_exprs(compiled_pattern.get());
    node->node_field("pattern", std::move(pattern_node));
    node->node_field("compiled_pattern", std::move(compiled_pattern));
    node->node_field("value", lower_expr(*node_field_required(expr, "right")));
    node->string_field("fail_mode", "match_error");
    return node;
  }

  std::unique_ptr<Node> lower_postfix_chain(const ast::Expr &expr) {
    const ast::Expr *base = node_field(expr, "base");
    const ast::ListField *tails = list_field(expr, "tails");
    if (base == nullptr) {
      auto node = make_node("HUnsupported", expr.span);
      node->string_field("source_kind", expr.kind);
      return node;
    }
    std::unique_ptr<Node> current = lower_expr(*base);
    if (tails == nullptr) {
      return current;
    }

    std::size_t i = 0;
    while (i < tails->values.size()) {
      const ast::Expr &tail = *tails->values[i];
      if (i == 0 && tail.kind == "AstTailCall" && is_builtin_send_base(*base)) {
        std::unique_ptr<Node> block;
        if (i + 1 < tails->values.size() &&
            tails->values[i + 1]->kind == "AstTailBlockSuffix") {
          block = lower_block_suffix(*tails->values[i + 1]);
          ++i;
        }
        std::unique_ptr<Node> lowered =
            lower_builtin_send_call(*base, tail, std::move(block));
        if (lowered) {
          current = std::move(lowered);
          ++i;
          continue;
        }
      }
      if (tail.kind == "AstTailDotMember" || tail.kind == "AstTailSafeMember") {
        const bool safe = tail.kind == "AstTailSafeMember";
        const std::string selector = string_value(tail, "name");
        std::vector<std::unique_ptr<Node>> pos_args;
        std::vector<std::unique_ptr<Node>> kw_args;
        std::unique_ptr<Node> block;
        if (i + 1 < tails->values.size() &&
            tails->values[i + 1]->kind == "AstTailCall") {
          collect_call_args(*tails->values[i + 1], &pos_args, &kw_args);
          ++i;
        }
        if (i + 1 < tails->values.size() &&
            tails->values[i + 1]->kind == "AstTailBlockSuffix") {
          block = lower_block_suffix(*tails->values[i + 1]);
          ++i;
        }
        const lexer::Span node_span = ast::join_spans(current->span, tail.span);
        std::unique_ptr<Node> guard_base;
        std::unique_ptr<Node> receiver;
        std::string temp_slot;
        if (safe) {
          guard_base = std::move(current);
          temp_slot = allocate_temp_local(guard_base->span);
          receiver = make_load_local(temp_slot, guard_base->span);
        } else {
          receiver = std::move(current);
        }
        auto node = make_node("HSend", node_span);
        node->node_field("receiver", std::move(receiver));
        node->string_field("selector", selector);
        node->list_field("pos_args", std::move(pos_args));
        node->list_field("kw_args", std::move(kw_args));
        if (block) {
          node->node_field("block", std::move(block));
        }
        current = safe ? wrap_safe_guard(std::move(guard_base), temp_slot,
                                         std::move(node), node_span)
                       : std::move(node);
        ++i;
        continue;
      }
      if (tail.kind == "AstTailCall" || tail.kind == "AstTailSafeCall") {
        const bool safe = tail.kind == "AstTailSafeCall";
        std::vector<std::unique_ptr<Node>> pos_args;
        std::vector<std::unique_ptr<Node>> kw_args;
        std::unique_ptr<Node> block;
        collect_call_args(tail, &pos_args, &kw_args);
        if (i + 1 < tails->values.size() &&
            tails->values[i + 1]->kind == "AstTailBlockSuffix") {
          block = lower_block_suffix(*tails->values[i + 1]);
          ++i;
        }
        const lexer::Span node_span = ast::join_spans(current->span, tail.span);
        std::unique_ptr<Node> guard_base;
        std::unique_ptr<Node> callable;
        std::string temp_slot;
        if (safe) {
          guard_base = std::move(current);
          temp_slot = allocate_temp_local(guard_base->span);
          callable = make_load_local(temp_slot, guard_base->span);
        } else {
          callable = std::move(current);
        }
        auto node = make_node("HCall", node_span);
        node->node_field("callable", std::move(callable));
        node->list_field("pos_args", std::move(pos_args));
        node->list_field("kw_args", std::move(kw_args));
        if (block) {
          node->node_field("block", std::move(block));
        }
        current = safe ? wrap_safe_guard(std::move(guard_base), temp_slot,
                                         std::move(node), node_span)
                       : std::move(node);
        ++i;
        continue;
      }
      if (tail.kind == "AstTailIndex" || tail.kind == "AstTailSafeIndex") {
        const bool safe = tail.kind == "AstTailSafeIndex";
        const lexer::Span node_span = ast::join_spans(current->span, tail.span);
        std::unique_ptr<Node> guard_base;
        std::unique_ptr<Node> receiver;
        std::string temp_slot;
        if (safe) {
          guard_base = std::move(current);
          temp_slot = allocate_temp_local(guard_base->span);
          receiver = make_load_local(temp_slot, guard_base->span);
        } else {
          receiver = std::move(current);
        }
        auto node = make_node("HIndex", node_span);
        node->node_field("receiver", std::move(receiver));
        if (const ast::Expr *index_expr = node_field(tail, "index_expr")) {
          node->node_field("index_expr", lower_expr(*index_expr));
        }
        current = safe ? wrap_safe_guard(std::move(guard_base), temp_slot,
                                         std::move(node), node_span)
                       : std::move(node);
        ++i;
        continue;
      }
      if (tail.kind == "AstTailBlockSuffix") {
        ++i;
        continue;
      }
      ++i;
    }

    return current;
  }

  bool is_builtin_send_base(const ast::Expr &base) const {
    if (base.kind != "AstName" || string_value(base, "name") != "send") {
      return false;
    }
    const binder::Reference *ref = find_reference(base.span, "send", "name");
    return ref == nullptr || !ref->resolved;
  }

  std::unique_ptr<Node> lower_builtin_send_call(const ast::Expr &base,
                                                const ast::Expr &tail,
                                                std::unique_ptr<Node> block) {
    const ast::ListField *args = list_field(tail, "args");
    if (args == nullptr || args->values.size() < 2) {
      return nullptr;
    }
    if (args->values[0]->kind == "AstKeywordArg" ||
        args->values[1]->kind == "AstKeywordArg") {
      return nullptr;
    }

    const ast::Expr &receiver_arg = *args->values[0];
    const ast::Expr &selector_arg = *args->values[1];
    const lexer::Span span = ast::join_spans(base.span, tail.span);

    std::vector<std::unique_ptr<Node>> pos_args;
    std::vector<std::unique_ptr<Node>> kw_args;
    for (std::size_t arg_i = 2; arg_i < args->values.size(); ++arg_i) {
      const ast::Expr &arg = *args->values[arg_i];
      if (arg.kind == "AstKeywordArg") {
        kw_args.push_back(lower_expr(arg));
      } else {
        pos_args.push_back(lower_expr(arg));
      }
    }

    if (is_string_literal(selector_arg)) {
      auto node = make_node("HSend", span);
      node->node_field("receiver", lower_expr(receiver_arg));
      node->string_field("selector", unquote_string_literal(
                                         string_value(selector_arg, "value")));
      node->list_field("pos_args", std::move(pos_args));
      node->list_field("kw_args", std::move(kw_args));
      if (block) {
        node->node_field("block", std::move(block));
      }
      return node;
    }

    auto node = make_node("HSendDyn", span);
    node->node_field("receiver", lower_expr(receiver_arg));
    node->node_field("selector_expr", lower_expr(selector_arg));
    node->list_field("pos_args", std::move(pos_args));
    node->list_field("kw_args", std::move(kw_args));
    if (block) {
      node->node_field("block", std::move(block));
    }
    return node;
  }

  void collect_call_args(const ast::Expr &tail,
                         std::vector<std::unique_ptr<Node>> *pos_args,
                         std::vector<std::unique_ptr<Node>> *kw_args) {
    const ast::ListField *args = list_field(tail, "args");
    if (args == nullptr) {
      return;
    }
    for (const std::unique_ptr<ast::Expr> &arg : args->values) {
      if (arg->kind == "AstKeywordArg") {
        kw_args->push_back(lower_expr(*arg));
      } else {
        pos_args->push_back(lower_expr(*arg));
      }
    }
  }

  std::unique_ptr<Node> lower_block_suffix(const ast::Expr &tail) {
    const ast::Expr *block = node_field(tail, "block");
    if (block == nullptr) {
      return make_node("HUnsupported", tail.span);
    }
    return lower_block(*block);
  }

  std::unique_ptr<Node> lower_block(const ast::Expr &expr) {
    const int scope_index =
        find_scope_index("block", expr.span, "block_suffix");
    auto node = make_node("HClosure", expr.span);
    const binder::Signature *signature = nullptr;
    std::unique_ptr<Node> signature_node;
    if (scope_index >= 0) {
      capture_bindings_for_scope(scope_index);
      const std::vector<CapturePlan> capture_plans =
          build_capture_plans(scope_index);
      signature_node = build_block_signature(expr, scope_index);
      std::vector<std::unique_ptr<Node>> param_patterns =
          build_block_param_patterns(expr);
      std::vector<const ast::Expr *> body_items;
      if (const ast::Expr *body_expr = node_field(expr, "body")) {
        auto wrapper = make_node("AstExprStmt", body_expr->span);
        wrapper->node_field("expr", clone_node(*body_expr));
        synthetic_items_.push_back(std::move(wrapper));
        body_items.push_back(synthetic_items_.back().get());
      }
      const std::string procedure_id = lower_procedure(
          scope_index, "__block__", "closure", "__block__", signature,
          body_items, expr.span, capture_plans, signature_node.get());
      node->string_field("procedure", procedure_id);
      for (Procedure &procedure : procedures_) {
        if (procedure.id == procedure_id) {
          procedure.param_patterns = clone_node_list(param_patterns);
          break;
        }
      }
      if (signature_node) {
        node->node_field("signature", std::move(signature_node));
      }
      node->list_field("param_patterns", std::move(param_patterns));
      node->list_field("captures", build_capture_nodes(capture_plans));
      return node;
    }
    node->list_field("param_patterns", {});
    node->list_field("captures", {});
    return node;
  }

  std::vector<std::unique_ptr<Node>>
  build_capture_nodes(const std::vector<CapturePlan> &capture_plans) {
    std::vector<std::unique_ptr<Node>> nodes;
    for (const CapturePlan &capture : capture_plans) {
      if (capture.binding == nullptr) {
        continue;
      }
      auto node = make_node("HCapture", capture.binding->span);
      node->string_field("slot", capture.slot);
      node->string_field("name", capture.binding->name);
      node->string_field("source_kind", capture.source_kind);
      node->string_field("source_slot", capture.source_slot);
      node->string_field("source_name", capture.binding->name);
      nodes.push_back(std::move(node));
    }
    return nodes;
  }

  std::vector<std::unique_ptr<Node>>
  build_block_param_patterns(const ast::Expr &expr) {
    std::vector<std::unique_ptr<Node>> nodes;
    const ast::ListField *ast_params = list_field(expr, "params");
    if (ast_params == nullptr) {
      return nodes;
    }
    for (std::size_t index = 0; index < ast_params->values.size(); ++index) {
      const ast::Expr &param = *ast_params->values[index];
      std::unique_ptr<Node> pattern_node = pattern::parse_pattern_text(
          string_value(param, "pattern"), param.span);
      std::unique_ptr<Node> compiled_pattern =
          pattern::compile_pattern_ir(*pattern_node);
      attach_pattern_matcher_exprs(compiled_pattern.get());
      auto node = make_node("HParamPattern", param.span);
      std::string param_slot = "__arg" + std::to_string(index);
      if (pattern_node != nullptr && pattern_node->kind == "PatBind") {
        param_slot = string_value(*pattern_node, "name");
      }
      node->string_field("param_slot", param_slot);
      node->node_field("pattern", std::move(pattern_node));
      node->node_field("compiled_pattern", std::move(compiled_pattern));
      nodes.push_back(std::move(node));
    }
    return nodes;
  }

  std::unique_ptr<Node> build_block_signature(const ast::Expr &expr,
                                              int scope_index) {
    auto signature = make_node("HSignature", expr.span);
    std::vector<std::unique_ptr<Node>> params;
    const binder::Scope &scope = graph_.scopes[scope_index];
    const ast::ListField *ast_params = list_field(expr, "params");
    if (ast_params != nullptr && !ast_params->values.empty()) {
      for (std::size_t index = 0; index < ast_params->values.size(); ++index) {
        const ast::Expr &param_pattern = *ast_params->values[index];
        std::unique_ptr<Node> parsed_pattern = pattern::parse_pattern_text(
            string_value(param_pattern, "pattern"), param_pattern.span);
        std::string local_name = "__arg" + std::to_string(index);
        if (parsed_pattern != nullptr && parsed_pattern->kind == "PatBind") {
          local_name = string_value(*parsed_pattern, "name");
        }
        auto param = make_node("HParam", param_pattern.span);
        param->string_field("external_name", local_name);
        param->string_field("local_name", local_name);
        param->string_field("kind", "positional");
        param->string_field("auto_assign_kind", "none");
        param->string_field("auto_assign_target", "");
        param->string_field("type_expr", "");
        param->bool_field("has_default", false);
        param->string_field("default_kind", "");
        params.push_back(std::move(param));
      }
    } else {
      std::vector<const binder::Binding *> placeholders;
      for (const std::string &binding_id : scope.bindings) {
        const binder::Binding *binding = binding_for_id(binding_id);
        if (binding == nullptr || binding->kind != "placeholder") {
          continue;
        }
        placeholders.push_back(binding);
      }
      std::sort(placeholders.begin(), placeholders.end(),
                [](const binder::Binding *left, const binder::Binding *right) {
                  return left->name < right->name;
                });
      for (const binder::Binding *binding : placeholders) {
        auto param = make_node("HParam", binding->span);
        param->string_field("external_name", binding->name);
        param->string_field("local_name", binding->name);
        param->string_field("kind", "positional");
        param->string_field("auto_assign_kind", "none");
        param->string_field("auto_assign_target", "");
        param->string_field("type_expr", "");
        param->bool_field("has_default", false);
        param->string_field("default_kind", "");
        params.push_back(std::move(param));
      }
    }
    signature->list_field("params", std::move(params));
    return signature;
  }

  const binder::Binding *binding_for_id(const std::string &id) const {
    const auto found = bindings_by_id_.find(id);
    return found == bindings_by_id_.end() ? nullptr : found->second;
  }

  const binder::Reference *find_reference(const lexer::Span &span,
                                          const std::string &name,
                                          const std::string &ref_kind) const {
    const auto found = refs_by_key_.find(
        RefKey{span.start.offset, span.end.offset, name, ref_kind});
    return found == refs_by_key_.end() ? nullptr : found->second;
  }

  std::string slot_for_binding(const std::string &binding_id) const {
    if (current_proc_ == nullptr) {
      return "";
    }
    const auto found = current_proc_->slot_by_binding_id.find(binding_id);
    return found == current_proc_->slot_by_binding_id.end() ? ""
                                                            : found->second;
  }

  std::string capture_slot_for_binding(const std::string &binding_id) const {
    if (current_proc_ == nullptr) {
      return "";
    }
    const auto found =
        current_proc_->capture_slot_by_binding_id.find(binding_id);
    return found == current_proc_->capture_slot_by_binding_id.end()
               ? ""
               : found->second;
  }

  static std::string last_path_segment(const std::string &path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos) {
      return path;
    }
    return path.substr(dot + 1);
  }

  static std::string map_unary_selector(const std::string &op) {
    if (op == "+") {
      return "u+";
    }
    if (op == "-") {
      return "u-";
    }
    return op;
  }

  static std::string map_loop_kind(const std::string &kind) {
    if (kind == "AstWhile") {
      return "while";
    }
    if (kind == "AstUntil") {
      return "until";
    }
    if (kind == "AstDoWhile") {
      return "do_while";
    }
    return "loop";
  }

  std::vector<std::unique_ptr<ast::Expr>> synthetic_items_;
};

} // namespace

Program lower_module(const std::vector<std::unique_ptr<ast::Expr>> &items,
                     const std::string &module_name,
                     const binder::BindGraph &bind_graph) {
  Lowerer lowerer(items, module_name, bind_graph);
  return lowerer.lower();
}

std::string program_to_json(const Program &program,
                            const std::string &module_name,
                            const std::string &source_hash) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"format\": \"amber.hir.v1\",\n";
  if (module_name.empty()) {
    out << "  \"module\": null,\n";
  } else {
    out << "  \"module\": \"" << json_escape(module_name) << "\",\n";
  }
  out << "  \"root\":";
  if (program.root) {
    append_node_json(out, *program.root);
  } else {
    out << "null";
  }
  out << ",\n";
  out << "  \"procedures\": [\n";
  for (std::size_t i = 0; i < program.procedures.size(); ++i) {
    const Procedure &procedure = program.procedures[i];
    out << "    {\"id\":\"" << json_escape(procedure.id) << "\",\"name\":\""
        << json_escape(procedure.name) << "\",\"kind\":\""
        << json_escape(procedure.kind) << "\",\"owner\":\""
        << json_escape(procedure.owner) << "\",\"span\":";
    append_span_json(out, procedure.span);
    out << ",\"signature\":";
    append_node_json(out, *procedure.signature);
    out << ",\"locals\":[";
    for (std::size_t local_i = 0; local_i < procedure.locals.size();
         ++local_i) {
      if (local_i != 0) {
        out << ",";
      }
      const ProcedureLocal &local = procedure.locals[local_i];
      out << "{\"slot\":\"" << json_escape(local.slot) << "\",\"name\":\""
          << json_escape(local.name) << "\",\"role\":\""
          << json_escape(local.role) << "\",\"binding_kind\":\""
          << json_escape(local.binding_kind) << "\",\"span\":";
      append_span_json(out, local.span);
      out << "}";
    }
    out << "],\"captures\":[";
    for (std::size_t capture_i = 0; capture_i < procedure.captures.size();
         ++capture_i) {
      if (capture_i != 0) {
        out << ",";
      }
      const ProcedureCapture &capture = procedure.captures[capture_i];
      out << "{\"slot\":\"" << json_escape(capture.slot) << "\",\"name\":\""
          << json_escape(capture.name) << "\",\"source_kind\":\""
          << json_escape(capture.source_kind) << "\",\"source_slot\":\""
          << json_escape(capture.source_slot) << "\",\"source_name\":\""
          << json_escape(capture.source_name) << "\",\"span\":";
      append_span_json(out, capture.span);
      out << "}";
    }
    out << "],\"body\":";
    append_node_json(out, *procedure.body);
    out << "}";
    if (i + 1 < program.procedures.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"constants\": [],\n";
  out << "  \"source_hash\": \"sha256:" << source_hash << "\"\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::hir
