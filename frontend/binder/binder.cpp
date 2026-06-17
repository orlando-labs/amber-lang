#include "frontend/binder/binder.h"
#include "frontend/lexer/lexer.h"
#include "frontend/pattern/pattern.h"

#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace amber::binder {
namespace {

const std::string kLastValueName = "$_";

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

lexer::Span list_span_or_parent(const ast::ListField *items,
                                const lexer::Span &parent_span) {
  if (items == nullptr || items->values.empty()) {
    return parent_span;
  }
  return ast::join_spans(items->values.front()->span,
                         items->values.back()->span);
}

std::string last_path_segment(const std::string &path) {
  const std::size_t dot = path.rfind('.');
  if (dot == std::string::npos) {
    return path;
  }
  return path.substr(dot + 1);
}

int placeholder_number(const std::string &name) {
  if (name.size() < 2 || name[0] != '_') {
    return 0;
  }
  int value = 0;
  for (std::size_t i = 1; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') {
      return 0;
    }
    value = value * 10 + (name[i] - '0');
  }
  return value;
}

void collect_default_refs(const ast::Expr &expr,
                          std::vector<const ast::Expr *> *names,
                          std::vector<const ast::Expr *> *fields) {
  if (expr.kind == "AstName") {
    names->push_back(&expr);
    return;
  }
  if (expr.kind == "AstIvar" || expr.kind == "AstCvar") {
    fields->push_back(&expr);
    return;
  }
  if (expr.kind == "AstTailDotMember" || expr.kind == "AstTailSafeMember") {
    return;
  }
  for (const ast::NodeField &field : expr.node_fields) {
    if (field.value) {
      collect_default_refs(*field.value, names, fields);
    }
  }
  for (const ast::ListField &field : expr.list_fields) {
    for (const std::unique_ptr<ast::Expr> &value : field.values) {
      collect_default_refs(*value, names, fields);
    }
  }
}

class Binder {
public:
  BindResult bind(const std::vector<std::unique_ptr<ast::Expr>> &items) {
    const lexer::Span span = module_span(items);
    const int module_scope = add_scope("module", "", -1, span);
    declare_last_value(module_scope, span);
    predeclare_items(module_scope, items);
    visit_items(module_scope, items);
    return BindResult{std::move(graph_), std::move(diagnostics_)};
  }

private:
  enum class DuplicatePolicy { AllowExisting, Error, ReopenSameRole };

  struct BlockContext {
    int scope_index = -1;
    bool has_explicit_params = false;
    std::set<int> placeholders;
    lexer::Span span;
  };

  lexer::Span
  module_span(const std::vector<std::unique_ptr<ast::Expr>> &items) const {
    if (items.empty()) {
      return lexer::Span{};
    }
    return ast::join_spans(items.front()->span, items.back()->span);
  }

  int add_scope(const std::string &kind, const std::string &owner,
                int parent_index, const lexer::Span &span) {
    const int index = static_cast<int>(graph_.scopes.size());
    Scope scope;
    scope.id = "s" + std::to_string(index);
    scope.parent_index = parent_index;
    scope.kind = kind;
    scope.owner = owner;
    scope.span = span;
    graph_.scopes.push_back(std::move(scope));
    return index;
  }

  Binding *find_local_binding(int scope_index, const std::string &name) {
    if (scope_index < 0) {
      return nullptr;
    }
    for (const std::string &id : graph_.scopes[scope_index].bindings) {
      Binding *binding = binding_by_id(id);
      if (binding != nullptr && binding->name == name) {
        return binding;
      }
    }
    return nullptr;
  }

  Binding *binding_by_id(const std::string &id) {
    for (Binding &binding : graph_.bindings) {
      if (binding.id == id) {
        return &binding;
      }
    }
    return nullptr;
  }

  Binding *resolve(int scope_index, const std::string &name) {
    for (int current = scope_index; current >= 0;
         current = graph_.scopes[current].parent_index) {
      Binding *binding = find_local_binding(current, name);
      if (binding != nullptr) {
        return binding;
      }
    }
    return nullptr;
  }

  Binding *declare_binding(
      int scope_index, const std::string &name, const std::string &kind,
      const std::string &role, const lexer::Span &span, bool read_only,
      const std::string &source = "",
      DuplicatePolicy duplicate_policy = DuplicatePolicy::AllowExisting) {
    if (Binding *existing = find_local_binding(scope_index, name)) {
      if (duplicate_policy == DuplicatePolicy::Error) {
        diagnostic("B0001", "error", "binder",
                   "duplicate lexical binding in one scope", span);
      } else if (duplicate_policy == DuplicatePolicy::ReopenSameRole &&
                 (existing->kind != kind || existing->role != role)) {
        diagnostic(
            "E3006", "error", "binder",
            "reopen mixin/class name collides with different binding kind",
            span);
      }
      return existing;
    }
    const int index = static_cast<int>(graph_.bindings.size());
    Binding binding;
    binding.id = "b" + std::to_string(index);
    binding.scope_index = scope_index;
    binding.name = name;
    binding.kind = kind;
    binding.role = role;
    binding.read_only = read_only;
    binding.span = span;
    binding.source = source;
    graph_.bindings.push_back(std::move(binding));
    graph_.scopes[scope_index].bindings.push_back("b" + std::to_string(index));
    return &graph_.bindings.back();
  }

  bool binding_is_property(const Binding &binding) const {
    return binding.role == "property" || binding.role == "class_property";
  }

  bool ast_is_property_decl(const ast::Expr &item) const {
    return item.kind == "AstPropDef" || item.kind == "AstClassPropDef" ||
           item.kind == "AstAttrDef";
  }

  bool ast_is_class_property_decl(const ast::Expr &item) const {
    return item.kind == "AstClassPropDef";
  }

  bool same_decl_span(const Binding &binding, const lexer::Span &span) const {
    return binding.span.file == span.file &&
           binding.span.start.offset == span.start.offset &&
           binding.span.end.offset == span.end.offset;
  }

  Binding *declare_property_binding(int scope_index, const std::string &name,
                                    const std::string &role,
                                    const lexer::Span &span, bool has_getter,
                                    bool has_setter) {
    if (Binding *existing = find_local_binding(scope_index, name)) {
      if (!same_decl_span(*existing, span) ||
          !binding_is_property(*existing) || existing->role != role) {
        diagnostic("E_MEMBER_NAME_CONFLICT", "error", "binder",
                   "external member '" + name + "' declared multiple times",
                   span);
      }
      existing->property_has_getter = has_getter;
      existing->property_has_setter = has_setter;
      existing->read_only = !has_setter;
      return existing;
    }
    Binding *binding =
        declare_binding(scope_index, name, "local", role, span, !has_setter);
    binding->property_has_getter = has_getter;
    binding->property_has_setter = has_setter;
    return binding;
  }

  Binding *declare_callable_binding(int scope_index, const std::string &name,
                                    const std::string &role,
                                    const lexer::Span &span) {
    if (Binding *existing = find_local_binding(scope_index, name)) {
      if (binding_is_property(*existing)) {
        diagnostic("E_MEMBER_NAME_CONFLICT", "error", "binder",
                   "external member '" + name + "' declared multiple times",
                   span);
        return existing;
      }
    }
    return declare_binding(scope_index, name, "local", role, span, false, "",
                           DuplicatePolicy::Error);
  }

  void declare_last_value(int scope_index, const lexer::Span &span) {
    declare_binding(scope_index, kLastValueName, "last_value", "last_value",
                    span, true);
  }

