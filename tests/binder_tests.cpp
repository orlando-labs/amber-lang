#include "frontend/binder/binder.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

amber::binder::BindResult bind_ok(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    std::exit(1);
  }
  return bind_result;
}

amber::binder::BindResult bind_any(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  return amber::binder::bind_module(parse_result.items,
                                    parse_result.module_name);
}

std::vector<amber::lexer::Diagnostic>
unresolved_name_diagnostics_for(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    std::exit(1);
  }

  return amber::binder::unresolved_name_diagnostics(parse_result.items,
                                                    bind_result.graph);
}

std::unique_ptr<amber::ast::Expr> parse_expr_ok(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<expr>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseResult parse_result = parser.parse_expression_unit();
  if (!parse_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  return std::move(parse_result.expr);
}

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "binder test failed: " << message << "\n";
    std::exit(1);
  }
}

const amber::binder::Scope *
scope_by_kind_owner(const amber::binder::BindGraph &graph,
                    const std::string &kind, const std::string &owner) {
  for (const amber::binder::Scope &scope : graph.scopes) {
    if (scope.kind == kind && scope.owner == owner) {
      return &scope;
    }
  }
  return nullptr;
}

const amber::binder::Binding *
binding_by_id(const amber::binder::BindGraph &graph, const std::string &id) {
  for (const amber::binder::Binding &binding : graph.bindings) {
    if (binding.id == id) {
      return &binding;
    }
  }
  return nullptr;
}

const amber::binder::Binding *
binding_in_scope(const amber::binder::BindGraph &graph,
                 const amber::binder::Scope &scope, const std::string &name) {
  for (const std::string &binding_id : scope.bindings) {
    const amber::binder::Binding *binding = binding_by_id(graph, binding_id);
    if (binding != nullptr && binding->name == name) {
      return binding;
    }
  }
  return nullptr;
}

const amber::binder::Signature *
signature_by_owner(const amber::binder::BindGraph &graph,
                   const std::string &owner) {
  for (const amber::binder::Signature &signature : graph.signatures) {
    if (signature.owner == owner) {
      return &signature;
    }
  }
  return nullptr;
}

bool has_resolved_reference(const amber::binder::BindGraph &graph,
                            const std::string &name,
                            const std::string &binding_id) {
  for (const amber::binder::Reference &ref : graph.references) {
    if (ref.name == name && ref.resolved && ref.binding_id == binding_id) {
      return true;
    }
  }
  return false;
}

void expect_diagnostic_code(const amber::binder::BindResult &result,
                            const std::string &code) {
  for (const amber::lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return;
    }
  }
  std::cerr << "missing diagnostic " << code << "\n";
  std::exit(1);
}

void expect_no_diagnostic_code(const amber::binder::BindResult &result,
                               const std::string &code) {
  for (const amber::lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      std::cerr << "unexpected diagnostic " << code << "\n";
      std::exit(1);
    }
  }
}

void expect_call_diagnostic_code(const amber::binder::CallBindResult &result,
                                 const std::string &code) {
  for (const amber::lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return;
    }
  }
  std::cerr << "missing call diagnostic " << code << "\n";
  std::exit(1);
}

amber::lexer::Span test_span(std::size_t offset) {
  amber::lexer::Span span;
  span.file = "<call>";
  span.start.line = 1;
  span.start.col = offset + 1;
  span.start.offset = offset;
  span.end.line = 1;
  span.end.col = offset + 2;
  span.end.offset = offset + 1;
  return span;
}

amber::binder::CallArgShape positional_arg(std::size_t offset) {
  amber::binder::CallArgShape arg;
  arg.span = test_span(offset);
  return arg;
}

amber::binder::CallArgShape keyword_arg(const std::string &name,
                                        std::size_t offset) {
  amber::binder::CallArgShape arg;
  arg.keyword_name = name;
  arg.span = test_span(offset);
  return arg;
}

