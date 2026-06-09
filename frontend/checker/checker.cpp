#include "frontend/checker/checker.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace amber::checker {
namespace {

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

const std::string *string_field_ptr(const ast::Expr &expr,
                                    const std::string &name) {
  for (const ast::StringField &field : expr.string_fields) {
    if (field.name == name) {
      return &field.value;
    }
  }
  return nullptr;
}

std::string string_field(const ast::Expr &expr, const std::string &name) {
  const std::string *value = string_field_ptr(expr, name);
  return value == nullptr ? "" : *value;
}

bool bool_field(const ast::Expr &expr, const std::string &name) {
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

bool is_static_string_literal(const ast::Expr &expr) {
  if (expr.kind == "AstLiteral" && string_field(expr, "token") == "STRING") {
    return true;
  }
  return expr.kind == "AstStringLiteral" && !bool_field(expr, "interpolation");
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

lexer::Diagnostic diagnostic(const std::string &code,
                             const std::string &message,
                             const lexer::Span &span) {
  return lexer::Diagnostic{code, "error", "typed", message, span};
}

lexer::Diagnostic effect_diagnostic(const std::string &code,
                                    const std::string &message,
                                    const lexer::Span &span) {
  return lexer::Diagnostic{code, "error", "effects", message, span};
}

std::string lower_ascii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

TypeTerm named_type(const std::string &name) {
  TypeTerm term;
  term.kind = "Named";
  term.name = name;
  return term;
}

TypeTerm optional_type(TypeTerm inner) {
  TypeTerm term;
  term.kind = "Optional";
  term.args.push_back(std::move(inner));
  return term;
}

TypeTerm union_type(std::vector<TypeTerm> terms) {
  std::vector<TypeTerm> flattened;
  for (TypeTerm &term : terms) {
    if (term.kind == "Union") {
      for (TypeTerm &inner : term.args) {
        flattened.push_back(std::move(inner));
      }
    } else {
      flattened.push_back(std::move(term));
    }
  }
  std::vector<TypeTerm> deduped;
  std::set<std::string> seen;
  for (TypeTerm &term : flattened) {
    const std::string text = type_term_to_string(term);
    if (seen.insert(text).second) {
      deduped.push_back(std::move(term));
    }
  }
  if (deduped.empty()) {
    return named_type("Never");
  }
  if (deduped.size() == 1U) {
    return deduped.front();
  }
  TypeTerm result;
  result.kind = "Union";
  result.args = std::move(deduped);
  return result;
}

bool is_named(const TypeTerm &term, const std::string &name) {
  return term.kind == "Named" && term.name == name;
}

bool is_any(const TypeTerm &term) { return is_named(term, "Any"); }

bool is_null(const TypeTerm &term) { return is_named(term, "Null"); }

bool is_falsey_singleton(const TypeTerm &term) {
  return is_named(term, "False") || is_null(term);
}

bool type_equivalent(const TypeTerm &left, const TypeTerm &right) {
  return type_term_to_string(left) == type_term_to_string(right);
}

bool compatible(const TypeTerm &actual, const TypeTerm &expected) {
  if (is_any(expected) || is_any(actual)) {
    return true;
  }
  if (is_named(expected, "Bool") &&
      (is_named(actual, "Bool") || is_named(actual, "True") ||
       is_named(actual, "False"))) {
    return true;
  }
  if (expected.kind == "Optional") {
    return is_null(actual) || compatible(actual, expected.args.front());
  }
  if (expected.kind == "Union") {
    for (const TypeTerm &option : expected.args) {
      if (compatible(actual, option)) {
        return true;
      }
    }
    return false;
  }
  if (actual.kind == "Union") {
    for (const TypeTerm &option : actual.args) {
      if (!compatible(option, expected)) {
        return false;
      }
    }
    return true;
  }
  if (expected.kind == "Generic" || actual.kind == "Generic") {
    return type_equivalent(actual, expected);
  }
  if (expected.kind == "Tuple" || actual.kind == "Tuple" ||
      expected.kind == "Record" || actual.kind == "Record") {
    return type_equivalent(actual, expected);
  }
  return type_equivalent(actual, expected);
}

TypeTerm truthy_part(const TypeTerm &term) {
  if (is_falsey_singleton(term)) {
    return named_type("Never");
  }
  if (term.kind == "Union") {
    std::vector<TypeTerm> parts;
    for (const TypeTerm &option : term.args) {
      TypeTerm part = truthy_part(option);
      if (!is_named(part, "Never")) {
        parts.push_back(part);
      }
    }
    return union_type(std::move(parts));
  }
  if (is_named(term, "Bool")) {
    return named_type("True");
  }
  return term;
}

TypeTerm falsy_part(const TypeTerm &term) {
  if (is_falsey_singleton(term)) {
    return term;
  }
  if (term.kind == "Union") {
    std::vector<TypeTerm> parts;
    for (const TypeTerm &option : term.args) {
      TypeTerm part = falsy_part(option);
      if (!is_named(part, "Never")) {
        parts.push_back(part);
      }
    }
    return union_type(std::move(parts));
  }
  if (is_named(term, "Bool")) {
    return named_type("False");
  }
  return named_type("Never");
}

class TypeParser {
public:
  TypeParser(std::string source, lexer::Span span)
      : source_(std::move(source)), span_(std::move(span)) {}

  TypeParseResult parse() {
    skip_space();
    if (at_end()) {
      error("empty TypeTerm");
      return result_;
    }
    result_.term = parse_union();
    skip_space();
    if (!at_end()) {
      error("unexpected trailing token in TypeTerm");
    }
    return result_;
  }

private:
  bool at_end() const { return cursor_ >= source_.size(); }

  char current() const { return at_end() ? '\0' : source_[cursor_]; }

  void skip_space() {
    while (!at_end() && std::isspace(static_cast<unsigned char>(current()))) {
      ++cursor_;
    }
  }

  bool match(char c) {
    skip_space();
    if (current() != c) {
      return false;
    }
    ++cursor_;
    return true;
  }

  void consume(char c, const std::string &message) {
    if (!match(c)) {
      error(message);
    }
  }

  void error(const std::string &message) {
    result_.diagnostics.push_back(diagnostic("T0003", message, span_));
  }

  bool identifier_char(unsigned char c) const {
    return std::isalnum(c) || c == '_' || c == '.' || c >= 0x80;
  }

  std::string parse_identifier() {
    skip_space();
    std::string name;
    while (!at_end() &&
           identifier_char(static_cast<unsigned char>(current()))) {
      name.push_back(current());
      ++cursor_;
    }
    if (name.empty()) {
      error("expected type name in TypeTerm");
      return "Any";
    }
    return name;
  }

  TypeTerm parse_union() {
    std::vector<TypeTerm> terms;
    terms.push_back(parse_postfix());
    while (match('|')) {
      terms.push_back(parse_postfix());
    }
    return union_type(std::move(terms));
  }

  TypeTerm parse_postfix() {
    TypeTerm term = parse_primary();
    while (true) {
      if (match('?')) {
        term = optional_type(std::move(term));
        continue;
      }
      if (term.kind == "Named" && match('[')) {
        TypeTerm generic;
        generic.kind = "Generic";
        generic.name = term.name;
        if (!match(']')) {
          do {
            generic.args.push_back(parse_union());
          } while (match(','));
          consume(']', "expected ']' after generic arguments");
        }
        term = std::move(generic);
        continue;
      }
      break;
    }
    return term;
  }

  TypeTerm parse_primary() {
    skip_space();
    if (match('(')) {
      TypeTerm inner = parse_union();
      consume(')', "expected ')' after TypeTerm");
      return inner;
    }
    if (match('[')) {
      TypeTerm tuple;
      tuple.kind = "Tuple";
      if (!match(']')) {
        do {
          tuple.args.push_back(parse_union());
        } while (match(','));
        consume(']', "expected ']' after tuple TypeTerm");
      }
      return tuple;
    }
    if (match('{')) {
      return parse_record();
    }
    return named_type(parse_identifier());
  }

  TypeTerm parse_record() {
    TypeTerm record;
    record.kind = "Record";
    if (match('}')) {
      return record;
    }
    while (!at_end()) {
      skip_space();
      if (match('*')) {
        consume('*', "expected second '*' in exact record marker");
        const std::string marker = parse_identifier();
        if (marker != "Never") {
          error("record exactness marker must be **Never");
        }
        record.exact_record = true;
      } else {
        const std::string name = parse_identifier();
        consume(':', "expected ':' after record field name");
        record.fields.push_back({name, parse_union()});
      }
      if (match('}')) {
        break;
      }
      if (!match(',')) {
        error("expected ',' between record fields");
        break;
      }
      if (match('}')) {
        break;
      }
    }
    return record;
  }

  std::string source_;
  lexer::Span span_;
  std::size_t cursor_ = 0;
  TypeParseResult result_;
};

class Checker {
public:
  Checker(const std::vector<std::unique_ptr<ast::Expr>> &items,
          std::string module_name, const binder::BindGraph &graph)
      : items_(items), module_name_(std::move(module_name)), graph_(graph) {
    for (const binder::Binding &binding : graph_.bindings) {
      bindings_by_id_.emplace(binding.id, &binding);
    }
    for (const binder::Signature &signature : graph_.signatures) {
      signatures_by_owner_.emplace(signature.owner, &signature);
      if (signature.has_effect_row) {
        std::vector<std::string> effects;
        effect::parse_effect_row(signature.effect_row_expr, &effects, nullptr,
                                 signature.owner);
        declared_effects_by_owner_[signature.owner] = std::move(effects);
      }
    }
    for (const binder::Export &export_record : graph_.exports) {
      if (!export_record.resolved) {
        continue;
      }
      const auto found = bindings_by_id_.find(export_record.binding_id);
      if (found != bindings_by_id_.end()) {
        exported_names_.insert(found->second->name);
      }
    }
  }

  CheckResult check() {
    visit_items(items_);
    return std::move(result_);
  }

private:
  using TypeEnv = std::map<std::string, TypeTerm>;
  using EffectSet = std::set<std::string>;

  void visit_items(const std::vector<std::unique_ptr<ast::Expr>> &items) {
    for (const std::unique_ptr<ast::Expr> &item : items) {
      if (item != nullptr) {
        visit_item(*item);
      }
    }
  }

  void visit_item(const ast::Expr &item) {
    if (item.kind == "AstDefStmt" || item.kind == "AstClassMethodDef") {
      check_callable(
          item, item.kind == "AstClassMethodDef" ? "class_method" : "function",
          node_field(item, "signature"), list_field(item, "body"));
      return;
    }
    if (item.kind == "AstPropDef" || item.kind == "AstClassPropDef" ||
        item.kind == "AstAttrDef") {
      const std::string kind =
          item.kind == "AstClassPropDef" ? "class_property" : "property";
      if (bool_field(item, "has_getter")) {
        check_callable(item, kind, nullptr, list_field(item, "getter_body"));
      }
      if (bool_field(item, "has_setter")) {
        check_callable(item, kind + "_setter",
                       node_field(item, "setter_signature"),
                       list_field(item, "setter_body"),
                       string_field(item, "name") + "=");
      }
      return;
    }
    if (item.kind == "AstClauseDef") {
      check_callable(item, "function", node_field(item, "base_signature"),
                     list_field(item, "else_body"));
      return;
    }
    if (item.kind == "AstClassDef" || item.kind == "AstMixinDef") {
      if (const ast::ListField *body = list_field(item, "body")) {
        visit_items(body->values);
      }
    }
  }

  void check_callable(const ast::Expr &item, const std::string &kind,
                      const ast::Expr *signature_node,
                      const ast::ListField *body,
                      const std::string &owner_override = "") {
    const std::string owner =
        owner_override.empty() ? string_field(item, "name") : owner_override;
    const binder::Signature *signature = signature_for_owner(owner);
    const bool exported = exported_names_.count(owner) != 0U;
    CallableBoundary boundary;
    boundary.owner = owner;
    boundary.kind = kind;
    boundary.exported = exported;

    TypeEnv env;
    if (signature != nullptr) {
      for (const binder::ParamDescriptor &param : signature->params) {
        ParamBoundary param_boundary;
        param_boundary.name = param.local_name;
        param_boundary.kind = param.kind;
        param_boundary.has_default = param.has_default;
        if (param.type_expr.empty()) {
          if (exported) {
            result_.diagnostics.push_back(diagnostic(
                "T0001", "exported callable parameter requires type annotation",
                param.span));
          }
          env.emplace(param.local_name, named_type("Any"));
        } else {
          TypeParseResult parsed = parse_type_term(param.type_expr, param.span);
          append_diagnostics(parsed.diagnostics);
          const std::string normalized = type_term_to_string(parsed.term);
          param_boundary.type = normalized;
          env.emplace(param.local_name, parsed.term);
          boundary.type_hooks.push_back("parameter:" + param.local_name + ":" +
                                        normalized);
          check_default_boundary(param, parsed.term, signature_node);
        }
        boundary.params.push_back(std::move(param_boundary));
      }
      if (!signature->return_type_expr.empty()) {
        TypeParseResult parsed =
            parse_type_term(signature->return_type_expr, signature->span);
        append_diagnostics(parsed.diagnostics);
        boundary.return_type = type_term_to_string(parsed.term);
        boundary.type_hooks.push_back("return:" + owner + ":" +
                                      boundary.return_type);
        const TypeTerm observed = infer_body(body, env);
        boundary.observed_return_type = type_term_to_string(observed);
        if (!compatible(observed, parsed.term)) {
          result_.diagnostics.push_back(diagnostic(
              "T0005", "inferred return type does not satisfy return boundary",
              item.span));
        }
      } else if (exported) {
        result_.diagnostics.push_back(diagnostic(
            "T0002", "exported callable requires return type annotation",
            item.span));
      }
    }

    if (signature != nullptr && signature->has_effect_row) {
      boundary.has_effect_row = true;
      std::vector<effect::EffectDiagnostic> effect_diagnostics;
      effect::parse_effect_row(signature->effect_row_expr,
                               &boundary.declared_effects, &effect_diagnostics,
                               owner);
      for (const effect::EffectDiagnostic &diag : effect_diagnostics) {
        result_.diagnostics.push_back(
            effect_diagnostic("FX0001", diag.message, signature->span));
      }
      EffectSet observed;
      collect_body_effects(body, observed);
      boundary.observed_effects = effect::normalize_effects(
          std::vector<std::string>(observed.begin(), observed.end()));
      boundary.effect_hooks.push_back(
          "declared:" + owner + ":" +
          effect::effect_row_to_text(boundary.declared_effects));
      boundary.effect_hooks.push_back(
          "observed:" + owner + ":" +
          effect::effect_row_to_text(boundary.observed_effects));
      result_.effect_summaries.push_back(
          effect::make_effect_summary(owner, kind, boundary.declared_effects,
                                      boundary.observed_effects, true));
      if (!effect::effects_subset_of(boundary.observed_effects,
                                     boundary.declared_effects)) {
        result_.diagnostics.push_back(effect_diagnostic(
            "FX0003",
            "observed effects " +
                effect::effect_row_to_text(boundary.observed_effects) +
                " exceed declared row " +
                effect::effect_row_to_text(boundary.declared_effects),
            item.span));
      }
    }

    const bool has_boundary = exported || !boundary.return_type.empty() ||
                              !boundary.type_hooks.empty() ||
                              boundary.has_effect_row;
    if (has_boundary) {
      result_.boundaries.push_back(std::move(boundary));
    }
  }

  void check_default_boundary(const binder::ParamDescriptor &param,
                              const TypeTerm &expected,
                              const ast::Expr *signature_node) {
    if (!param.has_default || signature_node == nullptr) {
      return;
    }
    const ast::ListField *params = list_field(*signature_node, "params");
    if (params == nullptr) {
      return;
    }
    for (const std::unique_ptr<ast::Expr> &param_node : params->values) {
      if (param_node == nullptr ||
          string_field(*param_node, "local_name") != param.local_name) {
        continue;
      }
      const ast::Expr *default_expr = node_field(*param_node, "default_expr");
      if (default_expr == nullptr) {
        return;
      }
      TypeEnv env;
      const TypeTerm actual = infer_expr(*default_expr, env);
      if (!compatible(actual, expected)) {
        result_.diagnostics.push_back(diagnostic(
            "T0004", "default value does not satisfy parameter type boundary",
            default_expr->span));
      }
      return;
    }
  }

  TypeTerm infer_body(const ast::ListField *body, TypeEnv env) {
    if (body == nullptr || body->values.empty()) {
      return named_type("Null");
    }
    TypeTerm last = named_type("Null");
    for (const std::unique_ptr<ast::Expr> &stmt : body->values) {
      if (stmt != nullptr) {
        last = infer_stmt(*stmt, env);
      }
    }
    return last;
  }

  TypeTerm infer_stmt(const ast::Expr &stmt, TypeEnv &env) {
    if (stmt.kind == "AstExprStmt") {
      if (const ast::Expr *expr = node_field(stmt, "expr")) {
        return infer_expr(*expr, env);
      }
    }
    if (stmt.kind == "AstPassStmt" || stmt.kind == "AstNoopStmt") {
      return named_type("Null");
    }
    return infer_expr(stmt, env);
  }

  TypeTerm infer_expr(const ast::Expr &expr, TypeEnv &env) {
    if (expr.kind == "AstLiteral") {
      const std::string token = string_field(expr, "token");
      if (token == "INTEGER") {
        return named_type("Int");
      }
      if (token == "FLOAT") {
        return named_type("Float");
      }
      if (token == "STRING") {
        return named_type("Str");
      }
      if (token == "SYMBOL") {
        return named_type("Symbol");
      }
      if (token == "KEYWORD_TRUE") {
        return named_type("True");
      }
      if (token == "KEYWORD_FALSE") {
        return named_type("False");
      }
      if (token == "KEYWORD_NULL") {
        return named_type("Null");
      }
    }
    if (expr.kind == "AstStringLiteral") {
      return named_type("Str");
    }
    if (expr.kind == "AstName") {
      const auto found = env.find(string_field(expr, "name"));
      return found == env.end() ? named_type("Any") : found->second;
    }
    if (expr.kind == "AstGroup") {
      const ast::Expr *inner = node_field(expr, "expr");
      return inner == nullptr ? named_type("Any") : infer_expr(*inner, env);
    }
    if (expr.kind == "AstUnary") {
      const std::string op = string_field(expr, "op");
      if (op == "not") {
        return named_type("Bool");
      }
      const ast::Expr *operand = node_field(expr, "operand");
      return operand == nullptr ? named_type("Any") : infer_expr(*operand, env);
    }
    if (expr.kind == "AstBinary" || expr.kind == "AstAssign") {
      return infer_binary(expr, env);
    }
    if (expr.kind == "AstCompareChain") {
      if (const ast::Expr *first = node_field(expr, "first")) {
        (void)infer_expr(*first, env);
      }
      if (const ast::ListField *links = list_field(expr, "links")) {
        for (const std::unique_ptr<ast::Expr> &link : links->values) {
          if (const ast::Expr *right = node_field(*link, "right")) {
            (void)infer_expr(*right, env);
          }
        }
      }
      return named_type("Bool");
    }
    if (expr.kind == "AstInlineIfExpr") {
      TypeEnv then_env = env;
      TypeEnv else_env = env;
      const ast::Expr *consequent = node_field(expr, "consequent");
      const ast::Expr *alternative = node_field(expr, "alternative");
      const TypeTerm then_type = consequent == nullptr
                                     ? named_type("Any")
                                     : infer_expr(*consequent, then_env);
      const TypeTerm else_type = alternative == nullptr
                                     ? named_type("Any")
                                     : infer_expr(*alternative, else_env);
      return union_type({then_type, else_type});
    }
    if (expr.kind == "AstIf" || expr.kind == "AstUnless") {
      return infer_if(expr, env);
    }
    if (expr.kind == "AstCase") {
      return infer_case(expr, env);
    }
    if (expr.kind == "AstListLiteral") {
      TypeTerm term;
      term.kind = "Generic";
      term.name = "Array";
      term.args.push_back(named_type("Any"));
      return term;
    }
    if (expr.kind == "AstTupleLiteral") {
      TypeTerm term;
      term.kind = "Tuple";
      if (const ast::ListField *elements = list_field(expr, "elements")) {
        for (std::size_t i = 0; i < elements->values.size(); ++i) {
          term.args.push_back(named_type("Any"));
        }
      }
      return term;
    }
    if (expr.kind == "AstSetLiteral") {
      TypeTerm term;
      term.kind = "Generic";
      term.name = "Set";
      term.args.push_back(named_type("Any"));
      return term;
    }
    if (expr.kind == "AstMapLiteral") {
      TypeTerm term;
      term.kind = "Generic";
      term.name = "Map";
      term.args.push_back(named_type("Symbol"));
      term.args.push_back(named_type("Any"));
      return term;
    }
    return named_type("Any");
  }

  TypeTerm infer_binary(const ast::Expr &expr, TypeEnv &env) {
    const std::string op = string_field(expr, "op");
    const ast::Expr *left_expr = node_field(expr, "left");
    const ast::Expr *right_expr = node_field(expr, "right");
    const TypeTerm left =
        left_expr == nullptr ? named_type("Any") : infer_expr(*left_expr, env);
    if (op == "=") {
      const TypeTerm right = right_expr == nullptr
                                 ? named_type("Any")
                                 : infer_expr(*right_expr, env);
      if (left_expr != nullptr && left_expr->kind == "AstName") {
        env[string_field(*left_expr, "name")] = right;
      }
      return right;
    }
    const std::string compound_op = compound_assignment_binary_op(op);
    if (!compound_op.empty()) {
      const TypeTerm right = right_expr == nullptr
                                 ? named_type("Any")
                                 : infer_expr(*right_expr, env);
      TypeTerm result = named_type("Any");
      if ((compound_op == "+" || compound_op == "-" || compound_op == "*" ||
           compound_op == "/" || compound_op == "%" || compound_op == "//" ||
           compound_op == "**") &&
          (is_named(left, "Float") || is_named(right, "Float"))) {
        result = named_type("Float");
      } else if (compound_op == "+" || compound_op == "-" ||
                 compound_op == "*" || compound_op == "/" ||
                 compound_op == "%" || compound_op == "//" ||
                 compound_op == "&" || compound_op == "|" ||
                 compound_op == "^" || compound_op == "<<" ||
                 compound_op == ">>" || compound_op == "**") {
        result = named_type("Int");
      }
      if (left_expr != nullptr && left_expr->kind == "AstName") {
        env[string_field(*left_expr, "name")] = result;
      }
      return result;
    }
    if (op == "and") {
      TypeEnv narrowed = env;
      const TypeTerm right = right_expr == nullptr
                                 ? named_type("Any")
                                 : infer_expr(*right_expr, narrowed);
      return union_type({falsy_part(left), right});
    }
    if (op == "or") {
      TypeEnv narrowed = env;
      const TypeTerm right = right_expr == nullptr
                                 ? named_type("Any")
                                 : infer_expr(*right_expr, narrowed);
      return union_type({truthy_part(left), right});
    }
    if (op == ".." || op == "...") {
      if (right_expr != nullptr) {
        (void)infer_expr(*right_expr, env);
      }
      if (const ast::Expr *step = node_field(expr, "step")) {
        (void)infer_expr(*step, env);
      }
      return named_type("Range");
    }
    if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
        op == ">=" || op == "in") {
      return named_type("Bool");
    }
    const TypeTerm right = right_expr == nullptr ? named_type("Any")
                                                 : infer_expr(*right_expr, env);
    if (op == "<=>") {
      return named_type("Int");
    }
    if ((op == "+" || op == "-" || op == "*" || op == "/" || op == "%" ||
         op == "//" || op == "**") &&
        (is_named(left, "Float") || is_named(right, "Float"))) {
      return named_type("Float");
    }
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%" ||
        op == "//" || op == "&" || op == "|" || op == "^" || op == "<<" ||
        op == ">>" || op == "**") {
      return named_type("Int");
    }
    return named_type("Any");
  }

  TypeTerm infer_if(const ast::Expr &expr, TypeEnv env) {
    TypeTerm then_type = infer_body(list_field(expr, "then_body"), env);
    TypeTerm else_type = named_type("Null");
    if (const ast::ListField *else_body = list_field(expr, "else_body")) {
      else_type = infer_body(else_body, env);
    }
    return union_type({then_type, else_type});
  }

  TypeTerm infer_case(const ast::Expr &expr, TypeEnv env) {
    const ast::Expr *scrutinee = node_field(expr, "scrutinee");
    const TypeTerm subject =
        scrutinee == nullptr ? named_type("Any") : infer_expr(*scrutinee, env);
    const ast::ListField *arms = list_field(expr, "arms");
    std::vector<TypeTerm> branch_types;
    if (arms != nullptr) {
      for (const std::unique_ptr<ast::Expr> &arm : arms->values) {
        if (arm != nullptr) {
          branch_types.push_back(infer_body(list_field(*arm, "body"), env));
        }
      }
    }
    const ast::ListField *else_body = list_field(expr, "else_body");
    if (else_body != nullptr && !else_body->values.empty()) {
      branch_types.push_back(infer_body(else_body, env));
    } else if (bool_field(expr, "strict")) {
      if (!case_exhaustive(subject, arms)) {
        result_.diagnostics.push_back(diagnostic(
            "T0006", "non-exhaustive case! in typed profile", expr.span));
      }
    } else {
      branch_types.push_back(named_type("Null"));
    }
    return union_type(std::move(branch_types));
  }

  bool case_exhaustive(const TypeTerm &subject, const ast::ListField *arms) {
    if (arms == nullptr) {
      return false;
    }
    if (is_named(subject, "Bool") || is_named(subject, "True") ||
        is_named(subject, "False")) {
      bool saw_true = false;
      bool saw_false = false;
      for (const std::unique_ptr<ast::Expr> &arm : arms->values) {
        if (arm == nullptr) {
          continue;
        }
        const std::string pattern = string_field(*arm, "pattern");
        saw_true = saw_true || pattern == "true";
        saw_false = saw_false || pattern == "false";
      }
      return saw_true && saw_false;
    }
    return false;
  }

  void collect_body_effects(const ast::ListField *body, EffectSet &effects) {
    if (body == nullptr) {
      return;
    }
    for (const std::unique_ptr<ast::Expr> &stmt : body->values) {
      if (stmt != nullptr) {
        collect_expr_effects(*stmt, effects);
      }
    }
  }

  void collect_list_effects(const ast::ListField &list, EffectSet &effects) {
    for (const std::unique_ptr<ast::Expr> &item : list.values) {
      if (item != nullptr) {
        collect_expr_effects(*item, effects);
      }
    }
  }

  std::string host_effect_for_name(const std::string &name) const {
    const std::string lower = lower_ascii(name);
    if (lower == "file" || lower == "fs" || lower == "filesystem") {
      return "fs";
    }
    if (lower == "http" || lower == "net" || lower == "socket") {
      return "net";
    }
    if (lower == "env" || lower == "environment") {
      return "env";
    }
    if (lower == "clock" || lower == "time") {
      return "time";
    }
    if (lower == "random" || lower == "rng") {
      return "random";
    }
    if (lower == "ffi" || lower == "native") {
      return "ffi";
    }
    if (lower == "db" || lower == "database") {
      return "db";
    }
    if (lower == "gpu" || lower == "accelerator") {
      return "gpu";
    }
    if (lower == "trace" || lower == "telemetry") {
      return "trace";
    }
    if (lower == "workflow") {
      return "workflow";
    }
    if (lower == "task" || lower == "await" || lower == "awaitable" ||
        lower == "spawn_task") {
      return "async";
    }
    if (lower == "strand" || lower == "channel" || lower == "mutex" ||
        lower == "atomic") {
      return "strand";
    }
    return "";
  }

  void collect_postfix_effects(const ast::Expr &expr, EffectSet &effects) {
    const ast::Expr *base = node_field(expr, "base");
    const ast::ListField *tails = list_field(expr, "tails");
    if (base == nullptr || tails == nullptr) {
      return;
    }
    std::string base_name;
    if (base->kind == "AstName") {
      base_name = string_field(*base, "name");
    }
    std::string first_member;
    bool has_call = false;
    for (const std::unique_ptr<ast::Expr> &tail : tails->values) {
      if (tail == nullptr) {
        continue;
      }
      if ((tail->kind == "AstTailDotMember" ||
           tail->kind == "AstTailSafeMember") &&
          first_member.empty()) {
        first_member = string_field(*tail, "name");
      }
      if (tail->kind == "AstTailCall" || tail->kind == "AstTailSafeCall") {
        has_call = true;
      }
    }
    if (!has_call) {
      return;
    }
    const auto declared = declared_effects_by_owner_.find(base_name);
    if (declared != declared_effects_by_owner_.end()) {
      effects.insert(declared->second.begin(), declared->second.end());
    }
    const std::string base_effect = host_effect_for_name(base_name);
    if (!base_effect.empty()) {
      effects.insert(base_effect);
    }
    const std::string member_effect = host_effect_for_name(first_member);
    if (!member_effect.empty()) {
      effects.insert(member_effect);
    }
    if (base_name == "send") {
      effects.insert("reflect");
      bool literal_selector = false;
      for (const std::unique_ptr<ast::Expr> &tail : tails->values) {
        if (tail == nullptr || tail->kind != "AstTailCall") {
          continue;
        }
        const ast::ListField *args = list_field(*tail, "args");
        if (args != nullptr && args->values.size() >= 2U &&
            args->values[1] != nullptr &&
            is_static_string_literal(*args->values[1])) {
          literal_selector = true;
        }
      }
      if (!literal_selector) {
        effects.insert("unsafe");
      }
    }
  }

  void collect_expr_effects(const ast::Expr &expr, EffectSet &effects) {
    if (expr.kind == "AstAssign" || expr.kind == "AstPatternAssign") {
      effects.insert("mut");
    } else if (expr.kind == "AstPostfixChain") {
      collect_postfix_effects(expr, effects);
    }

    for (const ast::NodeField &field : expr.node_fields) {
      if (field.value != nullptr) {
        collect_expr_effects(*field.value, effects);
      }
    }
    for (const ast::ListField &field : expr.list_fields) {
      collect_list_effects(field, effects);
    }
  }

  void append_diagnostics(const std::vector<lexer::Diagnostic> &diagnostics) {
    result_.diagnostics.insert(result_.diagnostics.end(), diagnostics.begin(),
                               diagnostics.end());
  }

  const binder::Signature *signature_for_owner(const std::string &owner) const {
    const auto found = signatures_by_owner_.find(owner);
    return found == signatures_by_owner_.end() ? nullptr : found->second;
  }

  const std::vector<std::unique_ptr<ast::Expr>> &items_;
  std::string module_name_;
  const binder::BindGraph &graph_;
  std::map<std::string, const binder::Binding *> bindings_by_id_;
  std::multimap<std::string, const binder::Signature *> signatures_by_owner_;
  std::map<std::string, std::vector<std::string>> declared_effects_by_owner_;
  std::set<std::string> exported_names_;
  CheckResult result_;
};

} // namespace