  void add_reference(int scope_index, const std::string &name,
                     const std::string &ref_kind, const lexer::Span &span,
                     Binding *binding) {
    const int index = static_cast<int>(graph_.references.size());
    Reference ref;
    ref.id = "r" + std::to_string(index);
    ref.scope_index = scope_index;
    ref.name = name;
    ref.ref_kind = ref_kind;
    ref.span = span;
    if (binding != nullptr) {
      ref.resolved = true;
      ref.binding_id = binding->id;
    }
    graph_.references.push_back(std::move(ref));
  }

  void diagnostic(const std::string &code, const std::string &severity,
                  const std::string &phase, const std::string &message,
                  const lexer::Span &span) {
    diagnostics_.push_back(
        lexer::Diagnostic{code, severity, phase, message, span});
  }

  void predeclare_items(int scope_index,
                        const std::vector<std::unique_ptr<ast::Expr>> &items) {
    for (const std::unique_ptr<ast::Expr> &item : items) {
      predeclare_item(scope_index, *item);
    }
  }

  void predeclare_item(int scope_index, const ast::Expr &item) {
    if (item.kind == "AstImportStmt" &&
        graph_.scopes[scope_index].kind == "module") {
      declare_imports(scope_index, item);
      return;
    }
    if (item.kind == "AstDefStmt" || item.kind == "AstClassMethodDef" ||
        item.kind == "AstClauseDef") {
      const std::string name = string_value(item, "name");
      const std::string scope_kind = graph_.scopes[scope_index].kind;
      std::string role = "function";
      if (scope_kind == "class" || scope_kind == "mixin") {
        role = item.kind == "AstClassMethodDef" ? "class_method" : "method";
      }
      declare_callable_binding(scope_index, name, role, item.span);
      return;
    }
    if (ast_is_property_decl(item)) {
      if (!property_allowed_in_scope(scope_index, item)) {
        return;
      }
      const std::string role =
          ast_is_class_property_decl(item) ? "class_property" : "property";
      declare_property_binding(scope_index, string_value(item, "name"), role,
                               item.span, bool_value(item, "has_getter"),
                               bool_value(item, "has_setter"));
      return;
    }
    if (item.kind == "AstClassDef") {
      declare_binding(scope_index, string_value(item, "name"), "constant",
                      "class", item.span, false, "",
                      DuplicatePolicy::ReopenSameRole);
      return;
    }
    if (item.kind == "AstMixinDef") {
      declare_binding(scope_index, string_value(item, "name"), "constant",
                      "mixin", item.span, false, "",
                      DuplicatePolicy::ReopenSameRole);
      return;
    }
    if (item.kind == "AstExprStmt" &&
        graph_.scopes[scope_index].kind == "module") {
      const ast::Expr *expr = node_field(item, "expr");
      const ast::Expr *left = expr != nullptr && expr->kind == "AstAssign"
                                  ? node_field(*expr, "left")
                                  : nullptr;
      if (left != nullptr && left->kind == "AstName") {
        const std::string name = string_value(*left, "name");
        if (name != "_") {
          declare_binding(scope_index, name, "local", "module_cell", left->span,
                          false);
        }
      }
    }
  }

  void declare_imports(int scope_index, const ast::Expr &item) {
    const std::string import_kind = string_value(item, "import_kind");
    const std::string module_path = string_value(item, "module_path");
    if (import_kind == "module") {
      std::string local_name = string_value(item, "alias");
      if (local_name.empty()) {
        local_name = last_path_segment(module_path);
      }
      declare_binding(scope_index, local_name, "import_alias", "module_import",
                      item.span, true, module_path, DuplicatePolicy::Error);
      return;
    }

    const ast::ListField *names = list_field(item, "names");
    if (names == nullptr) {
      return;
    }
    for (const std::unique_ptr<ast::Expr> &name : names->values) {
      const std::string local_name = string_value(*name, "local_name");
      const std::string source_name = string_value(*name, "source_name");
      declare_binding(scope_index, local_name, "import_alias", "from_import",
                      name->span, true, module_path + ":" + source_name,
                      DuplicatePolicy::Error);
    }
  }

  void visit_items(int scope_index,
                   const std::vector<std::unique_ptr<ast::Expr>> &items) {
    for (const std::unique_ptr<ast::Expr> &item : items) {
      visit_item(scope_index, *item);
    }
  }

  void visit_item(int scope_index, const ast::Expr &item) {
    if (item.kind == "AstPackageDecl" || item.kind == "AstImportStmt" ||
        item.kind == "AstPassStmt" || item.kind == "AstNoopStmt") {
      return;
    }
    if (item.kind == "AstExportStmt") {
      visit_export_stmt(scope_index, item);
      return;
    }
    if (item.kind == "AstDefStmt" || item.kind == "AstClassMethodDef") {
      visit_def(scope_index, item);
      return;
    }
    if (ast_is_property_decl(item)) {
      visit_property_def(scope_index, item);
      return;
    }
    if (item.kind == "AstClauseDef") {
      visit_clause_def(scope_index, item);
      return;
    }
    if (item.kind == "AstClassDef") {
      visit_class_like(scope_index, item, "class");
      return;
    }
    if (item.kind == "AstMixinDef") {
      visit_class_like(scope_index, item, "mixin");
      return;
    }
    if (item.kind == "AstIncludeStmt" || item.kind == "AstExtendStmt") {
      visit_include_like(scope_index, item);
      return;
    }
    if (item.kind == "AstExprStmt") {
      if (const ast::Expr *expr = node_field(item, "expr")) {
        visit_expr(scope_index, *expr);
      }
      return;
    }
    visit_expr(scope_index, item);
  }

  void visit_export_stmt(int scope_index, const ast::Expr &item) {
    const ast::ListField *items = list_field(item, "items");
    if (items == nullptr) {
      return;
    }
    std::set<std::string> public_names;
    for (const std::unique_ptr<ast::Expr> &export_item : items->values) {
      const std::string local_name = string_value(*export_item, "local_name");
      const std::string public_name = string_value(*export_item, "public_name");
      Binding *binding = resolve(scope_index, local_name);
      Export export_record;
      export_record.local_name = local_name;
      export_record.public_name = public_name;
      export_record.span = export_item->span;
      if (binding != nullptr) {
        export_record.resolved = true;
        export_record.binding_id = binding->id;
      } else {
        diagnostic("E2005", "error", "binder", "export of unknown local name",
                   export_item->span);
      }
      if (!public_names.insert(public_name).second) {
        diagnostic("E2006", "error", "binder", "duplicate public export",
                   export_item->span);
      }
      graph_.exports.push_back(std::move(export_record));
    }
  }

  void visit_def(int parent_scope, const ast::Expr &item) {
    const std::string name = string_value(item, "name");
    const std::string scope_kind =
        item.kind == "AstClassMethodDef" ? "class_method" : "function";
    const int scope = add_scope(scope_kind, name, parent_scope, item.span);
    declare_last_value(scope, item.span);

    if (const ast::Expr *signature = node_field(item, "signature")) {
      bind_signature(parent_scope, scope, *signature);
    }
    if (const ast::ListField *body = list_field(item, "body")) {
      visit_items(scope, body->values);
    }
    if (const ast::ListField *rescues = list_field(item, "rescues")) {
      for (std::size_t index = 0; index < rescues->values.size(); ++index) {
        visit_rescue_clause(scope, "rescue." + std::to_string(index),
                            *rescues->values[index]);
      }
    }
    if (bool_value(item, "has_ensure")) {
      visit_branch_scope(scope, "ensure", list_field(item, "ensure_body"),
                         item.span);
    }
  }