void test_module_class_and_unicode_bindings() {
  const std::string source = "package physics.core\n"
                             "import math.constants as consts\n"
                             "from lab.units import Meter as Метр, Second\n"
                             "export Particle, Метр\n"
                             "\n"
                             "mixin Timestamped:\n"
                             "  def touch!():\n"
                             "    noop\n"
                             "\n"
                             "class Particle < Entity:\n"
                             "  include Timestamped\n"
                             "  class_method def find(id):\n"
                             "    id\n"
                             "  def init(@масса, α = 1):\n"
                             "    pass\n";

  amber::binder::BindResult result = bind_ok(source);
  const amber::binder::BindGraph &graph = result.graph;

  const amber::binder::Scope *module = scope_by_kind_owner(graph, "module", "");
  expect(module != nullptr, "module scope exists");
  expect(binding_in_scope(graph, *module, "consts") != nullptr,
         "module import alias binding");
  expect(binding_in_scope(graph, *module, "Метр") != nullptr,
         "unicode from-import binding");
  const amber::binder::Binding *particle =
      binding_in_scope(graph, *module, "Particle");
  expect(particle != nullptr && particle->kind == "constant" &&
             particle->role == "class",
         "class binding");

  expect(graph.exports.size() == 2, "export count");
  expect(graph.exports[0].resolved &&
             graph.exports[0].binding_id == particle->id,
         "forward export resolves to class binding");

  const amber::binder::Scope *class_scope =
      scope_by_kind_owner(graph, "class", "Particle");
  expect(class_scope != nullptr, "class scope exists");
  expect(binding_in_scope(graph, *class_scope, "@масса") != nullptr,
         "unicode ivar binding from auto-assign");
  const amber::binder::Signature *init_signature =
      signature_by_owner(graph, "init");
  expect(init_signature != nullptr, "init signature descriptor");
  expect(init_signature->params.size() == 2, "init signature param count");
  expect(init_signature->params[0].local_name == "масса",
         "unicode auto-assign param name");
  expect(init_signature->params[0].auto_assign_target == "@масса",
         "auto-assign target");
  expect(init_signature->params[1].local_name == "α",
         "unicode default param name");
  expect(init_signature->params[1].has_default, "default metadata");
  expect(init_signature->params[1].default_kind == "AstLiteral",
         "default kind metadata");

  const amber::binder::Scope *find_scope =
      scope_by_kind_owner(graph, "class_method", "find");
  expect(find_scope != nullptr, "class method scope exists");
  const amber::binder::Binding *id_param =
      binding_in_scope(graph, *find_scope, "id");
  expect(id_param != nullptr && id_param->role == "param", "param binding");
  expect(has_resolved_reference(graph, "id", id_param->id),
         "method body resolves param reference");
}

void test_top_level_assignment_predeclaration() {
  amber::binder::BindResult result = bind_ok("export value\n"
                                             "value = 1\n"
                                             "def read():\n"
                                             "  value\n");
  const amber::binder::BindGraph &graph = result.graph;
  const amber::binder::Scope *module = scope_by_kind_owner(graph, "module", "");
  expect(module != nullptr, "module scope exists");
  const amber::binder::Binding *value =
      binding_in_scope(graph, *module, "value");
  expect(value != nullptr && value->kind == "local" &&
             value->role == "module_cell",
         "top-level assignment predeclares module cell");
  expect(graph.exports.size() == 1 && graph.exports[0].resolved &&
             graph.exports[0].binding_id == value->id,
         "export resolves forward top-level assignment");
  expect(has_resolved_reference(graph, "value", value->id),
         "method body resolves module cell read");
}

void test_implicit_block_placeholders() {
  const std::string source = "def map(xs):\n"
                             "  xs.map: _1 + _2\n";

  amber::binder::BindResult result = bind_ok(source);
  const amber::binder::BindGraph &graph = result.graph;

  const amber::binder::Scope *function_scope =
      scope_by_kind_owner(graph, "function", "map");
  expect(function_scope != nullptr, "function scope exists");
  const amber::binder::Binding *xs_param =
      binding_in_scope(graph, *function_scope, "xs");
  expect(xs_param != nullptr, "function param binding");
  expect(has_resolved_reference(graph, "xs", xs_param->id),
         "postfix base resolves to param");

  const amber::binder::Scope *block_scope =
      scope_by_kind_owner(graph, "block", "block_suffix");
  expect(block_scope != nullptr, "block suffix scope exists");
  const amber::binder::Binding *first =
      binding_in_scope(graph, *block_scope, "_1");
  const amber::binder::Binding *second =
      binding_in_scope(graph, *block_scope, "_2");
  expect(first != nullptr && first->kind == "placeholder", "_1 binding");
  expect(second != nullptr && second->kind == "placeholder", "_2 binding");
  expect(has_resolved_reference(graph, "_1", first->id), "_1 resolves");
  expect(has_resolved_reference(graph, "_2", second->id), "_2 resolves");
}

