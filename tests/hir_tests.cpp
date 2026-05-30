#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

amber::hir::Program lower_ok(const std::string &source,
                             std::string module_name = "") {
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

  module_name = parse_result.module_name;
  return amber::hir::lower_module(parse_result.items, parse_result.module_name,
                                  bind_result.graph);
}

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "hir test failed: " << message << "\n";
    std::exit(1);
  }
}

const amber::hir::Procedure *
procedure_by_name(const amber::hir::Program &program, const std::string &name) {
  for (const amber::hir::Procedure &procedure : program.procedures) {
    if (procedure.name == name) {
      return &procedure;
    }
  }
  return nullptr;
}

const amber::hir::Procedure *procedure_by_id(const amber::hir::Program &program,
                                             const std::string &id) {
  for (const amber::hir::Procedure &procedure : program.procedures) {
    if (procedure.id == id) {
      return &procedure;
    }
  }
  return nullptr;
}

const amber::ast::Expr *list_item(const amber::ast::Expr &expr,
                                  const std::string &field_name,
                                  std::size_t index) {
  for (const amber::ast::ListField &field : expr.list_fields) {
    if (field.name == field_name) {
      return index < field.values.size() ? field.values[index].get() : nullptr;
    }
  }
  return nullptr;
}

const amber::ast::Expr *node_field(const amber::ast::Expr &expr,
                                   const std::string &field_name) {
  for (const amber::ast::NodeField &field : expr.node_fields) {
    if (field.name == field_name) {
      return field.value.get();
    }
  }
  return nullptr;
}

const amber::ast::Expr *module_item_by_name(const amber::hir::Program &program,
                                            const std::string &name) {
  if (program.root == nullptr) {
    return nullptr;
  }
  for (const amber::ast::ListField &field : program.root->list_fields) {
    if (field.name != "items") {
      continue;
    }
    for (const std::unique_ptr<amber::ast::Expr> &item : field.values) {
      if (item == nullptr) {
        continue;
      }
      for (const amber::ast::StringField &string_field : item->string_fields) {
        if (string_field.name == "name" && string_field.value == name) {
          return item.get();
        }
      }
    }
  }
  return nullptr;
}