  void bind_empty_signature(int function_scope, const lexer::Span &span) {
    Signature descriptor;
    descriptor.id = "sig" + std::to_string(graph_.signatures.size());
    descriptor.scope_index = function_scope;
    descriptor.owner = graph_.scopes[function_scope].owner;
    descriptor.span = span;
    graph_.signatures.push_back(std::move(descriptor));
  }

  bool property_allowed_in_scope(int parent_scope, const ast::Expr &item) const {
    const std::string scope_kind = graph_.scopes[parent_scope].kind;
    if (item.kind == "AstClassPropDef") {
      return scope_kind == "class";
    }
    if (item.kind == "AstAttrDef") {
      return scope_kind == "class" || scope_kind == "mixin";
    }
    return scope_kind == "module" || scope_kind == "class" ||
           scope_kind == "mixin";
  }

  void diagnose_property_context(int parent_scope, const ast::Expr &item) {
    const std::string scope_kind = graph_.scopes[parent_scope].kind;
    if (item.kind == "AstClassPropDef" && scope_kind != "class") {
      diagnostic("AMB_PROP_CLASS_PROP_OUTSIDE_CLASS", "error", "binder",
                 "class_prop is only allowed in class body", item.span);
      return;
    }
    if (item.kind == "AstAttrDef" && scope_kind != "class" &&
        scope_kind != "mixin") {
      diagnostic("E_ATTR_INVALID_CONTEXT", "error", "binder",
                 "attr declarations are only allowed in class or mixin bodies",
                 item.span);
      return;
    }
    if (item.kind == "AstPropDef" && scope_kind != "module" &&
        scope_kind != "class" && scope_kind != "mixin") {
      diagnostic("AMB_PROP_INVALID_CONTEXT", "error", "binder",
                 "property declarations are not allowed in function or block "
                 "bodies",
                 item.span);
    }
  }

  void visit_property_def(int parent_scope, const ast::Expr &item) {
    const std::string name = string_value(item, "name");
    const bool class_property = ast_is_class_property_decl(item);
    if (!property_allowed_in_scope(parent_scope, item)) {
      diagnose_property_context(parent_scope, item);
      return;
    }
    const bool has_getter = bool_value(item, "has_getter");
    const bool has_setter = bool_value(item, "has_setter");
    if (graph_.scopes[parent_scope].kind == "module" && has_setter) {
      diagnostic("AMB_PROP_TOP_LEVEL_SETTER", "error", "binder",
                 "top-level writable properties are not part of Amber v20.3",
                 item.span);
    }
    declare_property_binding(parent_scope, name,
                             class_property ? "class_property" : "property",
                             item.span, has_getter, has_setter);
    if (has_getter) {
      const int getter_scope =
          add_scope(class_property ? "class_property" : "property", name,
                    parent_scope, item.span);
      declare_last_value(getter_scope, item.span);
      bind_empty_signature(getter_scope, item.span);
      if (const ast::ListField *body = list_field(item, "getter_body")) {
        visit_items(getter_scope, body->values);
      }
    }
    if (has_setter) {
      const int setter_scope = add_scope(
          class_property ? "class_property_setter" : "property_setter",
          name + "=", parent_scope, item.span);
      declare_last_value(setter_scope, item.span);
      if (const ast::Expr *signature = node_field(item, "setter_signature")) {
        bind_signature(parent_scope, setter_scope, *signature);
      } else {
        bind_empty_signature(setter_scope, item.span);
      }
      if (const ast::ListField *body = list_field(item, "setter_body")) {
        visit_items(setter_scope, body->values);
      }
    }
  }

  void visit_clause_def(int parent_scope, const ast::Expr &item) {
    const std::string name = string_value(item, "name");
    const int scope = add_scope("function", name, parent_scope, item.span);
    declare_last_value(scope, item.span);

    if (const ast::Expr *signature = node_field(item, "base_signature")) {
      bind_signature(parent_scope, scope, *signature);
    }
    if (const ast::ListField *clauses = list_field(item, "clauses")) {
      for (std::size_t index = 0; index < clauses->values.size(); ++index) {
        visit_clause(scope, name, index, *clauses->values[index]);
      }
    }
    visit_branch_scope(scope, name + ".else", list_field(item, "else_body"),
                       item.span);
  }

  void bind_signature(int owner_scope, int function_scope,
                      const ast::Expr &signature) {
    const ast::ListField *params = list_field(signature, "params");
    if (params == nullptr) {
      return;
    }
    Signature descriptor;
    descriptor.id = "sig" + std::to_string(graph_.signatures.size());
    descriptor.scope_index = function_scope;
    descriptor.owner = graph_.scopes[function_scope].owner;
    descriptor.return_type_expr = string_value(signature, "return_type_expr");
    descriptor.effect_row_expr = string_value(signature, "effect_row_expr");
    descriptor.has_effect_row = bool_value(signature, "has_effect_row");
    descriptor.span = signature.span;

    std::map<std::string, std::size_t> param_positions;
    std::set<std::string> auto_assign_fields;

    for (std::size_t position = 0; position < params->values.size();
         ++position) {
      const std::unique_ptr<ast::Expr> &param = params->values[position];
      const std::string local_name = string_value(*param, "local_name");
      const std::string auto_assign_kind =
          string_value(*param, "auto_assign_kind");

      ParamDescriptor param_descriptor;
      param_descriptor.external_name = string_value(*param, "external_name");
      param_descriptor.local_name = local_name;
      param_descriptor.kind = string_value(*param, "param_kind");
      param_descriptor.auto_assign_kind = auto_assign_kind;
      param_descriptor.type_expr = string_value(*param, "type_expr");
      param_descriptor.span = param->span;
      if (const ast::Expr *default_expr = node_field(*param, "default_expr")) {
        param_descriptor.has_default = true;
        param_descriptor.default_kind = default_expr->kind;
      }
      if (auto_assign_kind == "@" || auto_assign_kind == "@@") {
        param_descriptor.auto_assign_target = auto_assign_kind + local_name;
      }
      descriptor.params.push_back(param_descriptor);

      if (local_name == "_") {
        diagnostic("B0002", "error", "binder",
                   "wildcard '_' cannot be used as an ordinary binding",
                   param->span);
        continue;
      }
      param_positions.emplace(local_name, position);
      declare_binding(function_scope, local_name, "local", "param", param->span,
                      false, "", DuplicatePolicy::Error);

      if (auto_assign_kind == "@" || auto_assign_kind == "@@") {
        auto_assign_fields.insert(auto_assign_kind + local_name);
        const int field_scope = nearest_object_scope(owner_scope);
        const std::string field_name = auto_assign_kind + local_name;
        const int target_scope = field_scope >= 0 ? field_scope : function_scope;
        declare_binding(target_scope, field_name,
                        auto_assign_kind == "@" ? "ivar" : "cvar", "field",
                        param->span, false);
      }
    }
    graph_.signatures.push_back(std::move(descriptor));

    for (std::size_t position = 0; position < params->values.size();
         ++position) {
      const std::unique_ptr<ast::Expr> &param = params->values[position];
      if (const ast::Expr *default_expr = node_field(*param, "default_expr")) {
        preflight_default_expr(*default_expr, position, param_positions,
                               auto_assign_fields);
        visit_expr(function_scope, *default_expr);
      }
    }
  }