void test_assignment_to_import_alias_is_error() {
  const std::string source = "import math.constants as consts\n"
                             "consts = 1\n";

  amber::binder::BindResult result = bind_any(source);
  expect(!result.ok(), "import alias write is rejected");
  expect_diagnostic_code(result, "E2007");
}

void test_placeholder_diagnostics() {
  amber::binder::BindResult mixed = bind_any("def f(xs):\n"
                                             "  xs.map |x|: _1\n");
  expect(!mixed.ok(), "explicit block params reject placeholders");
  expect_diagnostic_code(mixed, "E1005");

  amber::binder::BindResult sparse = bind_any("def f(xs):\n"
                                              "  xs.map: _2\n");
  expect(!sparse.ok(), "sparse placeholders rejected");
  expect_diagnostic_code(sparse, "E1006");
}

void test_duplicate_binding_diagnostics() {
  amber::binder::BindResult duplicate_param = bind_any("def f(x, x):\n"
                                                       "  pass\n");
  expect(!duplicate_param.ok(), "duplicate params rejected");
  expect_diagnostic_code(duplicate_param, "B0001");

  amber::binder::BindResult import_collision =
      bind_any("import a.b as item\n"
               "from c import D as item\n");
  expect(!import_collision.ok(), "import alias collisions rejected");
  expect_diagnostic_code(import_collision, "B0001");
}

void test_wildcard_name_diagnostics() {
  amber::binder::BindResult read = bind_any("def f():\n"
                                            "  _\n");
  expect(!read.ok(), "wildcard read rejected");
  expect_diagnostic_code(read, "B0002");

  amber::binder::BindResult write = bind_any("def f():\n"
                                             "  _ = 1\n");
  expect(!write.ok(), "wildcard write rejected");
  expect_diagnostic_code(write, "B0002");
}

void test_unresolved_name_diagnostics() {
  std::vector<amber::lexer::Diagnostic> bare =
      unresolved_name_diagnostics_for("x\n");
  expect(bare.size() == 1U, "bare unresolved name gets one diagnostic");
  expect(bare[0].code == "E2012", "bare unresolved name diagnostic code");
  expect(bare[0].message == "undefined name 'x'",
         "bare unresolved name diagnostic message");

  std::vector<amber::lexer::Diagnostic> call =
      unresolved_name_diagnostics_for("f(x)\n");
  expect(call.size() == 2U, "call unresolved names get two diagnostics");
  expect(call[0].message == "undefined callable 'f'",
         "call base diagnostic is callable-specific");
  expect(call[1].message == "undefined name 'x'",
         "call arg diagnostic remains ordinary name-specific");

  std::vector<amber::lexer::Diagnostic> literal_call =
      unresolved_name_diagnostics_for("f(1)\n");
  expect(literal_call.size() == 1U,
         "literal call unresolved callable gets one diagnostic");
  expect(literal_call[0].message == "undefined callable 'f'",
         "literal call diagnostic is callable-specific");

  std::vector<amber::lexer::Diagnostic> reflective_send =
      unresolved_name_diagnostics_for("receiver = 1\n"
                                      "send(receiver, \"tick\")\n");
  expect(reflective_send.empty(),
         "reflective send builtin is not reported as undefined");
}

