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

bool binding_is_property(const binder::Binding &binding) {
  return binding.role == "property" || binding.role == "class_property";
}

bool ast_is_property_decl(const ast::Expr &item) {
  return item.kind == "AstPropDef" || item.kind == "AstClassPropDef" ||
         item.kind == "AstAttrDef";
}

bool ast_is_class_property_decl(const ast::Expr &item) {
  return item.kind == "AstClassPropDef";
}

bool signature_param_has_auto_assign(const binder::ParamDescriptor &param) {
  return param.auto_assign_kind == "@" || param.auto_assign_kind == "@@";
}

std::string compound_assignment_binary_op(const std::string &op) {
  if (op == "+=") {
    return "+";
  }
  if (op == "-=") {
    return "-";
  }
  if (op == "*=") {
    return "*";
  }
  if (op == "/=") {
    return "/";
  }
  if (op == "//=") {
    return "//";
  }
  if (op == "%=") {
    return "%";
  }
  return "";
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

std::string unquote_string_literal(const std::string &value) {
  if (value.size() >= 2) {
    const char quote = value.front();
    if ((quote == '"' || quote == '\'') && value.back() == quote) {
      return value.substr(1, value.size() - 2);
    }
  }
  return value;
}

std::string static_string_literal_value(const ast::Expr &expr, bool *ok) {
  if (expr.kind == "AstLiteral" && string_value(expr, "token") == "STRING") {
    *ok = true;
    return unquote_string_literal(string_value(expr, "value"));
  }
  if (expr.kind != "AstStringLiteral" || bool_value(expr, "interpolation")) {
    *ok = false;
    return "";
  }
  std::string value;
  if (const ast::ListField *parts = list_field(expr, "parts")) {
    for (const std::unique_ptr<ast::Expr> &part : parts->values) {
      if (part == nullptr ||
          (part->kind != "AstStringText" && part->kind != "AstStringEscape")) {
        *ok = false;
        return "";
      }
      value += string_value(*part, "value");
    }
  }
  *ok = true;
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
    module_procedure_id_ = module_proc;
    std::vector<std::unique_ptr<Node>> module_items = lower_module_items();
    materialize_module_function_bindings(module_proc, module_items);

    auto root = make_node("HModule", module_span());
    root->string_field("module_name", module_name_);
    root->string_field("init", module_proc);
    root->list_field("imports", lower_imports());
    root->list_field("exports", lower_exports());
    root->list_field("items", std::move(module_items));
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
  std::string module_procedure_id_;

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
      if (!is_module_exec_item(*item)) {
        continue;
      }
      values.push_back(item.get());
    }
    return values;
  }

  bool is_module_exec_item(const ast::Expr &item) const {
    return item.kind != "AstPackageDecl" && item.kind != "AstImportStmt" &&
           item.kind != "AstExportStmt" && item.kind != "AstClassDef" &&
           item.kind != "AstMixinDef" && item.kind != "AstDefStmt" &&
           item.kind != "AstClassMethodDef" && item.kind != "AstClauseDef" &&
           !ast_is_property_decl(item);
  }

  bool is_module_callable_decl(const ast::Expr &item) const {
    return item.kind == "AstDefStmt" || item.kind == "AstClauseDef" ||
           item.kind == "AstPropDef";
  }

  Procedure *mutable_procedure_by_id(const std::string &id) {
    for (Procedure &procedure : procedures_) {
      if (procedure.id == id) {
        return &procedure;
      }
    }
    return nullptr;
  }

  std::string module_local_slot_for_decl(const Procedure &procedure,
                                         const ast::Expr &item) const {
    const std::string name = string_value(item, "name");
    for (const ProcedureLocal &local : procedure.locals) {
      if (local.name == name &&
          (local.role == "function" || local.role == "property") &&
          same_span(local.span, item.span)) {
        return local.slot;
      }
    }
    return "";
  }

  std::string
  module_local_slot_for_binding(const binder::Binding &binding) const {
    for (const Procedure &procedure : procedures_) {
      if (procedure.id != module_procedure_id_) {
        continue;
      }
      for (const ProcedureLocal &local : procedure.locals) {
        if (local.name == binding.name && same_span(local.span, binding.span)) {
          return local.slot;
        }
      }
    }
    return "";
  }

  std::string current_local_slot_for_decl(const ast::Expr &item) const {
    if (current_proc_ == nullptr) {
      return "";
    }
    const Procedure &procedure = procedures_[current_proc_->procedure_index];
    const std::string name = string_value(item, "name");
    for (const ProcedureLocal &local : procedure.locals) {
      if (local.name == name && same_span(local.span, item.span)) {
        return local.slot;
      }
    }
    return "";
  }

  std::vector<CapturePlan> build_module_method_capture_plans(int scope_index) {
    std::vector<CapturePlan> plans;
    if (scope_index < 0) {
      return plans;
    }
    const std::vector<const binder::Binding *> &captures =
        capture_bindings_for_scope(scope_index);
    for (const binder::Binding *binding : captures) {
      if (binding == nullptr) {
        continue;
      }
      const std::string source_slot = module_local_slot_for_binding(*binding);
      if (source_slot.empty()) {
        continue;
      }
      plans.push_back(CapturePlan{binding, "u" + std::to_string(plans.size()),
                                  "local", source_slot});
    }
    return plans;
  }

  std::string procedure_id_for_module_decl(
      const ast::Expr &item,
      const std::vector<std::unique_ptr<Node>> &module_items) const {
    const std::string name = string_value(item, "name");
    for (const std::unique_ptr<Node> &module_item : module_items) {
      if (module_item == nullptr || module_item->kind != "HMethod" ||
          string_value(*module_item, "name") != name ||
          !same_span(module_item->span, item.span)) {
        continue;
      }
      return string_value(*module_item, "procedure");
    }
    return "";
  }

  std::vector<std::unique_ptr<Node>>
  build_capture_nodes_for_procedure(const std::string &procedure_id) const {
    std::vector<std::unique_ptr<Node>> nodes;
    for (const Procedure &procedure : procedures_) {
      if (procedure.id != procedure_id) {
        continue;
      }
      for (const ProcedureCapture &capture : procedure.captures) {
        auto node = make_node("HCapture", capture.span);
        node->string_field("slot", capture.slot);
        node->string_field("name", capture.name);
        node->string_field("source_kind", capture.source_kind);
        node->string_field("source_slot", capture.source_slot);
        node->string_field("source_name", capture.source_name);
        nodes.push_back(std::move(node));
      }
      break;
    }
    return nodes;
  }

  std::unique_ptr<Node>
  make_function_binding_init(const ast::Expr &item, const std::string &slot,
                             const std::string &procedure_id) {
    auto closure = make_node("HClosure", item.span);
    closure->string_field("procedure", procedure_id);
    closure->list_field("captures",
                        build_capture_nodes_for_procedure(procedure_id));

    auto store = make_node("HStoreLocal", item.span);
    store->string_field("slot", slot);
    store->node_field("expr", std::move(closure));
    return store;
  }

  std::unique_ptr<Node> lower_local_property_decl(const ast::Expr &item) {
    const int scope_index =
        find_scope_index("property", item.span, string_value(item, "name"));
    const binder::Signature *signature = signature_for_scope(scope_index);
    std::vector<const ast::Expr *> body_items;
    if (const ast::ListField *body = list_field(item, "getter_body")) {
      for (const std::unique_ptr<ast::Expr> &stmt : body->values) {
        body_items.push_back(stmt.get());
      }
    }

    std::vector<CapturePlan> capture_plans;
    if (scope_index >= 0) {
      capture_bindings_for_scope(scope_index);
      capture_plans = build_capture_plans(scope_index);
    }
    const std::string procedure_id = lower_procedure(
        scope_index, string_value(item, "name"), "property",
        procedure_name_for_owner(string_value(item, "name")), signature,
        body_items, item.span, capture_plans);
    const std::string slot = current_local_slot_for_decl(item);
    auto seq = make_node("HSeq", item.span);
    std::vector<std::unique_ptr<Node>> items;
    if (!slot.empty()) {
      items.push_back(make_function_binding_init(item, slot, procedure_id));
    }
    items.push_back(make_null_last_set(item.span));
    seq->list_field("items", std::move(items));
    return seq;
  }

  std::unique_ptr<Node> make_null_last_set(const lexer::Span &span) {
    auto node = make_node("HLastSet", span);
    node->node_field("expr", make_null_const(span));
    return node;
  }

  void materialize_module_function_bindings(
      const std::string &module_proc,
      const std::vector<std::unique_ptr<Node>> &module_items) {
    Procedure *procedure = mutable_procedure_by_id(module_proc);
    if (procedure == nullptr || procedure->body == nullptr) {
      return;
    }
    ast::ListField *items = mutable_list_field(*procedure->body, "items");
    if (items == nullptr) {
      return;
    }

    std::vector<std::unique_ptr<Node>> lowered_exec_items =
        std::move(items->values);
    std::vector<std::unique_ptr<Node>> reordered;
    std::size_t exec_index = 0;
    for (const std::unique_ptr<ast::Expr> &item : items_) {
      if (item == nullptr) {
        continue;
      }
      if (is_module_callable_decl(*item)) {
        const std::string slot = module_local_slot_for_decl(*procedure, *item);
        const std::string procedure_id =
            procedure_id_for_module_decl(*item, module_items);
        if (!slot.empty() && !procedure_id.empty()) {
          reordered.push_back(
              make_function_binding_init(*item, slot, procedure_id));
          reordered.push_back(make_null_last_set(item->span));
        }
        continue;
      }
      if (!is_module_exec_item(*item)) {
        continue;
      }
      if (exec_index < lowered_exec_items.size()) {
        reordered.push_back(std::move(lowered_exec_items[exec_index++]));
      }
    }
    while (exec_index < lowered_exec_items.size()) {
      reordered.push_back(std::move(lowered_exec_items[exec_index++]));
    }
    items->values = std::move(reordered);
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
          item->kind == "AstClauseDef" || ast_is_property_decl(*item)) {
        append_lowered_item_decls(&nodes, *item, "module");
      }
    }
    return nodes;
  }

  void append_lowered_item_decls(std::vector<std::unique_ptr<Node>> *nodes,
                                 const ast::Expr &item,
                                 const std::string &dispatch_side) {
    if (nodes == nullptr) {
      return;
    }
    std::vector<std::unique_ptr<Node>> lowered =
        lower_item_decl_list(item, dispatch_side);
    for (std::unique_ptr<Node> &node : lowered) {
      nodes->push_back(std::move(node));
    }
  }

  std::vector<std::unique_ptr<Node>>
  lower_item_decl_list(const ast::Expr &item,
                       const std::string &dispatch_side) {
    std::vector<std::unique_ptr<Node>> nodes;
    if (ast_is_property_decl(item)) {
      const std::string method_dispatch_side =
          ast_is_class_property_decl(item) ? "class" : dispatch_side;
      if (bool_value(item, "has_getter")) {
        nodes.push_back(
            lower_property_accessor_decl(item, method_dispatch_side, false));
      }
      if (bool_value(item, "has_setter")) {
        nodes.push_back(
            lower_property_accessor_decl(item, method_dispatch_side, true));
      }
      if (nodes.empty()) {
        auto unsupported = make_node("HUnsupported", item.span);
        unsupported->string_field("source_kind", item.kind);
        nodes.push_back(std::move(unsupported));
      }
      return nodes;
    }
    nodes.push_back(lower_item_decl(item, dispatch_side));
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
        append_lowered_item_decls(&body, *child, dispatch_side);
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

  std::unique_ptr<Node>
  lower_property_accessor_decl(const ast::Expr &item,
                               const std::string &dispatch_side,
                               bool setter) {
    const bool class_property = ast_is_class_property_decl(item);
    const std::string base_name = string_value(item, "name");
    const std::string selector = setter ? base_name + "=" : base_name;
    const std::string scope_kind =
        setter ? (class_property ? "class_property_setter"
                                 : "property_setter")
               : (class_property ? "class_property" : "property");
    const int scope_index = find_scope_index(scope_kind, item.span, selector);
    const binder::Signature *signature = signature_for_scope(scope_index);

    auto node = make_node("HMethod", item.span);
    node->string_field("name", selector);
    node->string_field("dispatch_side", dispatch_side);
    if (setter) {
      node->bool_field("property_setter", true);
    } else {
      node->bool_field("property_getter", true);
    }
    node->list_field("auto_assign", build_auto_assign_nodes(signature));
    std::unique_ptr<Node> signature_node =
        build_signature_node(signature, item.span);
    if (scope_index >= 0) {
      const ast::ListField *body =
          list_field(item, setter ? "setter_body" : "getter_body");
      std::vector<const ast::Expr *> body_items;
      if (body != nullptr) {
        for (const std::unique_ptr<ast::Expr> &stmt : body->values) {
          body_items.push_back(stmt.get());
        }
      }
      std::vector<CapturePlan> capture_plans;
      if (dispatch_side == "module") {
        capture_plans = build_module_method_capture_plans(scope_index);
      }
      const std::string procedure_id = lower_procedure(
          scope_index, selector, setter ? "property_setter" : "property",
          procedure_name_for_owner(selector), signature, body_items, item.span,
          capture_plans, nullptr,
          setter ? node_field(item, "setter_signature") : nullptr);
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

  std::unique_ptr<Node> lower_method_decl(const ast::Expr &item,
                                          const std::string &dispatch_side,
                                          bool property_getter = false) {
    const std::string scope_kind =
        item.kind == "AstClassMethodDef"
            ? "class_method"
            : (property_getter ? (item.kind == "AstClassPropDef"
                                      ? "class_property"
                                      : "property")
                               : "function");
    const int scope_index =
        find_scope_index(scope_kind, item.span, string_value(item, "name"));
    const binder::Signature *signature = signature_for_scope(scope_index);
    auto node = make_node("HMethod", item.span);
    node->string_field("name", string_value(item, "name"));
    node->string_field("dispatch_side", dispatch_side);
    if (property_getter) {
      node->bool_field("property_getter", true);
    }
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
      std::vector<CapturePlan> capture_plans;
      if (dispatch_side == "module") {
        capture_plans = build_module_method_capture_plans(scope_index);
      }
      const std::string procedure_id =
          lower_procedure(scope_index, string_value(item, "name"), "method",
                          procedure_name_for_owner(string_value(item, "name")),
                          signature, body_items, item.span, capture_plans,
                          nullptr, node_field(item, "signature"));
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
      std::vector<CapturePlan> capture_plans;
      if (dispatch_side == "module") {
        capture_plans = build_module_method_capture_plans(scope_index);
      }

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
      initialize_captures(&context, capture_plans);
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
    std::unique_ptr<Node> else_body =
        lower_body(list_field(item, "else_body"), item.span);
    if (has_context) {
      auto dispatch = make_node("HClauseDispatch", item.span);
      dispatch->list_field("clauses", clone_node_list(clauses));
      dispatch->node_field("else_body", clone_node(*else_body));

      std::vector<std::unique_ptr<Node>> body_items;
      auto last_set = make_node("HLastSet", item.span);
      last_set->node_field("expr", std::move(dispatch));
      body_items.push_back(std::move(last_set));
      procedures_[context.procedure_index].body->list_field(
          "items", std::move(body_items));
    }
    node->node_field("signature", std::move(signature_node));
    node->list_field("clauses", std::move(clauses));
    node->node_field("else_body", std::move(else_body));

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
      if (signature->has_effect_row) {
        node->bool_field("has_effect_row", true);
        node->string_field("effect_row_expr", signature->effect_row_expr);
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
    auto add_binding_slot = [&](const binder::Binding *binding_ptr,
                                const std::string &role_override = "") {
      if (binding_ptr == nullptr || !binding_has_slot(*binding_ptr) ||
          context->slot_by_binding_id.count(binding_ptr->id) != 0U) {
        return false;
      }
      const std::string slot = "l" + std::to_string(context->next_slot++);
      context->slot_by_binding_id.emplace(binding_ptr->id, slot);
      procedures_[context->procedure_index].locals.push_back(ProcedureLocal{
          slot, binding_ptr->name,
          role_override.empty() ? binding_ptr->role : role_override,
          binding_ptr->kind, binding_ptr->span});
      return true;
    };

    const ast::ListField *signature_params =
        list_field(*procedures_[context->procedure_index].signature, "params");
    if (signature_params != nullptr) {
      for (const std::unique_ptr<ast::Expr> &param : signature_params->values) {
        const std::string local_name = string_value(*param, "local_name");
        if (local_name.empty()) {
          continue;
        }
        const binder::Binding *param_binding = nullptr;
        for (const binder::Binding *binding_ptr : bindings) {
          if (binding_ptr != nullptr && binding_ptr->name == local_name &&
              (binding_ptr->role == "param" ||
               binding_ptr->role == "block_param" ||
               binding_ptr->role == "implicit_block_param")) {
            param_binding = binding_ptr;
            break;
          }
        }
        const std::string role_override =
            param_binding != nullptr && param_binding->role == "block_param"
                ? "param"
                : "";
        if (add_binding_slot(param_binding, role_override)) {
          continue;
        }
        const std::string slot = "l" + std::to_string(context->next_slot++);
        procedures_[context->procedure_index].locals.push_back(ProcedureLocal{
            slot, local_name, "param", "synthetic", param->span});
      }
    }
    for (const binder::Binding *binding_ptr : bindings) {
      add_binding_slot(binding_ptr);
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

  std::unique_ptr<Node> make_bool_const(bool value, const lexer::Span &span) {
    auto node = make_node("HConst", span);
    node->string_field("token", value ? "KEYWORD_TRUE" : "KEYWORD_FALSE");
    node->string_field("value", value ? "true" : "false");
    return node;
  }

  std::unique_ptr<Node> make_const_str(const std::string &value,
                                       const lexer::Span &span) {
    auto node = make_node("HConstStr", span);
    node->string_field("value", value);
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
    if (ast_is_property_decl(item)) {
      auto node = make_node("HUnsupported", item.span);
      node->string_field("source_kind", item.kind);
      return node;
    }
    auto node = make_node("HLastSet", item.span);
    node->node_field("expr", lower_expr(item));
    return node;
  }

  std::unique_ptr<Node> lower_expr(const ast::Expr &expr) {
    if (expr.kind == "AstStringLiteral") {
      std::vector<std::unique_ptr<Node>> parts;
      std::string static_value;
      bool can_collapse_static = !bool_value(expr, "interpolation");
      if (const ast::ListField *list = list_field(expr, "parts")) {
        for (const std::unique_ptr<ast::Expr> &part : list->values) {
          if (part->kind == "AstStringText" ||
              part->kind == "AstStringEscape") {
            const std::string value = string_value(*part, "value");
            if (can_collapse_static) {
              static_value += value;
            } else {
              parts.push_back(make_const_str(value, part->span));
            }
          } else if (part->kind == "AstStringExpr") {
            can_collapse_static = false;
            if (const ast::Expr *inner = node_field(*part, "expr")) {
              auto stringify = make_node("HStringify", inner->span);
              stringify->string_field("mode", "display");
              stringify->node_field("expr", lower_expr(*inner));
              parts.push_back(std::move(stringify));
            }
          }
        }
      }
      if (can_collapse_static) {
        return make_const_str(static_value, expr.span);
      }
      auto node = make_node("HStringBuild", expr.span);
      node->list_field("parts", std::move(parts));
      return node;
    }
    if (expr.kind == "AstInterpolatedString") {
      auto node = make_node("HStringBuild", expr.span);
      std::vector<std::unique_ptr<Node>> parts;
      if (const ast::ListField *list = list_field(expr, "parts")) {
        for (const std::unique_ptr<ast::Expr> &part : list->values) {
          if (part->kind == "AstInterpolationLiteral") {
            parts.push_back(make_const_str(
                unquote_string_literal(string_value(*part, "value")),
                part->span));
          } else if (part->kind == "AstInterpolationExpr") {
            if (const ast::Expr *inner = node_field(*part, "expr")) {
              auto stringify = make_node("HStringify", inner->span);
              stringify->string_field("mode", "display");
              stringify->node_field("expr", lower_expr(*inner));
              parts.push_back(std::move(stringify));
            }
          }
        }
      }
      node->list_field("parts", std::move(parts));
      return node;
    }
    if (expr.kind == "AstLiteral") {
      auto node = make_node("HConst", expr.span);
      node->string_field("token", string_value(expr, "token"));
      node->string_field("value", string_value(expr, "value"));
      return node;
    }
    if (expr.kind == "AstGroup") {
      return lower_expr(*node_field_required(expr, "expr"));
    }
    if (expr.kind == "AstListLiteral" || expr.kind == "AstTupleLiteral" ||
        expr.kind == "AstSetLiteral") {
      const char *kind = expr.kind == "AstListLiteral"
                             ? "HListLiteral"
                             : (expr.kind == "AstTupleLiteral" ? "HTupleLiteral"
                                                               : "HSetLiteral");
      auto node = make_node(kind, expr.span);
      std::vector<std::unique_ptr<Node>> elements;
      if (const ast::ListField *list = list_field(expr, "elements")) {
        for (const std::unique_ptr<ast::Expr> &element : list->values) {
          if (element->kind == "AstArrayElement" ||
              element->kind == "AstSetElement") {
            auto lowered = make_node("HConditionalElement", element->span);
            if (const ast::Expr *condition =
                    node_field(*element, "condition")) {
              lowered->string_field("condition_kind",
                                    string_value(*condition, "kind"));
              if (const ast::Expr *condition_expr =
                      node_field(*condition, "expr")) {
                lowered->node_field("condition", lower_expr(*condition_expr));
              }
            }
            if (const ast::Expr *value = node_field(*element, "expr")) {
              lowered->node_field("value", lower_expr(*value));
            }
            elements.push_back(std::move(lowered));
          } else {
            elements.push_back(lower_expr(*element));
          }
        }
      }
      node->list_field("elements", std::move(elements));
      return node;
    }
    if (expr.kind == "AstMapLiteral") {
      auto node = make_node("HMapLiteral", expr.span);
      std::vector<std::unique_ptr<Node>> entries;
      if (const ast::ListField *list = list_field(expr, "entries")) {
        for (const std::unique_ptr<ast::Expr> &entry : list->values) {
          auto lowered = make_node("HMapEntry", entry->span);
          lowered->string_field("key_kind", string_value(*entry, "key_kind"));
          lowered->string_field("key", string_value(*entry, "key"));
          if (const ast::Expr *value = node_field(*entry, "value")) {
            lowered->node_field("value", lower_expr(*value));
          }
          if (const ast::Expr *condition = node_field(*entry, "condition")) {
            lowered->string_field("condition_kind",
                                  string_value(*condition, "kind"));
            if (const ast::Expr *condition_expr =
                    node_field(*condition, "expr")) {
              lowered->node_field("condition", lower_expr(*condition_expr));
            }
          }
          entries.push_back(std::move(lowered));
        }
      }
      node->list_field("entries", std::move(entries));
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
    if (expr.kind == "AstCompareChain") {
      auto node = make_node("HCompareChain", expr.span);
      node->node_field("first", lower_expr(*node_field_required(expr, "first")));
      std::vector<std::unique_ptr<Node>> links;
      if (const ast::ListField *list = list_field(expr, "links")) {
        for (const std::unique_ptr<ast::Expr> &link : list->values) {
          auto lowered = make_node("HCompareLink", link->span);
          lowered->string_field("op", string_value(*link, "op"));
          lowered->node_field("right",
                              lower_expr(*node_field_required(*link, "right")));
          links.push_back(std::move(lowered));
        }
      }
      node->list_field("links", std::move(links));
      return node;
    }
    if (expr.kind == "AstBinary") {
      const std::string op = string_value(expr, "op");
      if (op == "and" || op == "or") {
        auto node = make_node("HLogical", expr.span);
        node->string_field("op", op);
        node->node_field("left",
                         lower_expr(*node_field_required(expr, "left")));
        node->node_field("right",
                         lower_expr(*node_field_required(expr, "right")));
        return node;
      }
      if (op == "in") {
        auto node = make_node("HSend", expr.span);
        node->node_field("receiver",
                         lower_expr(*node_field_required(expr, "right")));
        node->string_field("selector", "contains?");
        std::vector<std::unique_ptr<Node>> pos_args;
        pos_args.push_back(lower_expr(*node_field_required(expr, "left")));
        node->list_field("pos_args", std::move(pos_args));
        node->list_field("kw_args", {});
        return node;
      }
      if (op == "..") {
        auto node = make_node("HSend", expr.span);
        auto receiver = make_node("HLoadConst", expr.span);
        receiver->string_field("path", "Range");
        node->node_field("receiver", std::move(receiver));
        node->string_field("selector", "new");
        std::vector<std::unique_ptr<Node>> pos_args;
        pos_args.push_back(lower_expr(*node_field_required(expr, "left")));
        pos_args.push_back(lower_expr(*node_field_required(expr, "right")));
        node->list_field("pos_args", std::move(pos_args));
        std::vector<std::unique_ptr<Node>> kw_args;
        auto inclusive = make_node("HKeywordArg", expr.span);
        inclusive->string_field("name", "inclusive_end");
        inclusive->node_field("value", make_bool_const(true, expr.span));
        kw_args.push_back(std::move(inclusive));
        node->list_field("kw_args", std::move(kw_args));
        return node;
      }
      auto node = make_node("HSend", expr.span);
      node->node_field("receiver",
                       lower_expr(*node_field_required(expr, "left")));
      node->string_field("selector", op);
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
    if (expr.kind == "AstInlineIfExpr") {
      auto node = make_node("HIf", expr.span);
      node->node_field("cond",
                       lower_expr(*node_field_required(expr, "condition")));

      std::vector<std::unique_ptr<Node>> then_items;
      auto then_last = make_node("HLastSet", expr.span);
      then_last->node_field(
          "expr", lower_expr(*node_field_required(expr, "consequent")));
      then_items.push_back(std::move(then_last));
      auto then_body = make_node("HSeq", expr.span);
      then_body->list_field("items", std::move(then_items));

      std::vector<std::unique_ptr<Node>> else_items;
      auto else_last = make_node("HLastSet", expr.span);
      else_last->node_field(
          "expr", lower_expr(*node_field_required(expr, "alternative")));
      else_items.push_back(std::move(else_last));
      auto else_body = make_node("HSeq", expr.span);
      else_body->list_field("items", std::move(else_items));

      node->node_field("then_body", std::move(then_body));
      node->node_field("else_body", std::move(else_body));
      return node;
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
    if (binding.kind == "import_alias") {
      const std::string stdlib_path =
          stdlib_import_alias_constant_path(binding);
      if (!stdlib_path.empty()) {
        auto node = make_node("HLoadConst", expr.span);
        node->string_field("path", stdlib_path);
        return node;
      }
    }
    const std::string slot = slot_for_binding(binding.id);
    if (!slot.empty()) {
      auto node = make_node("HLoadLocal", expr.span);
      node->string_field("slot", slot);
      if (binding_is_property(binding)) {
        auto call = make_node("HCall", expr.span);
        call->node_field("callable", std::move(node));
        call->list_field("pos_args", {});
        call->list_field("kw_args", {});
        return call;
      }
      return node;
    }
    const std::string capture_slot = capture_slot_for_binding(binding.id);
    if (!capture_slot.empty()) {
      auto node = make_node("HLoadCapture", expr.span);
      node->string_field("slot", capture_slot);
      if (binding_is_property(binding)) {
        auto call = make_node("HCall", expr.span);
        call->node_field("callable", std::move(node));
        call->list_field("pos_args", {});
        call->list_field("kw_args", {});
        return call;
      }
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

    const std::string assign_op = string_value(expr, "op");
    const bool compound = assign_op != "=";
    const std::string binary_op =
        compound ? compound_assignment_binary_op(assign_op) : "";
    if (compound && binary_op.empty()) {
      auto node = make_node("HUnsupported", expr.span);
      node->string_field("source_kind", "assign-op:" + assign_op);
      return node;
    }
    auto lower_assigned_value = [&]() {
      if (!compound) {
        return lower_expr(*right);
      }
      auto send = make_node("HSend", expr.span);
      send->node_field("receiver", lower_expr(*left));
      send->string_field("selector", binary_op);
      std::vector<std::unique_ptr<Node>> pos_args;
      pos_args.push_back(lower_expr(*right));
      send->list_field("pos_args", std::move(pos_args));
      send->list_field("kw_args", {});
      return send;
    };

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
        node->node_field("expr", lower_assigned_value());
        return node;
      }
      auto node = make_node("HStoreLocal", expr.span);
      node->string_field("slot",
                         slot.empty() ? string_value(*left, "name") : slot);
      node->node_field("expr", lower_assigned_value());
      return node;
    }
    if (left->kind == "AstIvar") {
      auto node = make_node("HStoreIvar", expr.span);
      node->string_field("name", string_value(*left, "name"));
      node->node_field("expr", lower_assigned_value());
      return node;
    }
    if (left->kind == "AstCvar") {
      auto node = make_node("HStoreCvar", expr.span);
      node->string_field("name", string_value(*left, "name"));
      node->node_field("expr", lower_assigned_value());
      return node;
    }
    if (compound) {
      auto node = make_node("HUnsupported", expr.span);
      node->string_field("source_kind", "compound_assign:" + left->kind);
      return node;
    }
    if (left->kind == "AstPostfixChain") {
      const ast::ListField *tails = list_field(*left, "tails");
      if (tails != nullptr && !tails->values.empty()) {
        const ast::Expr &last_tail = *tails->values.back();
        if (last_tail.kind == "AstTailDotMember") {
          auto node = make_node("HSend", expr.span);
          node->node_field(
              "receiver",
              lower_postfix_chain_with_limit(*left, tails->values.size() - 1U));
          node->string_field("selector",
                             string_value(last_tail, "name") + "=");
          node->bool_field("property_assignment", true);
          std::vector<std::unique_ptr<Node>> pos_args;
          pos_args.push_back(lower_expr(*right));
          node->list_field("pos_args", std::move(pos_args));
          node->list_field("kw_args", {});
          return node;
        }
      }
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
    const ast::ListField *tails = list_field(expr, "tails");
    return lower_postfix_chain_with_limit(
        expr, tails == nullptr ? 0U : tails->values.size());
  }

  std::unique_ptr<Node>
  lower_postfix_chain_with_limit(const ast::Expr &expr,
                                 std::size_t tail_limit) {
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
    const std::size_t tail_count = std::min(tail_limit, tails->values.size());

    std::size_t i = 0;
    while (i < tail_count) {
      const ast::Expr &tail = *tails->values[i];
      if (i == 0 && tail.kind == "AstTailCall" && is_builtin_send_base(*base)) {
        std::unique_ptr<Node> block;
        if (i + 1 < tail_count &&
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
        bool has_explicit_call = false;
        bool has_block_suffix = false;
        if (i + 1 < tail_count &&
            tails->values[i + 1]->kind == "AstTailCall") {
          collect_call_args(*tails->values[i + 1], &pos_args, &kw_args);
          has_explicit_call = true;
          ++i;
        }
        if (i + 1 < tail_count &&
            tails->values[i + 1]->kind == "AstTailBlockSuffix") {
          block = lower_block_suffix(*tails->values[i + 1]);
          has_block_suffix = true;
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
        if (!has_explicit_call && !has_block_suffix) {
          node->bool_field("property_access", true);
        }
        node->list_field("pos_args", std::move(pos_args));
        node->list_field("kw_args", std::move(kw_args));
        if (block) {
          node->node_field("block", std::move(block));
        }
        std::unique_ptr<Node> lowered =
            lower_kernel_watch_send(*base, std::move(node));
        current = safe ? wrap_safe_guard(std::move(guard_base), temp_slot,
                                         std::move(lowered), node_span)
                       : std::move(lowered);
        ++i;
        continue;
      }
      if (tail.kind == "AstTailCall" || tail.kind == "AstTailSafeCall") {
        const bool safe = tail.kind == "AstTailSafeCall";
        std::vector<std::unique_ptr<Node>> pos_args;
        std::vector<std::unique_ptr<Node>> kw_args;
        std::unique_ptr<Node> block;
        collect_call_args(tail, &pos_args, &kw_args);
        if (i + 1 < tail_count &&
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

    bool selector_is_static = false;
    const std::string static_selector =
        static_string_literal_value(selector_arg, &selector_is_static);
    if (selector_is_static) {
      auto node = make_node("HSend", span);
      node->node_field("receiver", lower_expr(receiver_arg));
      node->string_field("selector", static_selector);
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

  std::unique_ptr<Node> lower_kernel_watch_send(const ast::Expr &base,
                                                std::unique_ptr<Node> send) {
    if (send == nullptr || send->kind != "HSend" ||
        string_value(*send, "selector") != "watch" || base.kind != "AstName" ||
        string_value(base, "name") != "Kernel") {
      return send;
    }
    const binder::Reference *kernel_ref =
        find_reference(base.span, "Kernel", "name");
    if (kernel_ref != nullptr && kernel_ref->resolved) {
      return send;
    }

    const ast::ListField *kw_args = list_field(*send, "kw_args");
    const ast::ListField *pos_args = list_field(*send, "pos_args");
    if (node_field(*send, "block") != nullptr ||
        (kw_args != nullptr && !kw_args->values.empty()) ||
        pos_args == nullptr || pos_args->values.size() != 1U) {
      auto node = make_node("HWatchUnsupported", send->span);
      node->string_field("target_kind", "arity");
      return node;
    }

    const ast::Expr &target = *pos_args->values.front();
    if (target.kind == "HLoadLocal") {
      auto node = make_node("HWatchLocal", send->span);
      node->string_field("slot", string_value(target, "slot"));
      return node;
    }
    if (target.kind == "HLoadCapture") {
      auto node = make_node("HWatchCapture", send->span);
      node->string_field("slot", string_value(target, "slot"));
      return node;
    }
    if (target.kind == "HLoadIvar") {
      auto node = make_node("HWatchIvar", send->span);
      node->string_field("name", string_value(target, "name"));
      return node;
    }

    auto node = make_node("HWatchUnsupported", send->span);
    node->string_field("target_kind", target.kind);
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
      } else if (const ast::ListField *body_list = list_field(expr, "body")) {
        for (const std::unique_ptr<ast::Expr> &item : body_list->values) {
          body_items.push_back(item.get());
        }
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

  static std::string
  stdlib_import_alias_constant_path(const binder::Binding &binding) {
    if (binding.role == "module_import" && binding.source == "task") {
      return "task";
    }
    if (binding.role == "module_import" && binding.source == "task.flow") {
      return "task.flow";
    }
    if (binding.role != "from_import") {
      return "";
    }
    if (binding.source == "sync:Channel") {
      return "sync.Channel";
    }
    if (binding.source == "sync:Mutex") {
      return "sync.Mutex";
    }
    if (binding.source == "sync:Atomic") {
      return "sync.Atomic";
    }
    if (binding.source == "sync:Barrier") {
      return "sync.Barrier";
    }
    if (binding.source == "task.flow:Flow") {
      return "Flow";
    }
    if (binding.source == "task.flow:ThreadedCollection") {
      return "ThreadedCollection";
    }
    return "";
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