  void preflight_default_expr(
      const ast::Expr &default_expr, std::size_t current_position,
      const std::map<std::string, std::size_t> &param_positions,
      const std::set<std::string> &auto_assign_fields) {
    std::vector<const ast::Expr *> names;
    std::vector<const ast::Expr *> fields;
    collect_default_refs(default_expr, &names, &fields);

    for (const ast::Expr *name_expr : names) {
      const std::string name = string_value(*name_expr, "name");
      const auto found = param_positions.find(name);
      if (found != param_positions.end() && found->second >= current_position) {
        diagnostic("E1007", "error", "binder",
                   "default expression refers to a parameter that is not "
                   "available yet",
                   name_expr->span);
      }
    }

    for (const ast::Expr *field_expr : fields) {
      const std::string prefix = field_expr->kind == "AstIvar" ? "@" : "@@";
      const std::string field_name = prefix + string_value(*field_expr, "name");
      if (auto_assign_fields.count(field_name) != 0) {
        diagnostic("W1001", "warning", "binder",
                   "default expression reads field also targeted by delayed "
                   "auto-assign",
                   field_expr->span);
      }
    }
  }

  void visit_class_like(int parent_scope, const ast::Expr &item,
                        const std::string &kind) {
    const std::string name = string_value(item, "name");
    const int scope = add_scope(kind, name, parent_scope, item.span);
    declare_last_value(scope, item.span);

    const std::string superclass = string_value(item, "superclass");
    if (!superclass.empty()) {
      add_reference(scope, superclass, "superclass", item.span,
                    resolve(scope, superclass));
    }

    if (const ast::ListField *body = list_field(item, "body")) {
      predeclare_items(scope, body->values);
      visit_items(scope, body->values);
    }
  }

  void visit_include_like(int scope_index, const ast::Expr &item) {
    const ast::ListField *paths = list_field(item, "paths");
    if (paths == nullptr) {
      return;
    }
    const std::string ref_kind =
        item.kind == "AstExtendStmt" ? "extend" : "include";
    for (const std::unique_ptr<ast::Expr> &path : paths->values) {
      const std::string path_text = string_value(*path, "path");
      add_reference(scope_index, path_text, ref_kind, path->span,
                    resolve(scope_index, path_text));
    }
  }

  int nearest_object_scope(int scope_index) const {
    for (int current = scope_index; current >= 0;
         current = graph_.scopes[current].parent_index) {
      if (graph_.scopes[current].kind == "class" ||
          graph_.scopes[current].kind == "mixin") {
        return current;
      }
    }
    return -1;
  }

  void visit_branch_scope(int parent_scope, const std::string &owner,
                          const ast::ListField *items,
                          const lexer::Span &fallback_span) {
    const int scope = add_scope("block", owner, parent_scope,
                                list_span_or_parent(items, fallback_span));
    declare_last_value(scope, graph_.scopes[scope].span);
    if (items != nullptr) {
      visit_items(scope, items->values);
    }
  }

  void visit_rescue_clause(int parent_scope, const std::string &owner,
                           const ast::Expr &clause) {
    const ast::ListField *body = list_field(clause, "body");
    const int scope = add_scope("block", owner, parent_scope,
                                list_span_or_parent(body, clause.span));
    declare_last_value(scope, graph_.scopes[scope].span);
    const std::string binding = string_value(clause, "binding");
    if (!binding.empty()) {
      if (binding == "_") {
        diagnostic("B0002", "error", "binder",
                   "wildcard '_' cannot be used as an ordinary binding",
                   clause.span);
      } else {
        declare_binding(scope, binding, "local", "rescue_binding",
                        clause.span, false, "", DuplicatePolicy::Error);
      }
    }
    if (body != nullptr) {
      visit_items(scope, body->values);
    }
  }

  void visit_clause(int function_scope, const std::string &function_name,
                    std::size_t clause_index, const ast::Expr &clause) {
    const ast::ListField *body = list_field(clause, "body");
    const int scope = add_scope(
        "block", function_name + ".when." + std::to_string(clause_index),
        function_scope, list_span_or_parent(body, clause.span));
    declare_last_value(scope, graph_.scopes[scope].span);
    declare_clause_pattern_bindings(scope, function_scope, clause);
    if (const ast::Expr *guard = node_field(clause, "guard_expr")) {
      visit_expr(scope, *guard);
    }
    if (body != nullptr) {
      visit_items(scope, body->values);
    }
  }

  std::unique_ptr<ast::Expr>
  parse_and_validate_pattern(const std::string &pattern_text,
                             const lexer::Span &span,
                             pattern::PatternContext context) {
    std::unique_ptr<ast::Expr> parsed =
        pattern::parse_pattern_text(pattern_text, span);
    for (const lexer::Diagnostic &diagnostic :
         pattern::validate_pattern(*parsed, context)) {
      diagnostics_.push_back(diagnostic);
    }
    return parsed;
  }

  void add_pattern_references(int scope_index, int resolve_scope,
                              const ast::Expr &pattern_node) {
    for (const std::string &name :
         pattern::collect_reference_names(pattern_node)) {
      add_reference(scope_index, name, "pattern_ref", pattern_node.span,
                    resolve(resolve_scope, name));
    }
  }

  void declare_scope_pattern_bindings(int scope_index, int resolve_scope,
                                      const std::string &pattern_text,
                                      const lexer::Span &span,
                                      pattern::PatternContext context,
                                      const std::string &role) {
    const std::unique_ptr<ast::Expr> pattern_node =
        parse_and_validate_pattern(pattern_text, span, context);
    add_pattern_references(scope_index, resolve_scope, *pattern_node);
    for (const std::string &name :
         pattern::collect_binding_names(*pattern_node)) {
      declare_binding(scope_index, name, "local", role, span, false, "",
                      DuplicatePolicy::Error);
    }
  }

  void declare_clause_pattern_bindings(int scope_index, int resolve_scope,
                                       const ast::Expr &clause) {
    declare_scope_pattern_bindings(scope_index, resolve_scope,
                                   string_value(clause, "pattern"), clause.span,
                                   pattern::PatternContext::Clause, "pattern");
  }