void test_default_ordering_diagnostics() {
  amber::binder::BindResult rightward = bind_any("def f(x = y, y):\n"
                                                 "  pass\n");
  expect(!rightward.ok(), "rightward default reference rejected");
  expect_diagnostic_code(rightward, "E1007");

  amber::binder::BindResult self = bind_any("def f(x = x):\n"
                                            "  pass\n");
  expect(!self.ok(), "self default reference rejected");
  expect_diagnostic_code(self, "E1007");

  amber::binder::BindResult leftward = bind_any("def f(x, y = x):\n"
                                                "  pass\n");
  expect(leftward.ok(), "leftward default reference accepted");
  expect_no_diagnostic_code(leftward, "E1007");
}

void test_auto_assign_default_warning() {
  amber::binder::BindResult warning =
      bind_any("class Timer:\n"
               "  def update(@timeout = @timeout):\n"
               "    pass\n");
  expect(warning.ok(), "auto-assign field read is warning-only");
  expect_diagnostic_code(warning, "W1001");
}

void test_clause_def_bindings() {
  amber::binder::BindResult result = bind_ok("def area(shape):\n"
                                             "  when Point(x, y):\n"
                                             "    x * y\n"
                                             "  when Rect(w:, h:):\n"
                                             "    w * h\n"
                                             "  else:\n"
                                             "    shape\n"
                                             "\n"
                                             "def fact(0): 1\n"
                                             "def fact(n) if n > 0: n\n");
  const amber::binder::BindGraph &graph = result.graph;

  const amber::binder::Scope *area_scope =
      scope_by_kind_owner(graph, "function", "area");
  expect(area_scope != nullptr, "area function scope exists");
  const amber::binder::Binding *shape_param =
      binding_in_scope(graph, *area_scope, "shape");
  expect(shape_param != nullptr, "area param binding exists");

  const amber::binder::Scope *area_first_clause =
      scope_by_kind_owner(graph, "block", "area.when.0");
  expect(area_first_clause != nullptr, "area first clause scope exists");
  const amber::binder::Binding *x_binding =
      binding_in_scope(graph, *area_first_clause, "x");
  const amber::binder::Binding *y_binding =
      binding_in_scope(graph, *area_first_clause, "y");
  expect(x_binding != nullptr && y_binding != nullptr,
         "head pattern bindings exist");
  expect(has_resolved_reference(graph, "x", x_binding->id), "x resolves");
  expect(has_resolved_reference(graph, "y", y_binding->id), "y resolves");

  const amber::binder::Scope *area_second_clause =
      scope_by_kind_owner(graph, "block", "area.when.1");
  expect(area_second_clause != nullptr, "area second clause scope exists");
  expect(binding_in_scope(graph, *area_second_clause, "w") != nullptr,
         "keyword head w binding");
  expect(binding_in_scope(graph, *area_second_clause, "h") != nullptr,
         "keyword head h binding");

  const amber::binder::Scope *area_else =
      scope_by_kind_owner(graph, "block", "area.else");
  expect(area_else != nullptr, "area else scope exists");
  expect(has_resolved_reference(graph, "shape", shape_param->id),
         "else body resolves outer param");

  const amber::binder::Signature *fact_signature =
      signature_by_owner(graph, "fact");
  expect(fact_signature != nullptr, "fact signature exists");
  expect(fact_signature->params.size() == 1, "fact synthetic arity");
  expect(fact_signature->params[0].local_name == "__arg0",
         "fact synthetic local");

  const amber::binder::Scope *fact_clause =
      scope_by_kind_owner(graph, "block", "fact.when.1");
  expect(fact_clause != nullptr, "fact clause scope exists");
  const amber::binder::Binding *n_binding =
      binding_in_scope(graph, *fact_clause, "n");
  expect(n_binding != nullptr, "fact pattern binding exists");
  expect(has_resolved_reference(graph, "n", n_binding->id),
         "fact guard/body resolve pattern binding");
}

void test_case_pattern_bindings() {
  amber::binder::BindResult result = bind_ok("def choose(x):\n"
                                             "  case! x:\n"
                                             "    when n if n > 0:\n"
                                             "      n\n"
                                             "    else:\n"
                                             "      x\n");
  const amber::binder::BindGraph &graph = result.graph;
  const amber::binder::Scope *arm_scope =
      scope_by_kind_owner(graph, "block", "case.when.0");
  expect(arm_scope != nullptr, "case arm scope exists");
  const amber::binder::Binding *n_binding =
      binding_in_scope(graph, *arm_scope, "n");
  expect(n_binding != nullptr && n_binding->role == "pattern",
         "case arm pattern binding");
  expect(has_resolved_reference(graph, "n", n_binding->id),
         "case guard/body resolve pattern binding");
}