std::string string_field(const amber::ast::Expr &expr,
                         const std::string &name) {
  for (const amber::ast::StringField &field : expr.string_fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  return "";
}

bool bool_field(const amber::ast::Expr &expr, const std::string &name) {
  for (const amber::ast::BoolField &field : expr.bool_fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  return false;
}

bool contains_kind(const amber::ast::Expr &expr, const std::string &kind) {
  if (expr.kind == kind) {
    return true;
  }
  for (const amber::ast::NodeField &field : expr.node_fields) {
    if (field.value != nullptr && contains_kind(*field.value, kind)) {
      return true;
    }
  }
  for (const amber::ast::ListField &field : expr.list_fields) {
    for (const std::unique_ptr<amber::ast::Expr> &value : field.values) {
      if (value != nullptr && contains_kind(*value, kind)) {
        return true;
      }
    }
  }
  return false;
}

void test_method_and_send_lowering() {
  const amber::hir::Program program =
      lower_ok("def normalize(numbers):\n"
               "  numbers.map: _1.email.downcase() .uniq()\n");
  expect(program.root != nullptr && program.root->kind == "HModule",
         "module root exists");
  const amber::hir::Procedure *normalize =
      procedure_by_name(program, "normalize");
  expect(normalize != nullptr, "normalize procedure exists");
  const amber::ast::Expr *stmt = list_item(*normalize->body, "items", 0);
  expect(stmt != nullptr && stmt->kind == "HLastSet",
         "statement wrapped in HLastSet");
  const amber::ast::Expr *expr = nullptr;
  for (const amber::ast::NodeField &field : stmt->node_fields) {
    if (field.name == "expr") {
      expr = field.value.get();
    }
  }
  expect(expr != nullptr && expr->kind == "HSend",
         "outer chain lowers to HSend");
  expect(string_field(*expr, "selector") == "uniq", "outer selector");
}

void test_module_def_materializes_callable_binding() {
  const amber::hir::Program program = lower_ok("def f(x):\n"
                                               "  x + 42\n"
                                               "\n"
                                               "f(3)\n");
  const amber::hir::Procedure *init =
      procedure_by_name(program, "__module_init__");
  expect(init != nullptr && init->body != nullptr, "module init exists");

  const amber::ast::Expr *method = module_item_by_name(program, "f");
  expect(method != nullptr && method->kind == "HMethod", "method item exists");
  const std::string method_procedure = string_field(*method, "procedure");

  const amber::ast::Expr *store = list_item(*init->body, "items", 0);
  expect(store != nullptr && store->kind == "HStoreLocal",
         "module def initializes function binding");
  const amber::ast::Expr *closure = node_field(*store, "expr");
  expect(closure != nullptr && closure->kind == "HClosure",
         "module def stores callable closure");
  expect(string_field(*closure, "procedure") == method_procedure,
         "module def closure points at method body");

  const amber::ast::Expr *call_stmt = list_item(*init->body, "items", 2);
  expect(call_stmt != nullptr && call_stmt->kind == "HLastSet",
         "module call remains after def initialization");
  const amber::ast::Expr *call_expr = node_field(*call_stmt, "expr");
  expect(call_expr != nullptr && call_expr->kind == "HCall",
         "top-level call stays callable invocation");
  const amber::ast::Expr *callable = node_field(*call_expr, "callable");
  expect(callable != nullptr && callable->kind == "HLoadLocal",
         "top-level call reads initialized function binding");
  expect(string_field(*callable, "slot") == string_field(*store, "slot"),
         "top-level call uses same function slot");
}

void test_module_def_closure_captures_prior_function() {
  const amber::hir::Program program = lower_ok("def tap(x):\n"
                                               "  x\n"
                                               "\n"
                                               "def describe(a):\n"
                                               "  tap(a)\n"
                                               "\n"
                                               "describe(7)\n");
  const amber::hir::Procedure *init =
      procedure_by_name(program, "__module_init__");
  expect(init != nullptr && init->body != nullptr, "module init exists");

  const amber::ast::Expr *describe_store = list_item(*init->body, "items", 2);
  expect(describe_store != nullptr && describe_store->kind == "HStoreLocal",
         "second module def initializes function binding");
  const amber::ast::Expr *describe_closure =
      node_field(*describe_store, "expr");
  expect(describe_closure != nullptr && describe_closure->kind == "HClosure",
         "second module def stores callable closure");
  const amber::ast::Expr *capture =
      list_item(*describe_closure, "captures", 0);
  expect(capture != nullptr && capture->kind == "HCapture",
         "second module closure captures first function");
  expect(string_field(*capture, "source_slot") == "l0",
         "second module closure captures tap slot");

  const amber::hir::Procedure *describe =
      procedure_by_id(program, string_field(*describe_closure, "procedure"));
  expect(describe != nullptr, "describe procedure exists");
  const amber::ast::Expr *stmt = list_item(*describe->body, "items", 0);
  const amber::ast::Expr *call = stmt == nullptr ? nullptr : node_field(*stmt, "expr");
  expect(call != nullptr && call->kind == "HCall", "describe body calls tap");
  const amber::ast::Expr *callable = node_field(*call, "callable");
  expect(callable != nullptr && callable->kind == "HLoadCapture",
         "describe body reads captured tap");
}

void test_module_def_closure_captures_self() {
  const amber::hir::Program program = lower_ok("def f(x):\n"
                                               "  f(x)\n"
                                               "\n"
                                               "f(1)\n");
  const amber::hir::Procedure *init =
      procedure_by_name(program, "__module_init__");
  expect(init != nullptr && init->body != nullptr, "module init exists");

  const amber::ast::Expr *store = list_item(*init->body, "items", 0);
  const amber::ast::Expr *closure =
      store == nullptr ? nullptr : node_field(*store, "expr");
  expect(closure != nullptr && closure->kind == "HClosure",
         "recursive def stores closure");
  const amber::ast::Expr *capture = list_item(*closure, "captures", 0);
  expect(capture != nullptr && capture->kind == "HCapture",
         "recursive def captures itself");
  expect(string_field(*capture, "source_slot") == string_field(*store, "slot"),
         "self capture reads the function slot");

  const amber::hir::Procedure *function =
      procedure_by_id(program, string_field(*closure, "procedure"));
  expect(function != nullptr && function->captures.size() == 1,
         "recursive procedure has capture metadata");
  expect(contains_kind(*function->body, "HLoadCapture"),
         "recursive body reads self capture");
}

void test_module_clause_def_materializes_callable_binding() {
  const amber::hir::Program program =
      lower_ok("def fact(0): 1\n"
               "def fact(n) if n > 0: n * fact(n - 1)\n"
               "\n"
               "fact(3)\n");
  const amber::hir::Procedure *init =
      procedure_by_name(program, "__module_init__");
  expect(init != nullptr && init->body != nullptr, "module init exists");

  const amber::ast::Expr *store = list_item(*init->body, "items", 0);
  expect(store != nullptr && store->kind == "HStoreLocal",
         "module clause def initializes function binding");
  const amber::ast::Expr *closure = node_field(*store, "expr");
  expect(closure != nullptr && closure->kind == "HClosure",
         "module clause def stores callable closure");
  const amber::ast::Expr *capture = list_item(*closure, "captures", 0);
  expect(capture != nullptr && string_field(*capture, "source_kind") == "local",
         "module clause closure captures itself from local slot");
  expect(string_field(*capture, "source_slot") == string_field(*store, "slot"),
         "module clause self capture uses function slot");

  const amber::ast::Expr *call_stmt = list_item(*init->body, "items", 2);
  expect(call_stmt != nullptr && call_stmt->kind == "HLastSet",
         "module clause call remains after initialization");
  const amber::hir::Procedure *function =
      procedure_by_id(program, string_field(*closure, "procedure"));
  expect(function != nullptr && function->captures.size() == 1,
         "module clause procedure has capture metadata");
  const amber::ast::Expr *method = module_item_by_name(program, "fact");
  expect(method != nullptr && contains_kind(*method, "HLoadCapture"),
         "module clause body reads recursive capture");
}

void test_safe_navigation_lowering() {
  const amber::hir::Program program = lower_ok("def city(user):\n"
                                               "  user.?.address.?.city\n");
  const amber::hir::Procedure *city = procedure_by_name(program, "city");
  expect(city != nullptr, "city procedure exists");
  expect(city->locals.size() == 3, "safe-nav creates temp locals");
  expect(city->locals[1].role == "temp", "first safe-nav temp role");
  expect(city->locals[2].role == "temp", "second safe-nav temp role");
  const amber::ast::Expr *stmt = list_item(*city->body, "items", 0);
  expect(stmt != nullptr && stmt->kind == "HLastSet", "safe-nav stmt wrapped");
  const amber::ast::Expr *expr = node_field(*stmt, "expr");
  expect(expr != nullptr && expr->kind == "HIf",
         "safe-nav lowers to explicit HIf");
  expect(contains_kind(*expr, "HIsNull"), "safe-nav uses HIsNull guard");
  expect(!contains_kind(*expr, "HSafeSend"),
         "HSafeSend removed from final HIR");

  const amber::ast::Expr *cond = node_field(*expr, "cond");
  expect(cond != nullptr && cond->kind == "HIsNull", "outer safe cond");
  const amber::ast::Expr *stored = node_field(*cond, "expr");
  expect(stored != nullptr && stored->kind == "HStoreLocal",
         "outer guard stores receiver once");
  expect(string_field(*stored, "slot") == "l2", "outer temp slot");

  const amber::ast::Expr *inner = node_field(*stored, "expr");
  expect(inner != nullptr && inner->kind == "HIf",
         "nested safe stage preserved");
}

void test_safe_call_and_index_lowering() {
  const amber::hir::Program program = lower_ok("def probe(fn, xs):\n"
                                               "  fn.?.(1)\n"
                                               "  xs.?.[0]\n");
  const amber::hir::Procedure *probe = procedure_by_name(program, "probe");
  expect(probe != nullptr, "probe procedure exists");
  expect(probe->locals.size() == 4, "safe call/index create temp locals");

  const amber::ast::Expr *call_stmt = list_item(*probe->body, "items", 0);
  expect(call_stmt != nullptr, "safe call stmt exists");
  const amber::ast::Expr *call_expr = node_field(*call_stmt, "expr");
  expect(call_expr != nullptr && call_expr->kind == "HIf",
         "safe call lowers to HIf");
  expect(contains_kind(*call_expr, "HCall"), "safe call keeps ordinary HCall");
  expect(!contains_kind(*call_expr, "HSafeCall"),
         "HSafeCall removed from final HIR");

  const amber::ast::Expr *index_stmt = list_item(*probe->body, "items", 1);
  expect(index_stmt != nullptr, "safe index stmt exists");
  const amber::ast::Expr *index_expr = node_field(*index_stmt, "expr");
  expect(index_expr != nullptr && index_expr->kind == "HIf",
         "safe index lowers to HIf");
  expect(contains_kind(*index_expr, "HIndex"),
         "safe index keeps ordinary HIndex");
  expect(!contains_kind(*index_expr, "HSafeIndex"),
         "HSafeIndex removed from final HIR");
}

void test_builtin_send_lowering() {
  const amber::hir::Program program =
      lower_ok("def invoke(recv, selector, value):\n"
               "  send(recv, \"touch\")\n"
               "  send(recv, selector, value, urgent: true)\n");
  const amber::hir::Procedure *invoke = procedure_by_name(program, "invoke");
  expect(invoke != nullptr, "invoke procedure exists");

  const amber::ast::Expr *static_stmt = list_item(*invoke->body, "items", 0);
  expect(static_stmt != nullptr, "static send stmt exists");
  const amber::ast::Expr *static_expr = node_field(*static_stmt, "expr");
  expect(static_expr != nullptr && static_expr->kind == "HSend",
         "static builtin send lowers to HSend");
  expect(string_field(*static_expr, "selector") == "touch",
         "static send selector unquoted");
  expect(!contains_kind(*static_expr, "HCall"),
         "static builtin send is not HCall");

  const amber::ast::Expr *dynamic_stmt = list_item(*invoke->body, "items", 1);
  expect(dynamic_stmt != nullptr, "dynamic send stmt exists");
  const amber::ast::Expr *dynamic_expr = node_field(*dynamic_stmt, "expr");
  expect(dynamic_expr != nullptr && dynamic_expr->kind == "HSendDyn",
         "dynamic builtin send lowers to HSendDyn");
  const amber::ast::Expr *selector_expr =
      node_field(*dynamic_expr, "selector_expr");
  expect(selector_expr != nullptr && selector_expr->kind == "HLoadLocal",
         "dynamic selector stays as expression");
  expect(list_item(*dynamic_expr, "pos_args", 0) != nullptr,
         "dynamic send forwards extra positional args");
  const amber::ast::Expr *kw = list_item(*dynamic_expr, "kw_args", 0);
  expect(kw != nullptr && kw->kind == "HKeywordArg",
         "dynamic send forwards keyword args");
}

void test_collection_literal_lowering() {
  const amber::hir::Program program = lower_ok("[1, 2]\n"
                                               "(3, 4)\n"
                                               "{5}\n"
                                               "{id: :ok}\n");
  const amber::hir::Procedure *init =
      procedure_by_name(program, "__module_init__");
  expect(init != nullptr && init->body != nullptr, "module init exists");

  const amber::ast::Expr *list_stmt = list_item(*init->body, "items", 0);
  const amber::ast::Expr *tuple_stmt = list_item(*init->body, "items", 1);
  const amber::ast::Expr *set_stmt = list_item(*init->body, "items", 2);
  const amber::ast::Expr *map_stmt = list_item(*init->body, "items", 3);
  expect(list_stmt != nullptr && tuple_stmt != nullptr && set_stmt != nullptr &&
             map_stmt != nullptr,
         "collection literal statements exist");

  const amber::ast::Expr *list_expr = node_field(*list_stmt, "expr");
  const amber::ast::Expr *tuple_expr = node_field(*tuple_stmt, "expr");
  const amber::ast::Expr *set_expr = node_field(*set_stmt, "expr");
  const amber::ast::Expr *map_expr = node_field(*map_stmt, "expr");
  expect(list_expr != nullptr && list_expr->kind == "HListLiteral",
         "list literal lowers to HListLiteral");
  expect(tuple_expr != nullptr && tuple_expr->kind == "HTupleLiteral",
         "tuple literal lowers to HTupleLiteral");
  expect(set_expr != nullptr && set_expr->kind == "HSetLiteral",
         "set literal lowers to HSetLiteral");
  expect(map_expr != nullptr && map_expr->kind == "HMapLiteral",
         "map literal lowers to HMapLiteral");
  expect(contains_kind(*map_expr, "HMapEntry"), "map entry is preserved");
  expect(contains_kind(*map_expr, "HConst"), "symbol value lowers to HConst");
}

void test_inline_conditional_and_conditional_literal_lowering() {
  const amber::hir::Program program =
      lower_ok("if true then 1 else 2\n"
               "[1, 2 if true]\n"
               "{id: 1 unless false}\n");
  const amber::hir::Procedure *init =
      procedure_by_name(program, "__module_init__");
  expect(init != nullptr && init->body != nullptr, "module init exists");

  const amber::ast::Expr *inline_stmt = list_item(*init->body, "items", 0);
  const amber::ast::Expr *list_stmt = list_item(*init->body, "items", 1);
  const amber::ast::Expr *map_stmt = list_item(*init->body, "items", 2);
  expect(inline_stmt != nullptr && list_stmt != nullptr && map_stmt != nullptr,
         "conditional lowering statements exist");

  const amber::ast::Expr *inline_expr = node_field(*inline_stmt, "expr");
  expect(inline_expr != nullptr && inline_expr->kind == "HIf",
         "inline conditional lowers to HIf");

  const amber::ast::Expr *list_expr = node_field(*list_stmt, "expr");
  expect(list_expr != nullptr && list_expr->kind == "HListLiteral",
         "conditional list remains HListLiteral");
  const amber::ast::Expr *conditional_element =
      list_item(*list_expr, "elements", 1);
  expect(conditional_element != nullptr &&
             conditional_element->kind == "HConditionalElement",
         "conditional list element lowers");
  expect(string_field(*conditional_element, "condition_kind") == "if",
         "conditional list kind preserved");

  const amber::ast::Expr *map_expr = node_field(*map_stmt, "expr");
  expect(map_expr != nullptr && map_expr->kind == "HMapLiteral",
         "conditional map remains HMapLiteral");
  const amber::ast::Expr *entry = list_item(*map_expr, "entries", 0);
  expect(entry != nullptr && entry->kind == "HMapEntry",
         "conditional map entry lowers");
  expect(string_field(*entry, "condition_kind") == "unless",
         "conditional map kind preserved");
  expect(node_field(*entry, "condition") != nullptr,
         "conditional map condition lowers");
}

void test_shadowed_send_stays_call() {
  const amber::hir::Program program =
      lower_ok("def invoke(send, recv, selector):\n"
               "  send(recv, selector)\n");
  const amber::hir::Procedure *invoke = procedure_by_name(program, "invoke");
  expect(invoke != nullptr, "shadowed invoke procedure exists");

  const amber::ast::Expr *stmt = list_item(*invoke->body, "items", 0);
  expect(stmt != nullptr, "shadowed send stmt exists");
  const amber::ast::Expr *expr = node_field(*stmt, "expr");
  expect(expr != nullptr && expr->kind == "HCall",
         "shadowed send remains HCall");
  expect(!contains_kind(*expr, "HSendDyn"),
         "shadowed send does not lower to HSendDyn");
}

void test_w13_operator_lowering() {
  const amber::hir::Program program = lower_ok("def ops(x, xs, a, b):\n"
                                               "  x in xs\n"
                                               "  a and b\n"
                                               "  a or b\n"
                                               "  1..10\n");
  const amber::hir::Procedure *ops = procedure_by_name(program, "ops");
  expect(ops != nullptr, "ops procedure exists");

  const amber::ast::Expr *in_stmt = list_item(*ops->body, "items", 0);
  const amber::ast::Expr *in_expr =
      in_stmt == nullptr ? nullptr : node_field(*in_stmt, "expr");
  expect(in_expr != nullptr && in_expr->kind == "HSend", "in lowers to send");
  expect(string_field(*in_expr, "selector") == "contains?",
         "in selector is contains?");
  expect(node_field(*in_expr, "receiver") != nullptr &&
             node_field(*in_expr, "receiver")->kind == "HLoadLocal",
         "in receiver is rhs collection");

  const amber::ast::Expr *and_stmt = list_item(*ops->body, "items", 1);
  const amber::ast::Expr *and_expr =
      and_stmt == nullptr ? nullptr : node_field(*and_stmt, "expr");
  expect(and_expr != nullptr && and_expr->kind == "HLogical",
         "and lowers to HLogical");
  expect(string_field(*and_expr, "op") == "and", "and op preserved");

  const amber::ast::Expr *or_stmt = list_item(*ops->body, "items", 2);
  const amber::ast::Expr *or_expr =
      or_stmt == nullptr ? nullptr : node_field(*or_stmt, "expr");
  expect(or_expr != nullptr && or_expr->kind == "HLogical",
         "or lowers to HLogical");
  expect(string_field(*or_expr, "op") == "or", "or op preserved");

  const amber::ast::Expr *range_stmt = list_item(*ops->body, "items", 3);
  const amber::ast::Expr *range_expr =
      range_stmt == nullptr ? nullptr : node_field(*range_stmt, "expr");
  expect(range_expr != nullptr && range_expr->kind == "HSend",
         "range lowers to constructor send");
  expect(string_field(*range_expr, "selector") == "new",
         "range constructor selector");
  const amber::ast::Expr *receiver = node_field(*range_expr, "receiver");
  expect(receiver != nullptr && receiver->kind == "HLoadConst",
         "range receiver is Range constant");
  expect(string_field(*receiver, "path") == "Range", "range constant path");
  const amber::ast::Expr *inclusive = list_item(*range_expr, "kw_args", 0);
  expect(inclusive != nullptr && inclusive->kind == "HKeywordArg",
         "range carries inclusive_end kwarg");
  expect(string_field(*inclusive, "name") == "inclusive_end",
         "range kwarg name");
}

void test_clause_method_lowering() {
  const amber::hir::Program program = lower_ok("def area(shape):\n"
                                               "  when Point(x, y):\n"
                                               "    x * y\n"
                                               "  when Rect(w:, h:):\n"
                                               "    w * h\n"
                                               "  else:\n"
                                               "    shape\n"
                                               "\n"
                                               "def fact(0): 1\n"
                                               "def fact(n) if n > 0: n\n");

  const amber::ast::Expr *area = module_item_by_name(program, "area");
  expect(area != nullptr && area->kind == "HMethod", "area HMethod exists");
  const amber::ast::Expr *area_clause0 = list_item(*area, "clauses", 0);
  const amber::ast::Expr *area_clause1 = list_item(*area, "clauses", 1);
  expect(area_clause0 != nullptr && area_clause0->kind == "HClause",
         "first area clause lowered");
  expect(area_clause1 != nullptr && area_clause1->kind == "HClause",
         "second area clause lowered");
  expect(string_field(*area_clause0, "subject_kind") == "single_positional",
         "single positional clause kind");
  const amber::ast::Expr *area_pattern0 = node_field(*area_clause0, "pattern");
  const amber::ast::Expr *area_pattern1 = node_field(*area_clause1, "pattern");
  const amber::ast::Expr *area_compiled0 =
      node_field(*area_clause0, "compiled_pattern");
  const amber::ast::Expr *area_compiled1 =
      node_field(*area_clause1, "compiled_pattern");
  expect(area_pattern0 != nullptr && area_pattern0->kind == "PatHead",
         "head pattern lowered structurally");
  expect(area_pattern1 != nullptr && area_pattern1->kind == "PatHead",
         "keyword head pattern lowered structurally");
  expect(area_compiled0 != nullptr &&
             area_compiled0->kind == "HCompiledPattern",
         "first compiled pattern exists");
  expect(area_compiled1 != nullptr &&
             area_compiled1->kind == "HCompiledPattern",
         "second compiled pattern exists");
  const amber::ast::Expr *area_compiled0_ir =
      node_field(*area_compiled0, "pattern_ir");
  const amber::ast::Expr *area_compiled1_ir =
      node_field(*area_compiled1, "pattern_ir");
  expect(area_compiled0_ir != nullptr && area_compiled0_ir->kind == "PIrHead",
         "first compiled pattern IR");
  expect(area_compiled1_ir != nullptr && area_compiled1_ir->kind == "PIrHead",
         "second compiled pattern IR");
  expect(string_field(*area_compiled0_ir, "destructure_mode") == "POSITIONAL",
         "first head IR positional mode");
  expect(bool_field(*area_compiled0_ir, "requires_deconstruct"),
         "first head IR requires deconstruct");
  expect(!bool_field(*area_compiled0_ir, "requires_deconstruct_keys"),
         "first head IR does not require deconstruct_keys");
  expect(string_field(*area_compiled1_ir, "destructure_mode") == "KEYS",
         "second head IR keys mode");
  expect(!bool_field(*area_compiled1_ir, "requires_deconstruct"),
         "second head IR does not require deconstruct");
  expect(bool_field(*area_compiled1_ir, "requires_deconstruct_keys"),
         "second head IR requires deconstruct_keys");
  expect(!bool_field(*area_compiled1_ir, "needs_full_map"),
         "second head IR does not require full map");
  expect(list_item(*area_compiled1_ir, "requested_keys", 0) != nullptr &&
             string_field(*list_item(*area_compiled1_ir, "requested_keys", 0),
                          "name") == "w",
         "second head IR first requested key");
  expect(list_item(*area_compiled1_ir, "requested_keys", 1) != nullptr &&
             string_field(*list_item(*area_compiled1_ir, "requested_keys", 1),
                          "name") == "h",
         "second head IR second requested key");
  expect(string_field(*area_pattern0, "head") == "Point", "first head name");
  expect(string_field(*area_pattern1, "head") == "Rect", "second head name");
  expect(list_item(*area_pattern0, "pos_args", 0) != nullptr &&
             list_item(*area_pattern0, "pos_args", 0)->kind == "PatBind" &&
             string_field(*list_item(*area_pattern0, "pos_args", 0), "name") ==
                 "x",
         "first head positional bind");
  expect(list_item(*area_pattern1, "kw_fields", 0) != nullptr &&
             string_field(*list_item(*area_pattern1, "kw_fields", 0), "name") ==
                 "w",
         "first keyword field name");
  expect(node_field(*area, "else_body") != nullptr, "area else body lowered");

  const amber::hir::Procedure *area_proc = procedure_by_name(program, "area");
  expect(area_proc != nullptr, "area procedure exists");
  expect(area_proc->locals.size() >= 5, "area locals include pattern bindings");
  expect(area_proc->locals[0].name == "shape", "area param local");
  expect(area_proc->locals[1].name == "x" &&
             area_proc->locals[1].role == "pattern",
         "area x pattern local");
  expect(area_proc->locals[2].name == "y" &&
             area_proc->locals[2].role == "pattern",
         "area y pattern local");

  const amber::ast::Expr *area_body = node_field(*area_clause0, "body");
  expect(area_body != nullptr, "area clause body exists");
  const amber::ast::Expr *area_stmt = list_item(*area_body, "items", 0);
  expect(area_stmt != nullptr, "area clause stmt exists");
  const amber::ast::Expr *area_expr = node_field(*area_stmt, "expr");
  expect(area_expr != nullptr && area_expr->kind == "HSend",
         "area clause expr lowered");
  const amber::ast::Expr *area_recv = node_field(*area_expr, "receiver");
  const amber::ast::Expr *area_arg = list_item(*area_expr, "pos_args", 0);
  expect(area_recv != nullptr && area_recv->kind == "HLoadLocal",
         "pattern receiver lowers to local");
  expect(area_arg != nullptr && area_arg->kind == "HLoadLocal",
         "pattern arg lowers to local");

  const amber::ast::Expr *fact = module_item_by_name(program, "fact");
  expect(fact != nullptr && fact->kind == "HMethod", "fact HMethod exists");
  expect(list_item(*fact, "clauses", 1) != nullptr, "fact clauses merged");
  const amber::ast::Expr *fact_clause1 = list_item(*fact, "clauses", 1);
  expect(fact_clause1 != nullptr, "fact second clause exists");
  const amber::ast::Expr *fact_guard = node_field(*fact_clause1, "guard");
  expect(fact_guard != nullptr && contains_kind(*fact_guard, "HLoadLocal"),
         "fact guard uses local pattern binding");
}

void test_case_pattern_lowering() {
  const amber::hir::Program program = lower_ok("def choose(x):\n"
                                               "  case! x:\n"
                                               "    when 0:\n"
                                               "      1\n"
                                               "    when n if n > 0:\n"
                                               "      n\n"
                                               "    else:\n"
                                               "      -1\n");
  const amber::hir::Procedure *choose = procedure_by_name(program, "choose");
  expect(choose != nullptr, "choose procedure exists");

  const amber::ast::Expr *stmt = list_item(*choose->body, "items", 0);
  expect(stmt != nullptr, "case stmt exists");
  const amber::ast::Expr *dispatch = node_field(*stmt, "expr");
  expect(dispatch != nullptr && dispatch->kind == "HMatchDispatch",
         "case lowers to HMatchDispatch");

  const amber::ast::Expr *arm0 = list_item(*dispatch, "arms", 0);
  const amber::ast::Expr *arm1 = list_item(*dispatch, "arms", 1);
  expect(arm0 != nullptr && arm1 != nullptr, "case arms exist");
  const amber::ast::Expr *pattern0 = node_field(*arm0, "pattern");
  const amber::ast::Expr *pattern1 = node_field(*arm1, "pattern");
  const amber::ast::Expr *compiled0 = node_field(*arm0, "compiled_pattern");
  const amber::ast::Expr *compiled1 = node_field(*arm1, "compiled_pattern");
  expect(pattern0 != nullptr && pattern0->kind == "PatLiteral",
         "literal case pattern lowered structurally");
  expect(string_field(*pattern0, "value") == "0", "literal pattern value");
  expect(pattern1 != nullptr && pattern1->kind == "PatBind",
         "bind case pattern lowered structurally");
  expect(string_field(*pattern1, "name") == "n", "bind pattern name");
  expect(compiled0 != nullptr && compiled0->kind == "HCompiledPattern",
         "first case compiled pattern exists");
  expect(compiled1 != nullptr && compiled1->kind == "HCompiledPattern",
         "second case compiled pattern exists");
  expect(node_field(*compiled0, "pattern_ir") != nullptr &&
             node_field(*compiled0, "pattern_ir")->kind == "PIrLiteral",
         "literal case compiled pattern IR");
  expect(node_field(*compiled1, "pattern_ir") != nullptr &&
             node_field(*compiled1, "pattern_ir")->kind == "PIrBind",
         "bind case compiled pattern IR");
  expect(
      node_field(*compiled0, "match_program") != nullptr &&
          node_field(*node_field(*compiled0, "match_program"), "root") !=
              nullptr &&
          node_field(*node_field(*compiled0, "match_program"), "root")->kind ==
              "PLiteral",
      "literal case match program");

  const amber::ast::Expr *guard = node_field(*arm1, "guard");
  expect(guard != nullptr && contains_kind(*guard, "HLoadLocal"),
         "case guard uses pattern local");
  const amber::ast::Expr *body = node_field(*arm1, "body");
  expect(body != nullptr, "case body exists");
  const amber::ast::Expr *body_stmt = list_item(*body, "items", 0);
  const amber::ast::Expr *body_expr =
      body_stmt == nullptr ? nullptr : node_field(*body_stmt, "expr");
  expect(body_expr != nullptr && body_expr->kind == "HLoadLocal",
         "case body uses pattern local");
}

void test_case_matcher_expr_lowering() {
  const amber::hir::Program program = lower_ok("def choose(x, limit):\n"
                                               "  case x:\n"
                                               "    when limit + 1:\n"
                                               "      x\n"
                                               "    else:\n"
                                               "      0\n");
  const amber::hir::Procedure *choose = procedure_by_name(program, "choose");
  expect(choose != nullptr, "matcher choose procedure exists");
  const amber::ast::Expr *stmt = list_item(*choose->body, "items", 0);
  expect(stmt != nullptr, "matcher case stmt exists");
  const amber::ast::Expr *dispatch = node_field(*stmt, "expr");
  expect(dispatch != nullptr && dispatch->kind == "HMatchDispatch",
         "matcher case lowers to HMatchDispatch");
  const amber::ast::Expr *arm0 = list_item(*dispatch, "arms", 0);
  expect(arm0 != nullptr, "matcher arm exists");
  const amber::ast::Expr *pattern0 = node_field(*arm0, "pattern");
  const amber::ast::Expr *compiled0 = node_field(*arm0, "compiled_pattern");
  expect(pattern0 != nullptr && pattern0->kind == "PatMatcherExpr",
         "matcher fallback preserved structurally");
  expect(string_field(*pattern0, "expr_text") == "limit + 1",
         "matcher expr text preserved");
  expect(compiled0 != nullptr && compiled0->kind == "HCompiledPattern",
         "matcher compiled pattern exists");
  expect(node_field(*compiled0, "pattern_ir") != nullptr &&
             node_field(*compiled0, "pattern_ir")->kind == "PIrMatcherExpr",
         "matcher fallback compiles to matcher IR");
  expect(
      node_field(*compiled0, "match_program") != nullptr &&
          node_field(*node_field(*compiled0, "match_program"), "root") !=
              nullptr &&
          node_field(*node_field(*compiled0, "match_program"), "root")->kind ==
              "PMatcherExpr",
      "matcher fallback compiles to matcher program");
  const amber::ast::Expr *matcher_ir = node_field(*compiled0, "pattern_ir");
  const amber::ast::Expr *matcher_prog =
      node_field(*node_field(*compiled0, "match_program"), "root");
  expect(matcher_ir != nullptr &&
             node_field(*matcher_ir, "matcher_expr") != nullptr &&
             contains_kind(*node_field(*matcher_ir, "matcher_expr"), "HSend"),
         "matcher IR carries lowered matcher expr");
  expect(matcher_prog != nullptr &&
             node_field(*matcher_prog, "matcher_expr") != nullptr &&
             contains_kind(*node_field(*matcher_prog, "matcher_expr"), "HSend"),
         "matcher program carries lowered matcher expr");
}

void test_dynamic_pattern_compilation() {
  const amber::hir::Program program = lower_ok("def choose(x):\n"
                                               "  case x:\n"
                                               "    when String:\n"
                                               "      x\n");
  const amber::hir::Procedure *choose = procedure_by_name(program, "choose");
  expect(choose != nullptr, "dynamic choose procedure exists");
  const amber::ast::Expr *stmt = list_item(*choose->body, "items", 0);
  expect(stmt != nullptr, "dynamic case stmt exists");
  const amber::ast::Expr *dispatch = node_field(*stmt, "expr");
  expect(dispatch != nullptr && dispatch->kind == "HMatchDispatch",
         "dynamic case lowers to HMatchDispatch");
  const amber::ast::Expr *arm0 = list_item(*dispatch, "arms", 0);
  expect(arm0 != nullptr, "dynamic arm exists");
  const amber::ast::Expr *pattern0 = node_field(*arm0, "pattern");
  const amber::ast::Expr *compiled0 = node_field(*arm0, "compiled_pattern");
  expect(pattern0 != nullptr && pattern0->kind == "PatConst",
         "constant pattern preserved at pattern layer");
  expect(compiled0 != nullptr && compiled0->kind == "HCompiledPattern",
         "constant compiled pattern exists");
  expect(node_field(*compiled0, "pattern_ir") != nullptr &&
             node_field(*compiled0, "pattern_ir")->kind == "PIrConst",
         "constant pattern compiles to const IR");
  expect(string_field(*pattern0, "path") == "String",
         "constant pattern path preserved");
  expect(string_field(*node_field(*compiled0, "pattern_ir"), "path") ==
             "String",
         "constant IR path preserved");
}

void test_advanced_pattern_lowering() {
  const amber::hir::Program program =
      lower_ok("def classify(shape, x):\n"
               "  case shape:\n"
               "    when whole as (Point(^x, y) | Rect(x: ^x, y: y)):\n"
               "      y\n"
               "    when {a:, **rest}:\n"
               "      rest\n"
               "    when pattern(route(\"/users/:id\")) with {id:, **null}:\n"
               "      id\n"
               "    else:\n"
               "      shape\n");
  const amber::hir::Procedure *classify =
      procedure_by_name(program, "classify");
  expect(classify != nullptr, "advanced classify procedure exists");

  const amber::ast::Expr *stmt = list_item(*classify->body, "items", 0);
  expect(stmt != nullptr, "advanced case stmt exists");
  const amber::ast::Expr *dispatch = node_field(*stmt, "expr");
  expect(dispatch != nullptr && dispatch->kind == "HMatchDispatch",
         "advanced case lowers to HMatchDispatch");

  const amber::ast::Expr *arm0 = list_item(*dispatch, "arms", 0);
  const amber::ast::Expr *arm1 = list_item(*dispatch, "arms", 1);
  const amber::ast::Expr *arm2 = list_item(*dispatch, "arms", 2);
  expect(arm0 != nullptr && arm1 != nullptr && arm2 != nullptr,
         "advanced arms exist");

  const amber::ast::Expr *pattern0 = node_field(*arm0, "pattern");
  expect(pattern0 != nullptr && pattern0->kind == "PatAs",
         "as-pattern lowered structurally");
  expect(string_field(*pattern0, "bind_name") == "whole",
         "as-pattern bind name");
  const amber::ast::Expr *inner0 = node_field(*pattern0, "inner");
  expect(inner0 != nullptr && inner0->kind == "PatOr", "or-pattern inner");
  const amber::ast::Expr *or_alt0 = list_item(*inner0, "alternatives", 0);
  const amber::ast::Expr *or_alt1 = list_item(*inner0, "alternatives", 1);
  expect(or_alt0 != nullptr && or_alt1 != nullptr, "or alternatives exist");
  expect(or_alt0->kind == "PatHead" && or_alt1->kind == "PatHead",
         "or alternatives are head patterns");
  expect(list_item(*or_alt0, "pos_args", 0) != nullptr &&
             list_item(*or_alt0, "pos_args", 0)->kind == "PatPin",
         "pin pattern preserved");

  const amber::ast::Expr *compiled0 = node_field(*arm0, "compiled_pattern");
  expect(compiled0 != nullptr && compiled0->kind == "HCompiledPattern",
         "advanced compiled pattern exists");
  expect(node_field(*compiled0, "pattern_ir") != nullptr &&
             node_field(*compiled0, "pattern_ir")->kind == "PIrAs",
         "advanced compiled as IR");

  const amber::ast::Expr *pattern1 = node_field(*arm1, "pattern");
  expect(pattern1 != nullptr && pattern1->kind == "PatMap",
         "map-rest pattern preserved");
  expect(string_field(*pattern1, "rest_mode") == "bind_rest(rest)",
         "map-rest bind mode");
  const amber::ast::Expr *compiled1 = node_field(*arm1, "compiled_pattern");
  const amber::ast::Expr *compiled1_ir =
      compiled1 == nullptr ? nullptr : node_field(*compiled1, "pattern_ir");
  expect(compiled1_ir != nullptr && compiled1_ir->kind == "PIrMap",
         "map-rest compiled IR");
  expect(
      node_field(*compiled1, "match_program") != nullptr &&
          node_field(*node_field(*compiled1, "match_program"), "root") !=
              nullptr &&
          node_field(*node_field(*compiled1, "match_program"), "root")->kind ==
              "PMap",
      "map-rest compiled match program");
  expect(string_field(*compiled1_ir, "rest_kind") == "capture",
         "map-rest IR rest kind");
  expect(bool_field(*compiled1_ir, "capture_rest"),
         "map-rest IR marks capture_rest");
  expect(!bool_field(*compiled1_ir, "strict_map"),
         "map-rest IR is not strict map");
  expect(!bool_field(*compiled1_ir, "ignore_rest"),
         "map-rest IR does not ignore rest");
  expect(bool_field(*compiled1_ir, "needs_full_map"),
         "map-rest IR requires full map");
  expect(string_field(*compiled1_ir, "rest_binding") == "rest",
         "map-rest IR carries rest binding");
  expect(list_item(*compiled1_ir, "requested_keys", 0) != nullptr &&
             string_field(*list_item(*compiled1_ir, "requested_keys", 0),
                          "name") == "a",
         "map-rest IR requested key");

  const amber::ast::Expr *pattern2 = node_field(*arm2, "pattern");
  expect(pattern2 != nullptr && pattern2->kind == "PatDynamic",
         "dynamic pattern object preserved");
  expect(string_field(*pattern2, "matcher_text") == "route(\"/users/:id\")",
         "dynamic matcher text");
  const amber::ast::Expr *export_map =
      node_field(*pattern2, "export_map_pattern");
  expect(export_map != nullptr && export_map->kind == "PatMap",
         "dynamic export map preserved");
  expect(string_field(*export_map, "rest_mode") == "strict_null",
         "dynamic export map strict-null");
  const amber::ast::Expr *compiled2 = node_field(*arm2, "compiled_pattern");
  const amber::ast::Expr *compiled2_ir =
      compiled2 == nullptr ? nullptr : node_field(*compiled2, "pattern_ir");
  expect(compiled2_ir != nullptr && compiled2_ir->kind == "PIrDynamic",
         "dynamic pattern compiles to dynamic IR");
  expect(
      node_field(*compiled2, "match_program") != nullptr &&
          node_field(*node_field(*compiled2, "match_program"), "root") !=
              nullptr &&
          node_field(*node_field(*compiled2, "match_program"), "root")->kind ==
              "PDynamic",
      "dynamic pattern compiles to dynamic match program");
  expect(string_field(*compiled2_ir, "protocol") == "DynamicMatchResult",
         "dynamic IR protocol tag");
  expect(string_field(*compiled2_ir, "binding_mode") == "map_pattern",
         "dynamic IR binding mode");
  expect(!bool_field(*compiled2_ir, "requires_empty_bindings"),
         "dynamic IR with map does not require empty bindings");
  expect(node_field(*compiled2_ir, "matcher_expr") != nullptr &&
             contains_kind(*node_field(*compiled2_ir, "matcher_expr"), "HCall"),
         "dynamic IR carries lowered matcher expr");
  const amber::ast::Expr *compiled_export_map =
      node_field(*compiled2_ir, "export_map_pattern");
  expect(compiled_export_map != nullptr &&
             compiled_export_map->kind == "PIrMap",
         "dynamic export map compiles");
  expect(string_field(*compiled_export_map, "rest_kind") == "strict_null",
         "dynamic export map IR rest kind");
  expect(bool_field(*compiled_export_map, "strict_map"),
         "dynamic export map IR marks strict map");
  expect(!bool_field(*compiled_export_map, "capture_rest"),
         "dynamic export map IR does not capture rest");
  expect(!bool_field(*compiled_export_map, "ignore_rest"),
         "dynamic export map IR does not ignore rest");
  expect(bool_field(*compiled_export_map, "needs_full_map"),
         "dynamic export map IR requires full map");
  expect(list_item(*compiled_export_map, "requested_keys", 0) != nullptr &&
             string_field(*list_item(*compiled_export_map, "requested_keys", 0),
                          "name") == "id",
         "dynamic export map IR requested key");
  const amber::ast::Expr *compiled2_program =
      node_field(*node_field(*compiled2, "match_program"), "root");
  expect(compiled2_program != nullptr &&
             node_field(*compiled2_program, "matcher_expr") != nullptr &&
             contains_kind(*node_field(*compiled2_program, "matcher_expr"),
                           "HCall"),
         "dynamic match program carries lowered matcher expr");
}

void test_block_param_pattern_lowering() {
  const amber::hir::Program program =
      lower_ok("def transform(xs):\n"
               "  xs.map |[head, *tail]|: head\n");
  const amber::hir::Procedure *transform =
      procedure_by_name(program, "transform");
  expect(transform != nullptr, "transform procedure exists");

  const amber::ast::Expr *stmt = list_item(*transform->body, "items", 0);
  expect(stmt != nullptr, "transform stmt exists");
  const amber::ast::Expr *send_expr = node_field(*stmt, "expr");
  expect(send_expr != nullptr && send_expr->kind == "HSend",
         "block pattern send exists");
  const amber::ast::Expr *closure = node_field(*send_expr, "block");
  expect(closure != nullptr && closure->kind == "HClosure",
         "block pattern closure exists");
  const amber::ast::Expr *signature = node_field(*closure, "signature");
  expect(signature != nullptr, "block pattern closure signature exists");
  expect(list_item(*signature, "params", 0) != nullptr &&
             string_field(*list_item(*signature, "params", 0), "local_name") ==
                 "__arg0",
         "complex pattern block param uses synthetic arg slot");
  const amber::ast::Expr *param_pattern =
      list_item(*closure, "param_patterns", 0);
  expect(param_pattern != nullptr && param_pattern->kind == "HParamPattern",
         "block param pattern metadata exists");
  expect(string_field(*param_pattern, "param_slot") == "__arg0",
         "block param pattern slot");
  const amber::ast::Expr *pattern = node_field(*param_pattern, "pattern");
  const amber::ast::Expr *compiled =
      node_field(*param_pattern, "compiled_pattern");
  expect(pattern != nullptr && pattern->kind == "PatList",
         "block param list pattern preserved");
  expect(compiled != nullptr &&
             node_field(*compiled, "pattern_ir") != nullptr &&
             node_field(*compiled, "pattern_ir")->kind == "PIrList",
         "block param list pattern compiled");
  expect(
      node_field(*compiled, "match_program") != nullptr &&
          node_field(*node_field(*compiled, "match_program"), "root") !=
              nullptr &&
          node_field(*node_field(*compiled, "match_program"), "root")->kind ==
              "PSeqList",
      "block param list match program");
}

void test_simple_block_param_pattern_slot() {
  const amber::hir::Program program = lower_ok("def transform(xs):\n"
                                               "  xs.map |x|: x\n");
  const amber::hir::Procedure *transform =
      procedure_by_name(program, "transform");
  expect(transform != nullptr, "simple transform procedure exists");

  const amber::ast::Expr *stmt = list_item(*transform->body, "items", 0);
  expect(stmt != nullptr, "simple transform stmt exists");
  const amber::ast::Expr *send_expr = node_field(*stmt, "expr");
  expect(send_expr != nullptr && send_expr->kind == "HSend",
         "simple block send exists");
  const amber::ast::Expr *closure = node_field(*send_expr, "block");
  expect(closure != nullptr && closure->kind == "HClosure",
         "simple block closure exists");
  const amber::ast::Expr *param_pattern =
      list_item(*closure, "param_patterns", 0);
  expect(param_pattern != nullptr && param_pattern->kind == "HParamPattern",
         "simple block param pattern metadata exists");
  expect(string_field(*param_pattern, "param_slot") == "x",
         "simple block param slot follows actual local");
  const amber::ast::Expr *pattern = node_field(*param_pattern, "pattern");
  expect(pattern != nullptr && pattern->kind == "PatBind",
         "simple block param pattern preserved");
}

void test_pattern_assignment_lowering() {
  const amber::hir::Program program = lower_ok("def unpack(values):\n"
                                               "  [head, *tail] = values\n");
  const amber::hir::Procedure *unpack = procedure_by_name(program, "unpack");
  expect(unpack != nullptr, "unpack procedure exists");
  const amber::ast::Expr *stmt = list_item(*unpack->body, "items", 0);
  expect(stmt != nullptr, "pattern assign stmt exists");
  const amber::ast::Expr *expr = node_field(*stmt, "expr");
  expect(expr != nullptr && expr->kind == "HPatternAssign",
         "pattern assignment lowers to HPatternAssign");
  expect(string_field(*expr, "fail_mode") == "match_error",
         "pattern assignment fail mode");
  const amber::ast::Expr *pattern = node_field(*expr, "pattern");
  const amber::ast::Expr *compiled = node_field(*expr, "compiled_pattern");
  expect(pattern != nullptr && pattern->kind == "PatList",
         "pattern assignment pattern preserved");
  expect(compiled != nullptr &&
             node_field(*compiled, "pattern_ir") != nullptr &&
             node_field(*compiled, "pattern_ir")->kind == "PIrList",
         "pattern assignment compiled list IR");
  expect(
      node_field(*compiled, "match_program") != nullptr &&
          node_field(*node_field(*compiled, "match_program"), "root") !=
              nullptr &&
          node_field(*node_field(*compiled, "match_program"), "root")->kind ==
              "PSeqList",
      "pattern assignment compiled list match program");
  expect(node_field(*expr, "value") != nullptr &&
             node_field(*expr, "value")->kind == "HLoadLocal",
         "pattern assignment rhs lowered");
}

void test_default_param_lowering() {
  const amber::hir::Program program = lower_ok("def configure(x, y = x + 1):\n"
                                               "  y\n");
  const amber::ast::Expr *method = list_item(*program.root, "items", 0);
  expect(method != nullptr && method->kind == "HMethod",
         "configure method exists");
  const amber::ast::Expr *signature = node_field(*method, "signature");
  expect(signature != nullptr, "configure signature exists");
  const amber::ast::Expr *param = list_item(*signature, "params", 1);
  expect(param != nullptr && bool_field(*param, "has_default"),
         "default param metadata preserved");
  const amber::ast::Expr *default_expr = node_field(*param, "default_expr");
  expect(default_expr != nullptr && default_expr->kind == "HSend",
         "default expression lowered to HSend");
  const amber::ast::Expr *receiver = node_field(*default_expr, "receiver");
  expect(receiver != nullptr && receiver->kind == "HLoadLocal",
         "default expression reads local slot");
}

void test_dynamic_pattern_without_with_lowering() {
  const amber::hir::Program program =
      lower_ok("def classify(shape):\n"
               "  case shape:\n"
               "    when pattern(route(shape)):\n"
               "      shape\n"
               "    else:\n"
               "      null\n");
  const amber::hir::Procedure *classify =
      procedure_by_name(program, "classify");
  expect(classify != nullptr, "no-with classify procedure exists");

  const amber::ast::Expr *stmt = list_item(*classify->body, "items", 0);
  expect(stmt != nullptr, "no-with case stmt exists");
  const amber::ast::Expr *dispatch = node_field(*stmt, "expr");
  expect(dispatch != nullptr && dispatch->kind == "HMatchDispatch",
         "no-with case lowers to HMatchDispatch");

  const amber::ast::Expr *arm0 = list_item(*dispatch, "arms", 0);
  expect(arm0 != nullptr, "no-with arm exists");
  const amber::ast::Expr *compiled0 = node_field(*arm0, "compiled_pattern");
  const amber::ast::Expr *compiled0_ir =
      compiled0 == nullptr ? nullptr : node_field(*compiled0, "pattern_ir");
  expect(compiled0_ir != nullptr && compiled0_ir->kind == "PIrDynamic",
         "no-with dynamic pattern compiles");
  expect(string_field(*compiled0_ir, "protocol") == "DynamicMatchResult",
         "no-with dynamic IR protocol tag");
  expect(string_field(*compiled0_ir, "binding_mode") == "forbid_bindings",
         "no-with dynamic IR binding mode");
  expect(bool_field(*compiled0_ir, "requires_empty_bindings"),
         "no-with dynamic IR requires empty bindings");
  expect(node_field(*compiled0_ir, "export_map_pattern") == nullptr,
         "no-with dynamic IR has no export map");
}

void test_direct_capture_lowering() {
  const amber::hir::Program program = lower_ok("def offsetter(δ):\n"
                                               "  values.map: _1 + δ\n");
  const amber::hir::Procedure *offsetter =
      procedure_by_name(program, "offsetter");
  expect(offsetter != nullptr, "offsetter procedure exists");

  const amber::ast::Expr *stmt = list_item(*offsetter->body, "items", 0);
  expect(stmt != nullptr, "offsetter statement exists");
  const amber::ast::Expr *send_expr = node_field(*stmt, "expr");
  expect(send_expr != nullptr && send_expr->kind == "HSend",
         "map call lowers to HSend");
  const amber::ast::Expr *closure = node_field(*send_expr, "block");
  expect(closure != nullptr && closure->kind == "HClosure",
         "map block lowers to HClosure");
  const amber::ast::Expr *capture = list_item(*closure, "captures", 0);
  expect(capture != nullptr && capture->kind == "HCapture",
         "closure capture node exists");
  expect(string_field(*capture, "slot") == "u0", "direct capture slot");
  expect(string_field(*capture, "name") == "δ", "direct capture name");
  expect(string_field(*capture, "source_kind") == "local",
         "direct capture source kind");
  expect(string_field(*capture, "source_slot") == "l0",
         "direct capture source slot");

  const amber::hir::Procedure *block =
      procedure_by_id(program, string_field(*closure, "procedure"));
  expect(block != nullptr, "direct capture block procedure exists");
  expect(block->captures.size() == 1, "direct capture procedure metadata");
  expect(block->captures[0].slot == "u0", "direct capture procedure slot");
  expect(block->captures[0].source_kind == "local",
         "direct capture procedure source kind");

  const amber::ast::Expr *block_stmt = list_item(*block->body, "items", 0);
  expect(block_stmt != nullptr, "direct capture block stmt exists");
  const amber::ast::Expr *block_expr = node_field(*block_stmt, "expr");
  expect(block_expr != nullptr && block_expr->kind == "HSend",
         "direct capture body lowers to HSend");
  const amber::ast::Expr *captured_arg = list_item(*block_expr, "pos_args", 0);
  expect(captured_arg != nullptr && captured_arg->kind == "HLoadCapture",
         "captured param lowers to HLoadCapture");
  expect(string_field(*captured_arg, "slot") == "u0", "captured param slot");
}

void test_nested_capture_propagation() {
  const amber::hir::Program program = lower_ok("def nest(xs, α):\n"
                                               "  xs.map: _1.filter: α\n");
  const amber::hir::Procedure *nest = procedure_by_name(program, "nest");
  expect(nest != nullptr, "nest procedure exists");

  const amber::ast::Expr *stmt = list_item(*nest->body, "items", 0);
  expect(stmt != nullptr, "nest statement exists");
  const amber::ast::Expr *map_expr = node_field(*stmt, "expr");
  expect(map_expr != nullptr && map_expr->kind == "HSend", "map send exists");
  const amber::ast::Expr *outer_closure = node_field(*map_expr, "block");
  expect(outer_closure != nullptr && outer_closure->kind == "HClosure",
         "outer closure exists");

  const amber::ast::Expr *outer_capture =
      list_item(*outer_closure, "captures", 0);
  expect(outer_capture != nullptr && outer_capture->kind == "HCapture",
         "outer closure capture exists");
  expect(string_field(*outer_capture, "source_kind") == "local",
         "outer capture comes from method local");
  expect(string_field(*outer_capture, "source_slot") == "l1",
         "outer capture source slot");

  const amber::hir::Procedure *outer_block =
      procedure_by_id(program, string_field(*outer_closure, "procedure"));
  expect(outer_block != nullptr, "outer block procedure exists");
  expect(outer_block->captures.size() == 1, "outer block capture metadata");
  expect(outer_block->captures[0].slot == "u0", "outer capture slot");

  const amber::ast::Expr *outer_stmt =
      list_item(*outer_block->body, "items", 0);
  expect(outer_stmt != nullptr, "outer block stmt exists");
  const amber::ast::Expr *filter_expr = node_field(*outer_stmt, "expr");
  expect(filter_expr != nullptr && filter_expr->kind == "HSend",
         "filter send exists");
  const amber::ast::Expr *inner_closure = node_field(*filter_expr, "block");
  expect(inner_closure != nullptr && inner_closure->kind == "HClosure",
         "inner closure exists");

  const amber::ast::Expr *inner_capture =
      list_item(*inner_closure, "captures", 0);
  expect(inner_capture != nullptr && inner_capture->kind == "HCapture",
         "inner closure capture exists");
  expect(string_field(*inner_capture, "source_kind") == "capture",
         "inner closure captures from outer capture");
  expect(string_field(*inner_capture, "source_slot") == "u0",
         "inner closure capture source slot");

  const amber::hir::Procedure *inner_block =
      procedure_by_id(program, string_field(*inner_closure, "procedure"));
  expect(inner_block != nullptr, "inner block procedure exists");
  expect(inner_block->captures.size() == 1, "inner block capture metadata");
  const amber::ast::Expr *inner_stmt =
      list_item(*inner_block->body, "items", 0);
  expect(inner_stmt != nullptr, "inner block stmt exists");
  const amber::ast::Expr *inner_expr = node_field(*inner_stmt, "expr");
  expect(inner_expr != nullptr && inner_expr->kind == "HLoadCapture",
         "inner body loads propagated capture");
  expect(string_field(*inner_expr, "slot") == "u0", "inner body capture slot");
}

} // namespace

int main() {
  test_method_and_send_lowering();
  test_module_def_materializes_callable_binding();
  test_module_def_closure_captures_prior_function();
  test_module_def_closure_captures_self();
  test_module_clause_def_materializes_callable_binding();
  test_safe_navigation_lowering();
  test_safe_call_and_index_lowering();
  test_builtin_send_lowering();
  test_collection_literal_lowering();
  test_inline_conditional_and_conditional_literal_lowering();
  test_shadowed_send_stays_call();
  test_w13_operator_lowering();
  test_clause_method_lowering();
  test_case_pattern_lowering();
  test_case_matcher_expr_lowering();
  test_dynamic_pattern_compilation();
  test_advanced_pattern_lowering();
  test_block_param_pattern_lowering();
  test_simple_block_param_pattern_slot();
  test_pattern_assignment_lowering();
  test_default_param_lowering();
  test_dynamic_pattern_without_with_lowering();
  test_direct_capture_lowering();
  test_nested_capture_propagation();
  std::cout << "hir_tests: ok\n";
  return 0;
}