  void visit_expr(int scope_index, const ast::Expr &expr) {
    if (expr.kind == "AstName") {
      const std::string name = string_value(expr, "name");
      if (name == "_") {
        diagnostic("B0002", "error", "binder",
                   "wildcard '_' cannot be used as an ordinary reference",
                   expr.span);
        add_reference(scope_index, name, "name", expr.span, nullptr);
        return;
      }
      Binding *binding = resolve(scope_index, name);
      if (binding != nullptr && binding_is_property(*binding) &&
          !binding->property_has_getter) {
        diagnostic("AMB_PROP_MISSING_GETTER", "error", "binder",
                   "cannot read from write-only property", expr.span);
      }
      add_reference(scope_index, name, "name", expr.span, binding);
      return;
    }
    if (expr.kind == "AstLastValue") {
      add_reference(scope_index, kLastValueName, "last_value", expr.span,
                    resolve(scope_index, kLastValueName));
      return;
    }
    if (expr.kind == "AstPlaceholder") {
      visit_placeholder(scope_index, expr);
      return;
    }
    if (expr.kind == "AstIvar" || expr.kind == "AstCvar") {
      visit_field_ref(scope_index, expr,
                      expr.kind == "AstIvar" ? "ivar" : "cvar", "read");
      return;
    }
    if (expr.kind == "AstAssign") {
      if (string_value(expr, "op") != "=") {
        if (const ast::Expr *left = node_field(expr, "left")) {
          visit_expr(scope_index, *left);
        }
      }
      if (const ast::Expr *right = node_field(expr, "right")) {
        visit_expr(scope_index, *right);
      }
      if (const ast::Expr *left = node_field(expr, "left")) {
        visit_assignment_left(scope_index, *left);
      }
      return;
    }
    if (expr.kind == "AstPatternAssign") {
      if (const ast::Expr *right = node_field(expr, "right")) {
        visit_expr(scope_index, *right);
      }
      visit_pattern_assignment(scope_index, expr);
      return;
    }
    if (expr.kind == "AstRaise") {
      if (const ast::Expr *value = node_field(expr, "expr")) {
        visit_expr(scope_index, *value);
      }
      return;
    }
    if (expr.kind == "AstThrow") {
      if (const ast::Expr *tag = node_field(expr, "tag")) {
        visit_expr(scope_index, *tag);
      }
      if (const ast::Expr *value = node_field(expr, "value")) {
        visit_expr(scope_index, *value);
      }
      return;
    }
    if (expr.kind == "AstCatch") {
      if (const ast::Expr *tag = node_field(expr, "tag")) {
        visit_expr(scope_index, *tag);
      }
      visit_branch_scope(scope_index, "catch", list_field(expr, "body"),
                         expr.span);
      return;
    }
    if (expr.kind == "AstTry") {
      if (const ast::ListField *body = list_field(expr, "body")) {
        visit_items(scope_index, body->values);
      }
      if (const ast::ListField *rescues = list_field(expr, "rescues")) {
        for (std::size_t index = 0; index < rescues->values.size(); ++index) {
          visit_rescue_clause(scope_index, "try.rescue." + std::to_string(index),
                              *rescues->values[index]);
        }
      }
      if (bool_value(expr, "has_ensure")) {
        visit_branch_scope(scope_index, "try.ensure",
                           list_field(expr, "ensure_body"), expr.span);
      }
      return;
    }
    if (expr.kind == "AstBlock") {
      visit_block(scope_index, expr);
      return;
    }
    if (expr.kind == "AstIf") {
      if (const ast::Expr *cond = node_field(expr, "cond")) {
        visit_expr(scope_index, *cond);
      }
      visit_branch_scope(scope_index, "if.then", list_field(expr, "then_body"),
                         expr.span);
      visit_branch_scope(scope_index, "if.else", list_field(expr, "else_body"),
                         expr.span);
      return;
    }
    if (expr.kind == "AstUnless") {
      if (const ast::Expr *cond = node_field(expr, "cond")) {
        visit_expr(scope_index, *cond);
      }
      visit_branch_scope(scope_index, "unless.then",
                         list_field(expr, "then_body"), expr.span);
      visit_branch_scope(scope_index, "unless.else",
                         list_field(expr, "else_body"), expr.span);
      return;
    }
    if (expr.kind == "AstWhile" || expr.kind == "AstUntil" ||
        expr.kind == "AstDoWhile") {
      if (const ast::Expr *cond = node_field(expr, "cond")) {
        visit_expr(scope_index, *cond);
      }
      visit_branch_scope(scope_index, expr.kind, list_field(expr, "body"),
                         expr.span);
      return;
    }
    if (expr.kind == "AstLoop") {
      visit_branch_scope(scope_index, "AstLoop", list_field(expr, "body"),
                         expr.span);
      return;
    }
    if (expr.kind == "AstCase") {
      if (const ast::Expr *scrutinee = node_field(expr, "scrutinee")) {
        visit_expr(scope_index, *scrutinee);
      }
      if (const ast::ListField *arms = list_field(expr, "arms")) {
        for (std::size_t index = 0; index < arms->values.size(); ++index) {
          visit_case_arm(scope_index, index, *arms->values[index]);
        }
      }
      visit_branch_scope(scope_index, "case.else",
                         list_field(expr, "else_body"), expr.span);
      return;
    }
    if (expr.kind == "AstPostfixChain") {
      visit_postfix_chain(scope_index, expr);
      return;
    }
    if (expr.kind == "AstTailDotMember" || expr.kind == "AstTailSafeMember") {
      return;
    }
    visit_children(scope_index, expr);
  }

  void visit_children(int scope_index, const ast::Expr &expr) {
    for (const ast::NodeField &field : expr.node_fields) {
      if (field.value) {
        visit_expr(scope_index, *field.value);
      }
    }
    for (const ast::ListField &field : expr.list_fields) {
      for (const std::unique_ptr<ast::Expr> &value : field.values) {
        visit_expr(scope_index, *value);
      }
    }
  }

  void visit_postfix_chain(int scope_index, const ast::Expr &expr) {
    const ast::Expr *base = node_field(expr, "base");
    if (base != nullptr) {
      visit_expr(scope_index, *base);
    }
    const ast::ListField *tails = list_field(expr, "tails");
    if (tails == nullptr) {
      return;
    }
    if (base != nullptr && base->kind == "AstName" &&
        !tails->values.empty() &&
        tails->values.front()->kind == "AstTailCall") {
      const Binding *binding =
          resolve(scope_index, string_value(*base, "name"));
      if (binding != nullptr && binding_is_property(*binding)) {
        diagnostic("AMB_PROP_CALLED_AS_METHOD", "error", "binder",
                   "property `" + string_value(*base, "name") +
                       "` is not a method; use `" +
                       string_value(*base, "name") + "` or `" +
                       string_value(*base, "name") +
                       ".()` if the property value is callable",
                   tails->values.front()->span);
      }
    }
    for (std::size_t i = 0; i < tails->values.size(); ++i) {
      const std::unique_ptr<ast::Expr> &tail = tails->values[i];
      if (tail->kind == "AstTailCall" || tail->kind == "AstTailSafeCall" ||
          tail->kind == "AstTailDotCall") {
        if (const ast::ListField *args = list_field(*tail, "args")) {
          for (const std::unique_ptr<ast::Expr> &arg : args->values) {
            visit_expr(scope_index, *arg);
          }
        }
      } else if (tail->kind == "AstTailIndex" ||
                 tail->kind == "AstTailSafeIndex") {
        if (const ast::Expr *index_expr = node_field(*tail, "index_expr")) {
          visit_expr(scope_index, *index_expr);
        }
      } else if (tail->kind == "AstTailBlockSuffix") {
        if (const ast::Expr *block = node_field(*tail, "block")) {
          visit_expr(scope_index, *block);
        }
      } else if (tail->kind == "AstTailDotMember" ||
                 tail->kind == "AstTailSafeMember") {
        const std::string member_name = string_value(*tail, "name");
        const bool followed_by_invocation =
            i + 1 < tails->values.size() &&
            (tails->values[i + 1]->kind == "AstTailCall" ||
             tails->values[i + 1]->kind == "AstTailBlockSuffix");
        if (!member_name.empty() && member_name.back() == '!' &&
            !followed_by_invocation) {
          diagnostic("W_BARE_BANG_CALL", "warning", "binder",
                     "bare access invokes mutating-looking method `" +
                         member_name + "`; prefer `" + member_name + "()`",
                     tail->span);
        }
      }
    }
  }

  void visit_case_arm(int parent_scope, std::size_t arm_index,
                      const ast::Expr &arm) {
    const ast::ListField *body = list_field(arm, "body");
    const int scope =
        add_scope("block", "case.when." + std::to_string(arm_index),
                  parent_scope, list_span_or_parent(body, arm.span));
    declare_last_value(scope, graph_.scopes[scope].span);
    declare_case_arm_pattern_bindings(scope, parent_scope, arm);
    if (const ast::Expr *guard = node_field(arm, "guard_expr")) {
      visit_expr(scope, *guard);
    }
    if (body != nullptr) {
      visit_items(scope, body->values);
    }
  }