TypeParseResult parse_type_term(const std::string &source,
                                const lexer::Span &span) {
  TypeParser parser(source, span);
  return parser.parse();
}

std::string type_term_to_string(const TypeTerm &term) {
  if (term.kind == "Named") {
    return term.name.empty() ? "Any" : term.name;
  }
  if (term.kind == "Optional") {
    const std::string inner = type_term_to_string(term.args.front());
    return term.args.front().kind == "Union" ? "(" + inner + ")?" : inner + "?";
  }
  if (term.kind == "Union") {
    std::ostringstream out;
    for (std::size_t i = 0; i < term.args.size(); ++i) {
      if (i != 0U) {
        out << " | ";
      }
      out << type_term_to_string(term.args[i]);
    }
    return out.str();
  }
  if (term.kind == "Generic") {
    std::ostringstream out;
    out << term.name << "[";
    for (std::size_t i = 0; i < term.args.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << type_term_to_string(term.args[i]);
    }
    out << "]";
    return out.str();
  }
  if (term.kind == "Tuple") {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < term.args.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << type_term_to_string(term.args[i]);
    }
    out << "]";
    return out.str();
  }
  if (term.kind == "Record") {
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto &field : term.fields) {
      if (!first) {
        out << ", ";
      }
      first = false;
      out << field.first << ": " << type_term_to_string(field.second);
    }
    if (term.exact_record) {
      if (!first) {
        out << ", ";
      }
      out << "**Never";
    }
    out << "}";
    return out.str();
  }
  return "Any";
}