void test_block_param_pattern_bindings() {
  amber::binder::BindResult result =
      bind_ok("def transform(xs, factor):\n"
              "  xs.map |Point(^factor, y), [head, *tail]|: y\n");
  const amber::binder::BindGraph &graph = result.graph;

  const amber::binder::Scope *function_scope =
      scope_by_kind_owner(graph, "function", "transform");
  expect(function_scope != nullptr, "transform function scope exists");
  const amber::binder::Binding *factor_binding =
      binding_in_scope(graph, *function_scope, "factor");
  expect(factor_binding != nullptr, "outer factor binding exists");

  const amber::binder::Scope *block_scope =
      scope_by_kind_owner(graph, "block", "block_suffix");
  expect(block_scope != nullptr, "pattern block scope exists");
  expect(binding_in_scope(graph, *block_scope, "y") != nullptr,
         "head-pattern block binding");
  expect(binding_in_scope(graph, *block_scope, "head") != nullptr,
         "list-pattern head binding");
  expect(binding_in_scope(graph, *block_scope, "tail") != nullptr,
         "list-pattern rest binding");
  expect(has_resolved_reference(graph, "factor", factor_binding->id),
         "pin pattern resolves outer binding");
}

void test_pattern_assignment_bindings() {
  amber::binder::BindResult result = bind_ok("def unpack(values):\n"
                                             "  [head, *tail] = values\n"
                                             "  head\n");
  const amber::binder::BindGraph &graph = result.graph;
  const amber::binder::Scope *function_scope =
      scope_by_kind_owner(graph, "function", "unpack");
  expect(function_scope != nullptr, "unpack function scope exists");
  expect(binding_in_scope(graph, *function_scope, "head") != nullptr,
         "pattern assignment head binding");
  expect(binding_in_scope(graph, *function_scope, "tail") != nullptr,
         "pattern assignment tail binding");
  const amber::binder::Binding *values_binding =
      binding_in_scope(graph, *function_scope, "values");
  expect(values_binding != nullptr, "values param exists");
  expect(has_resolved_reference(graph, "values", values_binding->id),
         "pattern assignment rhs resolves param");
}

void test_duplicate_pattern_binding_diagnostic() {
  amber::binder::BindResult result = bind_any("def area(shape):\n"
                                              "  when Point(x, x):\n"
                                              "    x\n");
  expect(!result.ok(), "duplicate pattern bindings rejected");
  expect_diagnostic_code(result, "E1001");
}

void test_or_pattern_binding_set_diagnostic() {
  amber::binder::BindResult result = bind_any("def choose(x):\n"
                                              "  when (n | 0):\n"
                                              "    x\n");
  expect(!result.ok(), "or-pattern binding set mismatch rejected");
  expect_diagnostic_code(result, "E1002");
}

void test_map_rest_position_diagnostic() {
  amber::binder::BindResult result = bind_any("def choose(x):\n"
                                              "  when {a:, **rest, b:}:\n"
                                              "    x\n");
  expect(!result.ok(), "map rest outside tail position rejected");
  expect_diagnostic_code(result, "E1003");
}

void test_dynamic_pattern_same_pattern_reference_diagnostic() {
  amber::binder::BindResult result =
      bind_any("def classify(shape):\n"
               "  case shape:\n"
               "    when pattern(route(id)) with {id:, **null}:\n"
               "      id\n"
               "    else:\n"
               "      shape\n");
  expect(!result.ok(), "dynamic pattern same-pattern reference rejected");
  expect_diagnostic_code(result, "E1011");
}

void test_dynamic_pattern_outer_scope_reference_ok() {
  amber::binder::BindResult result =
      bind_ok("def classify(shape):\n"
              "  case shape:\n"
              "    when pattern(route(shape)) with {id:, **null}:\n"
              "      id\n"
              "    else:\n"
              "      shape\n");
  expect_no_diagnostic_code(result, "E1011");
}