  void declare_case_arm_pattern_bindings(int scope_index, int resolve_scope,
                                         const ast::Expr &arm) {
    declare_scope_pattern_bindings(scope_index, resolve_scope,
                                   string_value(arm, "pattern"), arm.span,
                                   pattern::PatternContext::Case, "pattern");
  }

  void visit_assignment_left(int scope_index, const ast::Expr &left) {
    if (left.kind == "AstName") {
      const std::string name = string_value(left, "name");
      if (name == "_") {
        diagnostic("B0002", "error", "binder",
                   "wildcard '_' cannot be used as an ordinary binding",
                   left.span);
        add_reference(scope_index, name, "write", left.span, nullptr);
        return;
      }
      Binding *binding = resolve(scope_index, name);
      if (binding == nullptr) {
        binding = declare_binding(scope_index, name, "local", "local",
                                  left.span, false);
      } else if (binding_is_property(*binding)) {
        if (!binding->property_has_setter) {
          diagnostic("AMB_PROP_MISSING_SETTER", "error", "binder",
                     "cannot assign to read-only property", left.span);
        }
      } else if (binding->kind == "import_alias") {
        diagnostic("E2007", "error", "binder", "assignment to imported alias",
                   left.span);
      }
      add_reference(scope_index, name, "write", left.span, binding);
      return;
    }
    if (left.kind == "AstIvar" || left.kind == "AstCvar") {
      visit_field_ref(scope_index, left,
                      left.kind == "AstIvar" ? "ivar" : "cvar", "write");
      return;
    }
    visit_expr(scope_index, left);
  }

  void visit_pattern_assignment(int scope_index, const ast::Expr &expr) {
    const std::unique_ptr<ast::Expr> pattern_node =
        parse_and_validate_pattern(string_value(expr, "pattern"), expr.span,
                                   pattern::PatternContext::PatternAssignment);
    add_pattern_references(scope_index, scope_index, *pattern_node);
    for (const std::string &name :
         pattern::collect_binding_names(*pattern_node)) {
      Binding *binding = find_local_binding(scope_index, name);
      if (binding == nullptr) {
        binding = declare_binding(scope_index, name, "local", "pattern_assign",
                                  expr.span, false);
      } else if (binding->kind == "import_alias") {
        diagnostic("E2007", "error", "binder", "assignment to imported alias",
                   expr.span);
      }
      add_reference(scope_index, name, "write", expr.span, binding);
    }
  }

  void visit_field_ref(int scope_index, const ast::Expr &expr,
                       const std::string &kind, const std::string &ref_kind) {
    const std::string sigil = kind == "ivar" ? "@" : "@@";
    const std::string name = sigil + string_value(expr, "name");
    int field_scope = nearest_object_scope(scope_index);
    if (field_scope < 0) {
      field_scope = scope_index;
    }
    Binding *binding =
        declare_binding(field_scope, name, kind, "field", expr.span, false);
    add_reference(scope_index, name, ref_kind, expr.span, binding);
  }

  void visit_block(int parent_scope, const ast::Expr &expr) {
    const int scope =
        add_scope("block", "block_suffix", parent_scope, expr.span);
    declare_last_value(scope, expr.span);

    bool has_explicit_params = false;
    if (const ast::ListField *params = list_field(expr, "params")) {
      has_explicit_params = !params->values.empty();
      for (const std::unique_ptr<ast::Expr> &param : params->values) {
        declare_scope_pattern_bindings(
            scope, parent_scope, string_value(*param, "pattern"), param->span,
            pattern::PatternContext::BlockParam, "block_param");
      }
    }

    block_stack_.push_back(
        BlockContext{scope, has_explicit_params, {}, expr.span});
    if (const ast::Expr *body = node_field(expr, "body")) {
      visit_expr(scope, *body);
    } else if (const ast::ListField *body_items = list_field(expr, "body")) {
      visit_items(scope, body_items->values);
    }
    BlockContext context = block_stack_.back();
    block_stack_.pop_back();

    if (!context.has_explicit_params && !context.placeholders.empty()) {
      const int max_placeholder = *context.placeholders.rbegin();
      for (int i = 1; i <= max_placeholder; ++i) {
        if (context.placeholders.count(i) == 0) {
          diagnostic("E1006", "error", "parser", "sparse placeholder numbering",
                     context.span);
          break;
        }
      }
    }
  }

  void visit_placeholder(int scope_index, const ast::Expr &expr) {
    const std::string name = string_value(expr, "name");
    if (block_stack_.empty()) {
      add_reference(scope_index, name, "placeholder", expr.span, nullptr);
      return;
    }

    BlockContext &context = block_stack_.back();
    if (context.has_explicit_params) {
      diagnostic("E1005", "error", "parser",
                 "mixing implicit placeholders with explicit block params",
                 expr.span);
      add_reference(scope_index, name, "placeholder", expr.span, nullptr);
      return;
    }

    context.placeholders.insert(placeholder_number(name));
    Binding *binding = declare_binding(context.scope_index, name, "placeholder",
                                       "implicit_block_param", expr.span, true);
    add_reference(scope_index, name, "placeholder", expr.span, binding);
  }

  BindGraph graph_;
  std::vector<lexer::Diagnostic> diagnostics_;
  std::vector<BlockContext> block_stack_;
};

void append_string_array(std::ostringstream &out,
                         const std::vector<std::string> &values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << "\"" << json_escape(values[i]) << "\"";
  }
  out << "]";
}

bool diagnostics_ok(const std::vector<lexer::Diagnostic> &diagnostics) {
  for (const lexer::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.severity == "error") {
      return false;
    }
  }
  return true;
}

struct NameRefKey {
  std::string file;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  std::string name;
};

NameRefKey name_ref_key(const lexer::Span &span, const std::string &name) {
  return NameRefKey{span.file, span.start.offset, span.end.offset, name};
}

bool same_name_ref_key(const NameRefKey &left, const NameRefKey &right) {
  return left.file == right.file && left.start_offset == right.start_offset &&
         left.end_offset == right.end_offset && left.name == right.name;
}

bool contains_name_ref_key(const std::vector<NameRefKey> &keys,
                           const NameRefKey &target) {
  for (const NameRefKey &key : keys) {
    if (same_name_ref_key(key, target)) {
      return true;
    }
  }
  return false;
}

bool is_native_prelude_name(const std::string &name) {
  static const std::set<std::string> names = {
      "Amber", "Array", "Atomic", "Barrier", "Base64", "Base64Url", "Bool",
      "Bytes", "Channel", "Err", "Flow", "Float", "Hex", "Int", "Json",
      "Kernel", "Map", "Math", "Mutex", "Null", "Object", "Ok", "Range",
      "Set", "Str", "StrictMap", "Symbol", "ThreadedCollection", "Tuple",
      "desc", "io", "p", "pp", "print", "task",
      // Builtin runtime error classes resolve to native prelude constants at
      // runtime (see lookup_native_prelude_constant / runtime_error_id), so they
      // may be referenced in expression position too -- e.g. ValueError.new(msg)
      // or Err(OverflowError.new(msg)). Shared X-macro list keeps the binder and
      // the VM's kRuntimeErrorNames in lockstep with the spec registry.
#define AMBER_RUNTIME_ERROR(error_name) #error_name,
#include "spec/registries/runtime_errors.def"
#undef AMBER_RUNTIME_ERROR
  };
  return names.count(name) != 0U;
}