CheckResult check_module(const std::vector<std::unique_ptr<ast::Expr>> &items,
                         const std::string &module_name,
                         const binder::BindGraph &bind_graph) {
  Checker checker(items, module_name, bind_graph);
  return checker.check();
}

std::string check_result_to_json(const CheckResult &result,
                                 const std::string &module_name) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.typed.v1\",\n";
  out << "  \"status\": \"" << (result.ok() ? "ok" : "error") << "\",\n";
  out << "  \"module\": \"" << json_escape(module_name) << "\",\n";
  out << "  \"boundaries\": [\n";
  for (std::size_t i = 0; i < result.boundaries.size(); ++i) {
    const CallableBoundary &boundary = result.boundaries[i];
    out << "    {\"owner\":\"" << json_escape(boundary.owner)
        << "\",\"kind\":\"" << json_escape(boundary.kind)
        << "\",\"exported\":" << (boundary.exported ? "true" : "false")
        << ",\"return_type\":\"" << json_escape(boundary.return_type)
        << "\",\"observed_return_type\":\""
        << json_escape(boundary.observed_return_type) << "\",\"params\":[";
    for (std::size_t param_i = 0; param_i < boundary.params.size(); ++param_i) {
      const ParamBoundary &param = boundary.params[param_i];
      if (param_i != 0U) {
        out << ",";
      }
      out << "{\"name\":\"" << json_escape(param.name) << "\",\"kind\":\""
          << json_escape(param.kind) << "\",\"type\":\""
          << json_escape(param.type)
          << "\",\"has_default\":" << (param.has_default ? "true" : "false")
          << "}";
    }
    out << "],\"type_hooks\":[";
    for (std::size_t hook_i = 0; hook_i < boundary.type_hooks.size();
         ++hook_i) {
      if (hook_i != 0U) {
        out << ", ";
      }
      out << "\"" << json_escape(boundary.type_hooks[hook_i]) << "\"";
    }
    out << "]";
    if (boundary.has_effect_row) {
      out << ",\"effect_row\":\""
          << json_escape(effect::effect_row_to_text(boundary.declared_effects))
          << "\",\"observed_effects\":\""
          << json_escape(effect::effect_row_to_text(boundary.observed_effects))
          << "\",\"effect_hooks\":[";
      for (std::size_t hook_i = 0; hook_i < boundary.effect_hooks.size();
           ++hook_i) {
        if (hook_i != 0U) {
          out << ", ";
        }
        out << "\"" << json_escape(boundary.effect_hooks[hook_i]) << "\"";
      }
      out << "]";
    }
    out << "}";
    if (i + 1U < result.boundaries.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"diagnostic_count\": " << result.diagnostics.size() << "\n";
  out << "}\n";
  return out.str();
}

std::string effects_result_to_json(const CheckResult &result,
                                   const std::string &module_name) {
  bool effects_ok = true;
  for (const lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.phase == "effects") {
      effects_ok = false;
      break;
    }
  }

  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.effects.v1\",\n";
  out << "  \"status\": \"" << (effects_ok ? "ok" : "error") << "\",\n";
  out << "  \"module\": \"" << json_escape(module_name) << "\",\n";
  out << "  \"summaries\": [";
  for (std::size_t i = 0; i < result.effect_summaries.size(); ++i) {
    const effect::EffectSummary &summary = result.effect_summaries[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"owner\":\"" << json_escape(summary.owner)
        << "\",\"kind\":\"" << json_escape(summary.kind) << "\",\"declared\":\""
        << json_escape(effect::effect_row_to_text(summary.declared_effects))
        << "\",\"observed\":\""
        << json_escape(effect::effect_row_to_text(summary.observed_effects))
        << "\",\"flags\":" << summary.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"diagnostics\": [";
  bool first = true;
  std::size_t diagnostic_count = 0;
  for (const lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.phase != "effects") {
      continue;
    }
    ++diagnostic_count;
    if (!first) {
      out << ",";
    }
    first = false;
    out << "\n    {\"code\":\"" << json_escape(diagnostic.code)
        << "\",\"message\":\"" << json_escape(diagnostic.message)
        << "\",\"span\":{\"file\":\"" << json_escape(diagnostic.span.file)
        << "\",\"line\":" << diagnostic.span.start.line
        << ",\"col\":" << diagnostic.span.start.col << "}}";
  }
  out << "\n  ],\n";
  out << "  \"diagnostic_count\": " << diagnostic_count << "\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::checker