void test_invalid_pattern_context_diagnostics() {
  amber::binder::BindResult matcher_assign = bind_any("def unpack(values):\n"
                                                      "  x + 1 = values\n");
  expect(!matcher_assign.ok(), "bare matcher pattern assignment rejected");
  expect_diagnostic_code(matcher_assign, "E1008");

  amber::binder::BindResult dynamic_block =
      bind_any("def scan(xs, route):\n"
               "  xs.map |pattern(route(xs))|: xs\n");
  expect(!dynamic_block.ok(), "dynamic block param pattern rejected");
  expect_diagnostic_code(dynamic_block, "E1009");

  amber::binder::BindResult dynamic_assign =
      bind_any("def scan(xs, route):\n"
               "  pattern(route(xs)) = xs\n");
  expect(!dynamic_assign.ok(), "dynamic pattern assignment rejected");
  expect_diagnostic_code(dynamic_assign, "E1009");
}

void test_bind_call_shape_success() {
  amber::binder::BindResult bind_result =
      bind_ok("def configure(x, y = 1, α:, β: 2):\n"
              "  pass\n");
  const amber::binder::Signature *signature =
      signature_by_owner(bind_result.graph, "configure");
  expect(signature != nullptr, "configure signature exists");

  amber::binder::CallBindResult call = amber::binder::bind_call_shape(
      *signature, {positional_arg(0), keyword_arg("α", 2)});
  expect(call.ok(), "call shape bind accepts valid arguments");
  expect(call.slots.size() == 4, "call shape slot count");
  expect(call.slots[0].local_name == "x" &&
             call.slots[0].source_kind == "positional" &&
             call.slots[0].argument_index == 0,
         "positional param bound");
  expect(call.slots[1].local_name == "y" &&
             call.slots[1].source_kind == "missing",
         "defaulted positional slot stays missing");
  expect(call.slots[2].local_name == "α" &&
             call.slots[2].source_kind == "keyword" &&
             call.slots[2].keyword_name == "α",
         "unicode keyword param bound");
  expect(call.slots[3].local_name == "β" &&
             call.slots[3].source_kind == "missing",
         "optional keyword slot stays missing");
  expect(call.default_order.size() == 2 && call.default_order[0] == 1 &&
             call.default_order[1] == 3,
         "default evaluation order follows signature order");
}

void test_extract_call_shape_from_ast() {
  std::unique_ptr<amber::ast::Expr> paren_call =
      parse_expr_ok("configure(1, α: 2)\n");
  amber::binder::CallSiteShape paren_shape =
      amber::binder::extract_call_shape(*paren_call);
  expect(paren_shape.found, "paren call shape extracted");
  expect(paren_shape.call_kind == "call", "ordinary call kind");
  expect(paren_shape.call_style == "paren", "paren call style");
  expect(paren_shape.args.size() == 2, "paren arg count");
  expect(paren_shape.args[0].keyword_name.empty(), "first arg positional");
  expect(paren_shape.args[1].keyword_name == "α", "unicode keyword extracted");

  std::unique_ptr<amber::ast::Expr> bare_call =
      parse_expr_ok("configure 1, α: 2\n");
  amber::binder::CallSiteShape bare_shape =
      amber::binder::extract_call_shape(*bare_call);
  expect(bare_shape.found, "bare call shape extracted");
  expect(bare_shape.call_style == "bare", "bare call style");
  expect(bare_shape.args.size() == 2, "bare arg count");
  expect(bare_shape.args[1].keyword_name == "α",
         "bare unicode keyword extracted");
}