bool is_reflective_send_call(const ast::Expr &base, const ast::Expr &tail) {
  if (tail.kind != "AstTailCall" || string_value(base, "name") != "send") {
    return false;
  }
  const ast::ListField *args = list_field(tail, "args");
  if (args == nullptr || args->values.size() < 2U) {
    return false;
  }
  return args->values[0]->kind != "AstKeywordArg" &&
         args->values[0]->kind != "AstKeywordSpreadArg" &&
         args->values[0]->kind != "AstSpreadArg" &&
         args->values[1]->kind != "AstKeywordArg" &&
         args->values[1]->kind != "AstKeywordSpreadArg" &&
         args->values[1]->kind != "AstSpreadArg";
}

bool is_kernel_watch_chain(const ast::Expr &base, const ast::ListField &tails) {
  if (base.kind != "AstName" || string_value(base, "name") != "Kernel" ||
      tails.values.size() < 2U) {
    return false;
  }
  const ast::Expr &member = *tails.values[0];
  const ast::Expr &call = *tails.values[1];
  return member.kind == "AstTailDotMember" &&
         string_value(member, "name") == "watch" &&
         call.kind == "AstTailCall";
}

void collect_call_name_contexts(const ast::Expr &expr,
                                std::vector<NameRefKey> *callable_names,
                                std::vector<NameRefKey> *reflective_names) {
  if (expr.kind == "AstPostfixChain") {
    const ast::Expr *base = node_field(expr, "base");
    const ast::ListField *tails = list_field(expr, "tails");
    if (base != nullptr && base->kind == "AstName" && tails != nullptr &&
        !tails->values.empty()) {
      if (is_kernel_watch_chain(*base, *tails)) {
        reflective_names->push_back(
            name_ref_key(base->span, string_value(*base, "name")));
      }
      const ast::Expr &first_tail = *tails->values.front();
      if (first_tail.kind == "AstTailCall" ||
          first_tail.kind == "AstTailSafeCall") {
        const NameRefKey key =
            name_ref_key(base->span, string_value(*base, "name"));
        if (is_reflective_send_call(*base, first_tail)) {
          reflective_names->push_back(key);
        } else {
          callable_names->push_back(key);
        }
      }
    }
  }

  for (const ast::NodeField &field : expr.node_fields) {
    if (field.value) {
      collect_call_name_contexts(*field.value, callable_names,
                                 reflective_names);
    }
  }
  for (const ast::ListField &field : expr.list_fields) {
    for (const std::unique_ptr<ast::Expr> &value : field.values) {
      collect_call_name_contexts(*value, callable_names, reflective_names);
    }
  }
}

} // namespace

bool BindResult::ok() const { return diagnostics_ok(diagnostics); }

CallSiteShape extract_call_shape(const ast::Expr &expr) {
  CallSiteShape result;
  const ast::Expr *tail = nullptr;
  if (expr.kind == "AstTailCall" || expr.kind == "AstTailSafeCall") {
    tail = &expr;
  } else if (expr.kind == "AstPostfixChain") {
    const ast::ListField *tails = list_field(expr, "tails");
    if (tails != nullptr && !tails->values.empty()) {
      const ast::Expr *last_tail = tails->values.back().get();
      if (last_tail->kind == "AstTailCall" ||
          last_tail->kind == "AstTailSafeCall") {
        tail = last_tail;
      }
    }
  }

  if (tail == nullptr) {
    return result;
  }

  result.found = true;
  result.call_kind = tail->kind == "AstTailSafeCall" ? "safe_call" : "call";
  result.call_style = string_value(*tail, "call_style");
  result.span = tail->span;

  const ast::ListField *args = list_field(*tail, "args");
  if (args == nullptr) {
    return result;
  }

  result.args.reserve(args->values.size());
  for (const std::unique_ptr<ast::Expr> &arg_expr : args->values) {
    CallArgShape arg;
    arg.span = arg_expr->span;
    if (arg_expr->kind == "AstKeywordArg") {
      arg.keyword_name = string_value(*arg_expr, "name");
    } else if (arg_expr->kind == "AstSpreadArg") {
      arg.positional_spread = true;
    } else if (arg_expr->kind == "AstKeywordSpreadArg") {
      arg.keyword_spread = true;
    }
    result.args.push_back(std::move(arg));
  }

  return result;
}

bool CallBindResult::ok() const { return diagnostics_ok(diagnostics); }

CallBindResult bind_call_shape(const Signature &signature,
                               const std::vector<CallArgShape> &args) {
  CallBindResult result;
  result.slots.reserve(signature.params.size());

  std::vector<int> positional_indices;
  std::map<std::string, int> first_keyword_indices;
  std::set<std::string> consumed_keywords;
  const bool has_spread = std::any_of(
      args.begin(), args.end(), [](const CallArgShape &arg) {
        return arg.positional_spread || arg.keyword_spread;
      });
  if (has_spread) {
    return result;
  }

  for (std::size_t i = 0; i < args.size(); ++i) {
    const CallArgShape &arg = args[i];
    if (arg.keyword_name.empty()) {
      positional_indices.push_back(static_cast<int>(i));
      continue;
    }
    const auto insert_result =
        first_keyword_indices.emplace(arg.keyword_name, static_cast<int>(i));
    if (!insert_result.second) {
      result.diagnostics.push_back(lexer::Diagnostic{
          "E2008", "error", "binder", "duplicate keyword argument", arg.span});
    }
  }

  std::size_t positional_cursor = 0;
  for (const ParamDescriptor &param : signature.params) {
    BoundCallSlot slot;
    slot.local_name = param.local_name;
    slot.external_name = param.external_name;
    slot.param_kind = param.kind;
    slot.source_kind = "missing";
    slot.param_span = param.span;

    if (param.kind == "keyword") {
      const auto found = first_keyword_indices.find(param.external_name);
      if (found != first_keyword_indices.end()) {
        slot.source_kind = "keyword";
        slot.argument_index = found->second;
        slot.keyword_name = param.external_name;
        slot.argument_span = args[static_cast<std::size_t>(found->second)].span;
        consumed_keywords.insert(param.external_name);
      }
    } else if (param.kind == "block") {
      // RFC block-parameters §5: a block parameter is supplied through the
      // call's block channel, not a positional/keyword argument, and is
      // optional, so it never consumes a positional slot nor is "missing".
      slot.source_kind = "block";
    } else if (positional_cursor < positional_indices.size()) {
      const int arg_index = positional_indices[positional_cursor++];
      slot.source_kind = "positional";
      slot.argument_index = arg_index;
      slot.argument_span = args[static_cast<std::size_t>(arg_index)].span;
    }

    result.slots.push_back(std::move(slot));
  }

  if (positional_cursor < positional_indices.size()) {
    result.diagnostics.push_back(lexer::Diagnostic{
        "E2010", "error", "binder", "too many positional arguments",
        args[static_cast<std::size_t>(positional_indices[positional_cursor])]
            .span});
  }

  for (std::size_t i = 0; i < args.size(); ++i) {
    const CallArgShape &arg = args[i];
    if (!arg.keyword_name.empty() &&
        consumed_keywords.count(arg.keyword_name) == 0 &&
        first_keyword_indices[arg.keyword_name] == static_cast<int>(i)) {
      result.diagnostics.push_back(lexer::Diagnostic{
          "E2009", "error", "binder", "unknown keyword argument", arg.span});
    }
  }

  for (std::size_t i = 0; i < signature.params.size(); ++i) {
    const ParamDescriptor &param = signature.params[i];
    const BoundCallSlot &slot = result.slots[i];
    if (slot.source_kind == "missing" && !param.has_default) {
      result.diagnostics.push_back(
          lexer::Diagnostic{"E2011", "error", "binder",
                            "missing required parameter", param.span});
    }
  }

  if (result.ok()) {
    for (std::size_t i = 0; i < signature.params.size(); ++i) {
      const ParamDescriptor &param = signature.params[i];
      const BoundCallSlot &slot = result.slots[i];
      if (slot.source_kind == "missing" && param.has_default) {
        result.default_order.push_back(static_cast<int>(i));
      }
      if (param.auto_assign_kind == "@" || param.auto_assign_kind == "@@") {
        PendingAutoAssign pending;
        pending.slot_index = static_cast<int>(i);
        pending.target_name = param.auto_assign_target;
        pending.target_kind = param.auto_assign_kind == "@" ? "ivar" : "cvar";
        pending.span = param.span;
        result.pending_auto_assigns.push_back(std::move(pending));
      }
    }
  }

  return result;
}

BindResult bind_module(const std::vector<std::unique_ptr<ast::Expr>> &items,
                       const std::string &module_name) {
  (void)module_name;
  Binder binder;
  return binder.bind(items);
}

std::vector<lexer::Diagnostic> unresolved_name_diagnostics(
    const std::vector<std::unique_ptr<ast::Expr>> &items,
    const BindGraph &graph) {
  std::vector<NameRefKey> callable_names;
  std::vector<NameRefKey> reflective_names;
  for (const std::unique_ptr<ast::Expr> &item : items) {
    if (item) {
      collect_call_name_contexts(*item, &callable_names, &reflective_names);
    }
  }

  std::vector<lexer::Diagnostic> diagnostics;
  for (const Reference &ref : graph.references) {
    if (ref.resolved || ref.ref_kind != "name") {
      continue;
    }
    const NameRefKey key = name_ref_key(ref.span, ref.name);
    if (contains_name_ref_key(reflective_names, key)) {
      continue;
    }
    if (is_native_prelude_name(ref.name)) {
      continue;
    }
    const bool callable = contains_name_ref_key(callable_names, key);
    diagnostics.push_back(lexer::Diagnostic{
        "E2012",
        "error",
        "binder",
        callable ? "undefined callable '" + ref.name + "'"
                 : "undefined name '" + ref.name + "'",
        ref.span});
  }
  return diagnostics;
}

std::string bind_graph_to_json(const BindGraph &graph,
                               const std::string &module_name,
                               const std::string &source_hash) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"format\": \"amber.bind.v1\",\n";
  if (module_name.empty()) {
    out << "  \"module\": null,\n";
  } else {
    out << "  \"module\": \"" << json_escape(module_name) << "\",\n";
  }

  out << "  \"scopes\": [\n";
  for (std::size_t i = 0; i < graph.scopes.size(); ++i) {
    const Scope &scope = graph.scopes[i];
    out << "    {\"id\":\"" << json_escape(scope.id) << "\",\"kind\":\""
        << json_escape(scope.kind) << "\",\"parent\":";
    if (scope.parent_index < 0) {
      out << "null";
    } else {
      out << "\"" << json_escape(graph.scopes[scope.parent_index].id) << "\"";
    }
    out << ",\"owner\":\"" << json_escape(scope.owner) << "\",\"span\":";
    append_span_json(out, scope.span);
    out << ",\"bindings\":";
    append_string_array(out, scope.bindings);
    out << "}";
    if (i + 1 < graph.scopes.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";

  out << "  \"bindings\": [\n";
  for (std::size_t i = 0; i < graph.bindings.size(); ++i) {
    const Binding &binding = graph.bindings[i];
    out << "    {\"id\":\"" << json_escape(binding.id) << "\",\"scope\":\""
        << json_escape(graph.scopes[binding.scope_index].id) << "\",\"name\":\""
        << json_escape(binding.name) << "\",\"kind\":\""
        << json_escape(binding.kind) << "\",\"role\":\""
        << json_escape(binding.role)
        << "\",\"read_only\":" << (binding.read_only ? "true" : "false")
        << ",\"property_has_getter\":"
        << (binding.property_has_getter ? "true" : "false")
        << ",\"property_has_setter\":"
        << (binding.property_has_setter ? "true" : "false")
        << ",\"span\":";
    append_span_json(out, binding.span);
    out << ",\"source\":\"" << json_escape(binding.source) << "\"}";
    if (i + 1 < graph.bindings.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";

  out << "  \"signatures\": [\n";
  for (std::size_t i = 0; i < graph.signatures.size(); ++i) {
    const Signature &signature = graph.signatures[i];
    out << "    {\"id\":\"" << json_escape(signature.id) << "\",\"scope\":\""
        << json_escape(graph.scopes[signature.scope_index].id)
        << "\",\"owner\":\"" << json_escape(signature.owner) << "\",\"span\":";
    append_span_json(out, signature.span);
    if (!signature.return_type_expr.empty()) {
      out << ",\"return_type_expr\":\""
          << json_escape(signature.return_type_expr) << "\"";
    }
    if (signature.has_effect_row) {
      out << ",\"effect_row_expr\":\"" << json_escape(signature.effect_row_expr)
          << "\"";
    }
    out << ",\"params\":[";
    for (std::size_t param_i = 0; param_i < signature.params.size();
         ++param_i) {
      const ParamDescriptor &param = signature.params[param_i];
      if (param_i != 0) {
        out << ",";
      }
      out << "{\"external_name\":\"" << json_escape(param.external_name)
          << "\",\"local_name\":\"" << json_escape(param.local_name)
          << "\",\"kind\":\"" << json_escape(param.kind)
          << "\",\"auto_assign_kind\":\"" << json_escape(param.auto_assign_kind)
          << "\",\"auto_assign_target\":\""
          << json_escape(param.auto_assign_target) << "\",\"type_expr\":\""
          << json_escape(param.type_expr)
          << "\",\"has_default\":" << (param.has_default ? "true" : "false")
          << ",\"default_kind\":\"" << json_escape(param.default_kind)
          << "\",\"span\":";
      append_span_json(out, param.span);
      out << "}";
    }
    out << "]}";
    if (i + 1 < graph.signatures.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";

  out << "  \"references\": [\n";
  for (std::size_t i = 0; i < graph.references.size(); ++i) {
    const Reference &ref = graph.references[i];
    out << "    {\"id\":\"" << json_escape(ref.id) << "\",\"scope\":\""
        << json_escape(graph.scopes[ref.scope_index].id) << "\",\"name\":\""
        << json_escape(ref.name) << "\",\"ref_kind\":\""
        << json_escape(ref.ref_kind)
        << "\",\"resolved\":" << (ref.resolved ? "true" : "false")
        << ",\"binding\":";
    if (ref.resolved) {
      out << "\"" << json_escape(ref.binding_id) << "\"";
    } else {
      out << "null";
    }
    out << ",\"span\":";
    append_span_json(out, ref.span);
    out << "}";
    if (i + 1 < graph.references.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";

  out << "  \"exports\": [\n";
  for (std::size_t i = 0; i < graph.exports.size(); ++i) {
    const Export &export_record = graph.exports[i];
    out << "    {\"local_name\":\"" << json_escape(export_record.local_name)
        << "\",\"public_name\":\"" << json_escape(export_record.public_name)
        << "\",\"resolved\":" << (export_record.resolved ? "true" : "false")
        << ",\"binding\":";
    if (export_record.resolved) {
      out << "\"" << json_escape(export_record.binding_id) << "\"";
    } else {
      out << "null";
    }
    out << ",\"span\":";
    append_span_json(out, export_record.span);
    out << "}";
    if (i + 1 < graph.exports.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"source_hash\": \"sha256:" << source_hash << "\"\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::binder