void test_bind_call_shape_from_ast_args() {
  amber::binder::BindResult bind_result =
      bind_ok("def configure(x, α:, β: 2):\n"
              "  pass\n");
  const amber::binder::Signature *signature =
      signature_by_owner(bind_result.graph, "configure");
  expect(signature != nullptr, "configure signature for ast-arg bind exists");

  std::unique_ptr<amber::ast::Expr> call_expr =
      parse_expr_ok("configure 1, α: 2\n");
  amber::binder::CallSiteShape call_shape =
      amber::binder::extract_call_shape(*call_expr);
  expect(call_shape.found, "call-site shape for ast bind extracted");

  amber::binder::CallBindResult call =
      amber::binder::bind_call_shape(*signature, call_shape.args);
  expect(call.ok(), "bind_call_shape accepts extracted ast args");
  expect(call.slots.size() == 3, "ast bind slot count");
  expect(call.slots[0].source_kind == "positional", "ast positional bind");
  expect(call.slots[1].source_kind == "keyword", "ast keyword bind");
  expect(call.slots[2].source_kind == "missing", "ast defaulted bind");
  expect(call.default_order.size() == 1 && call.default_order[0] == 2,
         "ast bind default order");
}

void test_bind_call_auto_assign_plan() {
  amber::binder::BindResult bind_result =
      bind_ok("def init(@масса, α = 1, @@ρ: 2):\n"
              "  pass\n");
  const amber::binder::Signature *signature =
      signature_by_owner(bind_result.graph, "init");
  expect(signature != nullptr, "init signature exists");

  amber::binder::CallBindResult call =
      amber::binder::bind_call_shape(*signature, {positional_arg(0)});
  expect(call.ok(), "auto-assign call plan accepts valid arguments");
  expect(call.pending_auto_assigns.size() == 2, "pending auto-assign count");
  expect(call.pending_auto_assigns[0].slot_index == 0 &&
             call.pending_auto_assigns[0].target_name == "@масса" &&
             call.pending_auto_assigns[0].target_kind == "ivar",
         "ivar auto-assign plan");
  expect(call.pending_auto_assigns[1].slot_index == 2 &&
             call.pending_auto_assigns[1].target_name == "@@ρ" &&
             call.pending_auto_assigns[1].target_kind == "cvar",
         "cvar auto-assign plan");
  expect(call.default_order.size() == 2 && call.default_order[0] == 1 &&
             call.default_order[1] == 2,
         "default order includes missing defaulted params before commit");
}

void test_bind_call_shape_diagnostics() {
  amber::binder::BindResult positional_bind = bind_ok("def add(x):\n"
                                                      "  pass\n");
  const amber::binder::Signature *positional_signature =
      signature_by_owner(positional_bind.graph, "add");
  expect(positional_signature != nullptr, "add signature exists");

  amber::binder::CallBindResult too_many = amber::binder::bind_call_shape(
      *positional_signature, {positional_arg(0), positional_arg(2)});
  expect(!too_many.ok(), "too many positional args rejected");
  expect_call_diagnostic_code(too_many, "E2010");

  amber::binder::BindResult keyword_bind = bind_ok("def route(x, α:, β: 2):\n"
                                                   "  pass\n");
  const amber::binder::Signature *keyword_signature =
      signature_by_owner(keyword_bind.graph, "route");
  expect(keyword_signature != nullptr, "route signature exists");

  amber::binder::CallBindResult duplicate = amber::binder::bind_call_shape(
      *keyword_signature,
      {positional_arg(0), keyword_arg("α", 2), keyword_arg("α", 4)});
  expect(!duplicate.ok(), "duplicate keyword args rejected");
  expect_call_diagnostic_code(duplicate, "E2008");

  amber::binder::CallBindResult unknown = amber::binder::bind_call_shape(
      *keyword_signature, {positional_arg(0), keyword_arg("γ", 6)});
  expect(!unknown.ok(), "unknown keyword args rejected");
  expect_call_diagnostic_code(unknown, "E2009");
  expect_call_diagnostic_code(unknown, "E2011");
}

void test_property_bindings_and_conflicts() {
  amber::binder::BindResult bound = bind_ok("prop answer: 42\n"
                                            "answer\n");
  const amber::binder::Scope *module =
      scope_by_kind_owner(bound.graph, "module", "");
  expect(module != nullptr, "module scope exists for property");
  const amber::binder::Binding *answer =
      binding_in_scope(bound.graph, *module, "answer");
  expect(answer != nullptr && answer->role == "property" &&
             answer->read_only,
         "property binding metadata");
  const amber::binder::Signature *signature =
      signature_by_owner(bound.graph, "answer");
  expect(signature != nullptr && signature->params.empty(),
         "property getter zero-arg signature");
  expect(has_resolved_reference(bound.graph, "answer", answer->id),
         "property read resolves to property binding");

  amber::binder::BindResult method_conflict =
      bind_any("class User:\n"
               "  prop name: @name\n"
               "  def name(): @name\n");
  expect_diagnostic_code(method_conflict, "E_MEMBER_NAME_CONFLICT");

  amber::binder::BindResult storage_separation =
      bind_any("class User:\n"
               "  attr name\n"
               "  def init(@name): pass\n");
  if (!storage_separation.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(
        storage_separation.diagnostics);
    std::exit(1);
  }

  amber::binder::BindResult missing_setter = bind_any("prop answer: 42\n"
                                                      "answer = 7\n");
  expect_diagnostic_code(missing_setter, "AMB_PROP_MISSING_SETTER");

  amber::binder::BindResult top_level_setter =
      bind_any("prop answer:\n"
               "  get: 42\n"
               "  set(value): pass\n");
  expect_diagnostic_code(top_level_setter, "AMB_PROP_TOP_LEVEL_SETTER");

  amber::binder::BindResult setter_property =
      bind_ok("class Box:\n"
              "  prop value:\n"
              "    get: @value\n"
              "    set(value): @value = value\n");
  const amber::binder::Scope *box_scope =
      scope_by_kind_owner(setter_property.graph, "class", "Box");
  expect(box_scope != nullptr, "box scope exists for property setter");
  const amber::binder::Binding *value =
      binding_in_scope(setter_property.graph, *box_scope, "value");
  expect(value != nullptr && value->property_has_getter &&
             value->property_has_setter && !value->read_only,
         "read-write property binding metadata");
  const amber::binder::Scope *setter_scope =
      scope_by_kind_owner(setter_property.graph, "property_setter", "value=");
  expect(setter_scope != nullptr, "property setter scope exists");
  const amber::binder::Signature *setter_signature =
      signature_by_owner(setter_property.graph, "value=");
  expect(setter_signature != nullptr && setter_signature->params.size() == 1 &&
             setter_signature->params[0].local_name == "value",
         "property setter one-arg signature");

  amber::binder::BindResult attr_conflict = bind_any("class User:\n"
                                                     "  attr email\n"
                                                     "  prop email: @email\n");
  expect_diagnostic_code(attr_conflict, "E_MEMBER_NAME_CONFLICT");

  amber::binder::BindResult attr_attr_conflict =
      bind_any("class User:\n"
               "  attr email\n"
               "  attr var email\n");
  expect_diagnostic_code(attr_attr_conflict, "E_MEMBER_NAME_CONFLICT");
}

} // namespace

int main() {
  test_module_class_and_unicode_bindings();
  test_top_level_assignment_predeclaration();
  test_implicit_block_placeholders();
  test_assignment_to_import_alias_is_error();
  test_placeholder_diagnostics();
  test_duplicate_binding_diagnostics();
  test_wildcard_name_diagnostics();
  test_unresolved_name_diagnostics();
  test_default_ordering_diagnostics();
  test_auto_assign_default_warning();
  test_clause_def_bindings();
  test_case_pattern_bindings();
  test_block_param_pattern_bindings();
  test_pattern_assignment_bindings();
  test_duplicate_pattern_binding_diagnostic();
  test_or_pattern_binding_set_diagnostic();
  test_map_rest_position_diagnostic();
  test_dynamic_pattern_same_pattern_reference_diagnostic();
  test_dynamic_pattern_outer_scope_reference_ok();
  test_invalid_pattern_context_diagnostics();
  test_bind_call_shape_success();
  test_extract_call_shape_from_ast();
  test_bind_call_shape_from_ast_args();
  test_bind_call_auto_assign_plan();
  test_bind_call_shape_diagnostics();
  test_property_bindings_and_conflicts();
  std::cout << "binder_tests: ok\n";
  return 0;
}
