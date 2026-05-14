#include "runtime/vm.h"

#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/ast/expr.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "vm test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::EmitResult emit_ok(const std::string &source) {
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

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(emit_result.diagnostics);
    std::exit(1);
  }
  return emit_result;
}

const amber::bytecode::BcMethod *
method_by_name(const amber::bytecode::BcModule &module,
               const std::string &name);

void test_execute_emitted_method() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def echo(x):\n"
                                                          "  x\n");
  expect(emit_result.module.methods.size() == 1, "expected one method");

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.methods[0].entry_code_id,
      {amber::runtime::Value::integer(7)});
  expect(exec.ok(), "echo execution failed");
  expect(exec.value.is_integer(), "echo should return integer");
  expect(exec.value.as_integer() == 7, "echo should return argument");
}

void test_branching_and_last_result() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def flag(x):\n"
                                                          "  if x:\n"
                                                          "    1\n"
                                                          "  else:\n"
                                                          "    0\n");
  expect(emit_result.module.methods.size() == 1, "expected one flag method");

  const std::uint32_t code_id = emit_result.module.methods[0].entry_code_id;
  const amber::runtime::ExecutionResult when_true =
      amber::runtime::execute_code(emit_result.module, code_id,
                                   {amber::runtime::Value::boolean(true)});
  expect(when_true.ok(), "flag(true) execution failed");
  expect(when_true.value.is_integer() && when_true.value.as_integer() == 1,
         "flag(true) should return 1");

  const amber::runtime::ExecutionResult when_false =
      amber::runtime::execute_code(emit_result.module, code_id,
                                   {amber::runtime::Value::boolean(false)});
  expect(when_false.ok(), "flag(false) execution failed");
  expect(when_false.value.is_integer() && when_false.value.as_integer() == 0,
         "flag(false) should return 0");

  const amber::runtime::ExecutionResult when_null =
      amber::runtime::execute_code(emit_result.module, code_id,
                                   {amber::runtime::Value::null()});
  expect(when_null.ok(), "flag(null) execution failed");
  expect(when_null.value.is_integer() && when_null.value.as_integer() == 0,
         "flag(null) should treat null as falsey");
}

void test_manual_closure_call_and_capture() {
  using namespace amber::bytecode;

  BcModule module;
  Constant five;
  five.kind = ConstantKind::Integer;
  five.int_value = 5;
  module.const_pool.push_back(five);

  BcCode outer;
  outer.code_id = 1;
  outer.kind = CodeKind::Method;
  outer.reg_count = 3;
  outer.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  outer.instructions.push_back(
      {Opcode::MakeClosure,
       {{1, false}, {2, false}, {1, false}, {0, false}, {0, false}}});
  outer.instructions.push_back({Opcode::Call,
                                {{2, false},
                                 {1, false},
                                 {0, false},
                                 {0, false},
                                 {-1, true},
                                 {0, false}}});
  outer.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode inner;
  inner.code_id = 2;
  inner.kind = CodeKind::Block;
  inner.reg_count = 1;
  inner.instructions.push_back({Opcode::LoadUpval, {{0, false}, {0, false}}});
  inner.instructions.push_back({Opcode::Return, {{0, false}}});

  module.code_objects = {outer, inner};

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1);
  expect(exec.ok(), "manual closure execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 5,
         "manual closure should return captured integer");
}

void test_execute_emitted_send_method() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def add(x, y):\n"
                                                          "  x + y\n");
  expect(emit_result.module.methods.size() == 1, "expected add method");

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.methods[0].entry_code_id,
      {amber::runtime::Value::integer(9), amber::runtime::Value::integer(4)});
  expect(exec.ok(), "add execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 13,
         "add should return summed integer");
}

void test_execute_emitted_compare_method() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def choose(x):\n"
                                                          "  if x > 0:\n"
                                                          "    x\n"
                                                          "  else:\n"
                                                          "    0\n");
  expect(emit_result.module.methods.size() == 1, "expected choose method");
  const std::uint32_t code_id = emit_result.module.methods[0].entry_code_id;

  const amber::runtime::ExecutionResult positive = amber::runtime::execute_code(
      emit_result.module, code_id, {amber::runtime::Value::integer(3)});
  expect(positive.ok(), "choose(3) execution failed");
  expect(positive.value.is_integer() && positive.value.as_integer() == 3,
         "choose(3) should return input");

  const amber::runtime::ExecutionResult negative = amber::runtime::execute_code(
      emit_result.module, code_id, {amber::runtime::Value::integer(-2)});
  expect(negative.ok(), "choose(-2) execution failed");
  expect(negative.value.is_integer() && negative.value.as_integer() == 0,
         "choose(-2) should return zero");
}

void test_execute_emitted_default_method() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Config:\n"
              "  class_method def build(x, y = x + 1):\n"
              "    y\n"
              "\n"
              "def probe():\n"
              "  Config.build(7)\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "defaulted probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(exec.ok(), "defaulted method execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 8,
         "default thunk should materialize y = x + 1");
}

void test_execute_emitted_keyword_method() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Config:\n"
              "  class_method def build(x, α:, β: 2):\n"
              "    x + α + β\n"
              "\n"
              "def probe():\n"
              "  Config.build(4, α: 5)\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "keyword probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(exec.ok(), "keyword method execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 11,
         "keyword shaping should bind α and materialize β default");
}

void test_execute_emitted_block_send() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Config:\n"
              "  class_method def build(x):\n"
              "    x\n"
              "\n"
              "def probe():\n"
              "  Config.build(4): 9\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "block send probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(exec.ok(), "block send execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 4,
         "user-defined SEND should accept forwarded block");
}

void test_manual_dynamic_send() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"+"};

  Constant plus;
  plus.kind = ConstantKind::SymbolRef;
  plus.ref_id = 0;
  module.const_pool.push_back(plus);

  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  module.const_pool.push_back(two);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 4;
  code.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  code.instructions.push_back({Opcode::LoadK, {{2, false}, {1, false}}});
  code.instructions.push_back({Opcode::SendDyn,
                               {{3, false},
                                {0, false},
                                {1, false},
                                {1, false},
                                {2, false},
                                {0, false},
                                {-1, true},
                                {0, false}}});
  code.instructions.push_back({Opcode::Return, {{3, false}}});
  module.code_objects.push_back(code);

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::integer(8)});
  expect(exec.ok(), "dynamic send execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 10,
         "dynamic send should dispatch by symbol selector");
}

const amber::bytecode::BcMethod *
method_by_name(const amber::bytecode::BcModule &module,
               const std::string &name) {
  for (const amber::bytecode::BcMethod &method : module.methods) {
    if (method.selector_sym_id < module.symbols.size() &&
        module.symbols[method.selector_sym_id] == name) {
      return &method;
    }
  }
  return nullptr;
}

std::uint32_t symbol_id_or_die(const amber::bytecode::BcModule &module,
                               const std::string &name) {
  for (std::uint32_t i = 0; i < module.symbols.size(); ++i) {
    if (module.symbols[i] == name) {
      return i;
    }
  }
  std::cerr << "vm test failed: missing symbol " << name << "\n";
  std::exit(1);
}

std::uint32_t ensure_symbol_id(amber::bytecode::BcModule *module,
                               const std::string &name) {
  for (std::uint32_t i = 0; i < module->symbols.size(); ++i) {
    if (module->symbols[i] == name) {
      return i;
    }
  }
  module->symbols.push_back(name);
  return static_cast<std::uint32_t>(module->symbols.size() - 1U);
}

amber::runtime::Value make_symbol_map(
    const amber::bytecode::BcModule &module,
    std::initializer_list<std::pair<const char *, amber::runtime::Value>>
        entries) {
  std::vector<amber::runtime::MapEntry> map_entries;
  map_entries.reserve(entries.size());
  for (const auto &entry : entries) {
    map_entries.push_back(
        {symbol_id_or_die(module, entry.first), entry.second});
  }
  return amber::runtime::make_symbol_map_value(std::move(map_entries));
}

const amber::runtime::RuntimeArenaStats *
arena_stats_for(const amber::runtime::RuntimeHeapStats &stats,
                std::uint64_t worker_id) {
  for (const amber::runtime::RuntimeArenaStats &arena : stats.arenas) {
    if (arena.worker_id == worker_id) {
      return &arena;
    }
  }
  return nullptr;
}

void test_execute_emitted_class_method_send() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  class_method def find(id):\n"
              "    id\n"
              "\n"
              "def probe():\n"
              "  Particle.find(4)\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "probe method exists");
  amber::runtime::RuntimeWorld table_world(emit_result.module);
  expect(table_world.method_table_size(
             0, amber::runtime::MethodTableSide::Class) == 1,
         "class-side method table should include emitted class method");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "class-side send execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 4,
         "class-side send should return class method result");
}

void test_execute_emitted_constructor_call() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  def init(x):\n"
              "    @mass = x\n"
              "  def mass():\n"
              "    @mass\n"
              "\n"
              "def probe():\n"
              "  Particle(4).mass()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "constructor probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "constructor call execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 4,
         "constructor call should return initialized ivar");
}

void test_execute_emitted_constructor_auto_assign() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  def init(@масса):\n"
              "    pass\n"
              "  def масса():\n"
              "    @масса\n"
              "\n"
              "def probe():\n"
              "  Particle(7).масса()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "auto-assign probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "constructor auto-assign execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 7,
         "constructor auto-assign should materialize ivar");
}

void test_execute_emitted_constructor_default() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  def init(x, y = x + 1):\n"
              "    @mass = y\n"
              "  def mass():\n"
              "    @mass\n"
              "\n"
              "def probe():\n"
              "  Particle(4).mass()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "constructor default probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "constructor default execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 5,
         "constructor default should materialize trailing default param");
}

void test_execute_emitted_cvar_store_and_load() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Settings:\n"
              "  class_method def set(x):\n"
              "    @@ρ = x\n"
              "  class_method def get():\n"
              "    @@ρ\n"
              "\n"
              "def probe():\n"
              "  Settings.set(13)\n"
              "  Settings.get()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "cvar probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "cvar store/load execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 13,
         "class variable should round-trip through emitted methods");
}

void test_execute_emitted_constructor_cvar_auto_assign() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Settings:\n"
              "  def init(@@ρ):\n"
              "    pass\n"
              "  class_method def ρ():\n"
              "    @@ρ\n"
              "\n"
              "def probe():\n"
              "  Settings(17)\n"
              "  Settings.ρ()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "cvar auto-assign probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "cvar auto-assign execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 17,
         "constructor auto-assign should materialize class variable");
}

void test_execute_emitted_superclass_dispatch() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Base:\n"
              "  def mass():\n"
              "    11\n"
              "\n"
              "class Particle < Base:\n"
              "  def own():\n"
              "    0\n"
              "\n"
              "def probe():\n"
              "  Particle().mass()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "superclass probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "superclass dispatch execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 11,
         "instance SEND should fall through superclass chain");
}

void test_execute_emitted_include_linearization() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("mixin Older:\n"
              "  def value():\n"
              "    1\n"
              "\n"
              "mixin Newer:\n"
              "  def value():\n"
              "    2\n"
              "\n"
              "class Box:\n"
              "  include Older, Newer\n"
              "\n"
              "def probe():\n"
              "  Box().value()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "include probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "include dispatch execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 2,
         "later include should win in instance-side lookup");
}

void test_execute_emitted_extend_linearization() {
  const amber::bytecode::EmitResult emit_result = emit_ok("mixin Tagged:\n"
                                                          "  def label():\n"
                                                          "    23\n"
                                                          "\n"
                                                          "class Box:\n"
                                                          "  extend Tagged\n"
                                                          "\n"
                                                          "def probe():\n"
                                                          "  Box.label()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "extend probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "extend dispatch execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 23,
         "class-side lookup should see extended mixin methods");
}

void test_execute_emitted_method_missing_instance() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Proxy:\n"
              "  def method_missing(name, α:):\n"
              "    α\n"
              "\n"
              "def probe():\n"
              "  Proxy().unknown(α: 5)\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "instance method_missing probe exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "instance method_missing execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 5,
         "method_missing should receive forwarded keyword arguments");
}

void test_execute_emitted_method_missing_class_side() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Proxy:\n"
              "  class_method def method_missing(name):\n"
              "    29\n"
              "\n"
              "def probe():\n"
              "  Proxy.unknown()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "class method_missing probe exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "class method_missing execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 29,
         "class-side miss should fall back to class method_missing");
}

void test_method_missing_does_not_recurse() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Proxy:\n"
              "  def own():\n"
              "    0\n"
              "\n"
              "def probe():\n"
              "  Proxy().method_missing()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "non-recursive method_missing probe exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(!exec.ok(), "missing method_missing should fail");
  expect(exec.fault.has_value() && exec.fault->error_name == "NoMethodError",
         "method_missing selector should not recurse into itself");
}

void test_execute_emitted_case_literal() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def classify(x):\n"
                                                          "  case x:\n"
                                                          "    when 1:\n"
                                                          "      11\n"
                                                          "    else:\n"
                                                          "      0\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "classify");
  expect(method != nullptr, "literal case method exists");

  const amber::runtime::ExecutionResult hit =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id,
                                   {amber::runtime::Value::integer(1)});
  expect(hit.ok(), "literal case hit failed");
  expect(hit.value.is_integer() && hit.value.as_integer() == 11,
         "literal case should take matching arm");

  const amber::runtime::ExecutionResult miss =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id,
                                   {amber::runtime::Value::integer(2)});
  expect(miss.ok(), "literal case miss failed");
  expect(miss.value.is_integer() && miss.value.as_integer() == 0,
         "literal case should take else arm");
}

void test_execute_emitted_case_pin() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def same(x, y):\n"
                                                          "  case y:\n"
                                                          "    when ^x:\n"
                                                          "      1\n"
                                                          "    else:\n"
                                                          "      0\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "same");
  expect(method != nullptr, "pin case method exists");

  const amber::runtime::ExecutionResult hit = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {amber::runtime::Value::integer(7), amber::runtime::Value::integer(7)});
  expect(hit.ok(), "pin case hit failed");
  expect(hit.value.is_integer() && hit.value.as_integer() == 1,
         "pin case should match equal value");

  const amber::runtime::ExecutionResult miss = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {amber::runtime::Value::integer(7), amber::runtime::Value::integer(8)});
  expect(miss.ok(), "pin case miss failed");
  expect(miss.value.is_integer() && miss.value.as_integer() == 0,
         "pin case should fall through to else");
}

void test_execute_emitted_case_bind() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def mirror(x):\n"
                                                          "  case x:\n"
                                                          "    when y:\n"
                                                          "      y\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "mirror");
  expect(method != nullptr, "bind case method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id,
                                   {amber::runtime::Value::integer(9)});
  expect(exec.ok(), "bind case execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 9,
         "bind case should materialize bound local");
}

void test_execute_emitted_case_bang_failure() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def classify!(x):\n"
                                                          "  case! x:\n"
                                                          "    when 1:\n"
                                                          "      11\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "classify!");
  expect(method != nullptr, "case! method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id,
                                   {amber::runtime::Value::integer(2)});
  expect(!exec.ok(), "case! miss should fail");
  expect(exec.fault.has_value() && exec.fault->error_name == "MatchError",
         "case! miss should raise MatchError");
}

void test_execute_emitted_case_const_class() {
  const amber::bytecode::EmitResult emit_result = emit_ok("class Marker:\n"
                                                          "  def own():\n"
                                                          "    0\n"
                                                          "\n"
                                                          "def classify(x):\n"
                                                          "  case x:\n"
                                                          "    when Marker:\n"
                                                          "      1\n"
                                                          "    else:\n"
                                                          "      0\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "classify");
  expect(method != nullptr, "const class case method exists");
  expect(!emit_result.module.classes.empty(), "marker class emitted");

  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const amber::runtime::ExecutionResult hit =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id,
                                   {amber::runtime::Value::instance(instance)});
  expect(hit.ok(), "const class case hit failed");
  expect(hit.value.is_integer() && hit.value.as_integer() == 1,
         "const class pattern should match instance of class");
}

void test_execute_emitted_case_list_exact() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def second(values):\n"
              "  case values:\n"
              "    when [1, x]:\n"
              "      x\n"
              "    else:\n"
              "      0\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "second");
  expect(method != nullptr, "list exact case method exists");

  const amber::runtime::ExecutionResult hit = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {amber::runtime::make_list_value({amber::runtime::Value::integer(1),
                                        amber::runtime::Value::integer(9)})});
  expect(hit.ok(), "list exact case hit failed");
  expect(hit.value.is_integer() && hit.value.as_integer() == 9,
         "list exact case should bind second element");

  const amber::runtime::ExecutionResult miss = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {amber::runtime::make_list_value({amber::runtime::Value::integer(1),
                                        amber::runtime::Value::integer(9),
                                        amber::runtime::Value::integer(10)})});
  expect(miss.ok(), "list exact case miss failed");
  expect(miss.value.is_integer() && miss.value.as_integer() == 0,
         "list exact case should reject extra elements");
}

void test_execute_emitted_pattern_assign_list_rest() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def unpack(values):\n"
              "  [head, *tail] = values\n"
              "  tail\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "unpack");
  expect(method != nullptr, "pattern assignment method exists");

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {amber::runtime::make_list_value({amber::runtime::Value::integer(3),
                                        amber::runtime::Value::integer(4),
                                        amber::runtime::Value::integer(5)})});
  expect(exec.ok(), "pattern assignment execution failed");
  expect(exec.value.is_list(), "pattern assignment tail should be a list");
  const std::shared_ptr<amber::runtime::ListValue> tail = exec.value.as_list();
  expect(tail != nullptr && tail->items.size() == 2,
         "pattern assignment tail should have two items");
  expect(tail->items[0].is_integer() && tail->items[0].as_integer() == 4 &&
             tail->items[1].is_integer() && tail->items[1].as_integer() == 5,
         "pattern assignment tail should contain remaining elements");
}

void test_execute_emitted_case_map_rest() {
  amber::bytecode::EmitResult emit_result = emit_ok("def capture(payload):\n"
                                                    "  case payload:\n"
                                                    "    when {a:, **rest}:\n"
                                                    "      rest\n"
                                                    "    else:\n"
                                                    "      null\n");
  ensure_symbol_id(&emit_result.module, "b");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "capture");
  expect(method != nullptr, "map-rest case method exists");

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {make_symbol_map(emit_result.module,
                       {{"a", amber::runtime::Value::integer(1)},
                        {"b", amber::runtime::Value::integer(7)}})});
  expect(exec.ok(), "map-rest case execution failed");
  expect(exec.value.is_map(), "map-rest case should return rest map");
  const std::shared_ptr<amber::runtime::MapValue> rest = exec.value.as_map();
  expect(rest != nullptr && rest->entries.size() == 1,
         "map-rest case should keep one extra key");
  expect(rest->entries[0].symbol_id ==
                 symbol_id_or_die(emit_result.module, "b") &&
             rest->entries[0].value.is_integer() &&
             rest->entries[0].value.as_integer() == 7,
         "map-rest case should capture remaining field");
}

void test_execute_emitted_case_map_strict_null() {
  amber::bytecode::EmitResult emit_result = emit_ok("def strict(payload):\n"
                                                    "  case payload:\n"
                                                    "    when {a:, **null}:\n"
                                                    "      1\n"
                                                    "    else:\n"
                                                    "      0\n");
  ensure_symbol_id(&emit_result.module, "b");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "strict");
  expect(method != nullptr, "strict map case method exists");

  const amber::runtime::ExecutionResult exact = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {make_symbol_map(emit_result.module,
                       {{"a", amber::runtime::Value::integer(1)}})});
  expect(exact.ok(), "strict map exact execution failed");
  expect(exact.value.is_integer() && exact.value.as_integer() == 1,
         "strict map should accept exact key set");

  const amber::runtime::ExecutionResult extra = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {make_symbol_map(emit_result.module,
                       {{"a", amber::runtime::Value::integer(1)},
                        {"b", amber::runtime::Value::integer(2)}})});
  expect(extra.ok(), "strict map extra-key execution failed");
  expect(extra.value.is_integer() && extra.value.as_integer() == 0,
         "strict map should reject extra keys");
}

void test_execute_emitted_clause_method_dispatch() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  def mass(0): 1\n"
              "  def mass(n) if n > 0: n\n"
              "\n"
              "def zero():\n"
              "  Particle().mass(0)\n"
              "\n"
              "def positive():\n"
              "  Particle().mass(4)\n");
  const amber::bytecode::BcMethod *zero =
      method_by_name(emit_result.module, "zero");
  const amber::bytecode::BcMethod *positive =
      method_by_name(emit_result.module, "positive");
  expect(zero != nullptr && positive != nullptr,
         "clause dispatch probes exist");

  const amber::runtime::ExecutionResult zero_result =
      amber::runtime::execute_code(emit_result.module, zero->entry_code_id);
  expect(zero_result.ok(), "clause dispatch zero execution failed");
  expect(zero_result.value.is_integer() && zero_result.value.as_integer() == 1,
         "first clause should match zero");

  const amber::runtime::ExecutionResult positive_result =
      amber::runtime::execute_code(emit_result.module, positive->entry_code_id);
  expect(positive_result.ok(), "clause dispatch positive execution failed");
  expect(positive_result.value.is_integer() &&
             positive_result.value.as_integer() == 4,
         "guarded clause should match positive argument");
}

void test_manual_make_map() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"α", "β"};

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  module.const_pool = {one, two};

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 4;
  code.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  code.instructions.push_back({Opcode::LoadK, {{1, false}, {1, false}}});
  code.instructions.push_back({Opcode::MakeMap,
                               {{2, false},
                                {2, false},
                                {0, false},
                                {0, false},
                                {1, false},
                                {1, false}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(code);

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1);
  expect(exec.ok(), "MAKE_MAP execution failed");
  expect(exec.value.is_map(), "MAKE_MAP should materialize map value");
  const std::shared_ptr<amber::runtime::MapValue> map = exec.value.as_map();
  expect(map != nullptr && map->entries.size() == 2,
         "MAKE_MAP should preserve two entries");
  expect(map->entries[0].symbol_id == 0 && map->entries[0].value.is_integer() &&
             map->entries[0].value.as_integer() == 1,
         "MAKE_MAP should preserve first entry");
  expect(map->entries[1].symbol_id == 1 && map->entries[1].value.is_integer() &&
             map->entries[1].value.as_integer() == 2,
         "MAKE_MAP should preserve second entry");
}

void test_manual_instance_send_dispatch() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Particle", "mass"};
  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  BcClass klass;
  klass.class_name_sym_id = 0;
  klass.method_range_start = 0;
  klass.method_range_count = 1;
  module.classes.push_back(klass);

  BcMethod method;
  method.selector_sym_id = 1;
  method.owner_dispatch_ref = 0;
  method.signature_blob_id = 0;
  method.entry_code_id = 2;
  method.flags = 1;
  module.methods.push_back(method);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back({Opcode::Send,
                                 {{1, false},
                                  {0, false},
                                  {1, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 2;
  body.instructions.push_back({Opcode::LoadSelf, {{0, false}}});
  body.instructions.push_back(
      {Opcode::LoadIvar, {{1, false}, {0, false}, {1, false}, {0, false}}});
  body.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {caller, body};

  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->ivars["mass"] = amber::runtime::Value::integer(11);

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(exec.ok(), "instance send dispatch failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 11,
         "instance send should dispatch to local method table");
}

void test_manual_store_and_load_ivar() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"mass"};

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 3;
  code.instructions.push_back({Opcode::LoadSelf, {{1, false}}});
  code.instructions.push_back(
      {Opcode::StoreIvar, {{1, false}, {0, false}, {0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::LoadIvar, {{2, false}, {1, false}, {0, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(code);

  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 0;

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::integer(17)},
      amber::runtime::Value::instance(instance));
  expect(exec.ok(), "ivar store/load execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 17,
         "ivar load should return stored value");
  expect(instance->ivars.count("mass") == 1,
         "ivar slot should be materialized");
  expect(instance->ivars.at("mass").is_integer() &&
             instance->ivars.at("mass").as_integer() == 17,
         "ivar map should contain stored integer");
}

void test_manual_store_and_load_cvar() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"ρ"};
  module.classes.push_back(BcClass{});

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 3;
  code.instructions.push_back({Opcode::LoadSelf, {{1, false}}});
  code.instructions.push_back(
      {Opcode::StoreCvar, {{1, false}, {0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::LoadCvar, {{2, false}, {1, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(code);

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::integer(19)},
      amber::runtime::Value::class_object(0));
  expect(exec.ok(), "cvar store/load execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 19,
         "cvar load should return stored value");
}

void test_manual_multi_segment_lookup_const() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"physics", "Particle"};

  Constant particle_path;
  particle_path.kind = ConstantKind::Path;
  particle_path.items = {0, 1};
  module.const_pool.push_back(particle_path);

  BcClass particle;
  particle.class_name_sym_id = 1;
  module.classes.push_back(particle);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 1;
  code.instructions.push_back({Opcode::LookupConst, {{0, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects.push_back(code);

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1);
  expect(exec.ok(), "multi-segment LOOKUP_CONST failed");
  expect(exec.value.is_class_object() &&
             exec.value.as_class_object().class_index == 0,
         "multi-segment LOOKUP_CONST should resolve class leaf");
}

void test_manual_multi_segment_superclass_dispatch() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"physics", "Base", "Child", "answer"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant base_path;
  base_path.kind = ConstantKind::Path;
  base_path.items = {0, 1};
  module.const_pool.push_back(base_path);

  Constant answer_value;
  answer_value.kind = ConstantKind::Integer;
  answer_value.int_value = 41;
  module.const_pool.push_back(answer_value);

  BcClass base;
  base.class_name_sym_id = 1;
  base.method_range_start = 0;
  base.method_range_count = 1;
  module.classes.push_back(base);

  BcClass child;
  child.class_name_sym_id = 2;
  child.has_superclass_ref = true;
  child.superclass_ref = 1;
  child.method_range_start = 1;
  child.method_range_count = 0;
  module.classes.push_back(child);

  BcMethod method;
  method.selector_sym_id = 3;
  method.owner_dispatch_ref = 0;
  method.signature_blob_id = 0;
  method.entry_code_id = 2;
  method.flags = 1;
  module.methods.push_back(method);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back({Opcode::Send,
                                 {{1, false},
                                  {0, false},
                                  {3, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 1;
  body.instructions.push_back({Opcode::LoadK, {{0, false}, {2, false}}});
  body.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body};

  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 1;

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(exec.ok(), "multi-segment superclass dispatch failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 41,
         "multi-segment superclass ref should resolve through dispatch");
}

void test_manual_send_cache_receiver_class_guard() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"A", "B", "value", "+", "=="};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant zero;
  zero.kind = ConstantKind::Integer;
  zero.int_value = 0;
  module.const_pool.push_back(zero);

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  module.const_pool.push_back(two);

  BcClass class_a;
  class_a.class_name_sym_id = 0;
  class_a.method_range_start = 0;
  class_a.method_range_count = 1;
  module.classes.push_back(class_a);

  BcClass class_b;
  class_b.class_name_sym_id = 1;
  class_b.method_range_start = 1;
  class_b.method_range_count = 1;
  module.classes.push_back(class_b);

  BcMethod method_a;
  method_a.selector_sym_id = 2;
  method_a.owner_dispatch_ref = 0;
  method_a.signature_blob_id = 0;
  method_a.entry_code_id = 2;
  method_a.flags = 1;
  module.methods.push_back(method_a);

  BcMethod method_b;
  method_b.selector_sym_id = 2;
  method_b.owner_dispatch_ref = 1;
  method_b.signature_blob_id = 0;
  method_b.entry_code_id = 3;
  method_b.flags = 1;
  module.methods.push_back(method_b);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 6;
  caller.instructions.push_back({Opcode::LoadK, {{3, false}, {1, false}}});
  caller.instructions.push_back({Opcode::LoadK, {{4, false}, {2, false}}});
  caller.instructions.push_back({Opcode::Send,
                                 {{2, false},
                                  {0, false},
                                  {2, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Send,
                                 {{3, false},
                                  {3, false},
                                  {3, false},
                                  {1, false},
                                  {4, false},
                                  {0, false},
                                  {-1, true},
                                  {1, false}}});
  caller.instructions.push_back({Opcode::Send,
                                 {{5, false},
                                  {3, false},
                                  {4, false},
                                  {1, false},
                                  {4, false},
                                  {0, false},
                                  {-1, true},
                                  {2, false}}});
  caller.instructions.push_back(
      {Opcode::JumpIfFalse, {{5, false}, {8, false}}});
  caller.instructions.push_back({Opcode::Move, {{0, false}, {1, false}}});
  caller.instructions.push_back({Opcode::Jump, {{2, false}}});
  caller.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode body_a;
  body_a.code_id = 2;
  body_a.kind = CodeKind::Method;
  body_a.reg_count = 1;
  body_a.instructions.push_back({Opcode::LoadK, {{0, false}, {2, false}}});
  body_a.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode body_b;
  body_b.code_id = 3;
  body_b.kind = CodeKind::Method;
  body_b.reg_count = 1;
  body_b.instructions.push_back({Opcode::LoadK, {{0, false}, {3, false}}});
  body_b.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body_a, body_b};

  auto a = std::make_shared<amber::runtime::InstanceValue>();
  a->class_index = 0;
  auto b = std::make_shared<amber::runtime::InstanceValue>();
  b->class_index = 1;

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1,
      {amber::runtime::Value::instance(a), amber::runtime::Value::instance(b)});
  expect(exec.ok(), "send cache receiver-class guard execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 2,
         "send cache should miss when receiver class changes");
}

void test_manual_ivar_cache_shape_guard() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"mass"};

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 3;
  code.instructions.push_back({Opcode::LoadSelf, {{1, false}}});
  code.instructions.push_back(
      {Opcode::LoadIvar, {{2, false}, {1, false}, {0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::StoreIvar, {{1, false}, {0, false}, {0, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::LoadIvar, {{2, false}, {1, false}, {0, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(code);

  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 0;

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::integer(23)},
      amber::runtime::Value::instance(instance));
  expect(exec.ok(), "ivar cache shape guard execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 23,
         "ivar cache should miss after a shape-changing store");
}

void test_runtime_ivar_shape_slot_transition_stability() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"mass", "charge"};

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 4;
  code.instructions.push_back({Opcode::LoadSelf, {{2, false}}});
  code.instructions.push_back(
      {Opcode::StoreIvar, {{2, false}, {0, false}, {0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::StoreIvar, {{2, false}, {1, false}, {1, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::LoadIvar, {{3, false}, {2, false}, {0, false}, {2, false}}});
  code.instructions.push_back({Opcode::Return, {{3, false}}});
  module.code_objects.push_back(code);

  amber::runtime::RuntimeWorld world(module);
  auto first = std::make_shared<amber::runtime::InstanceValue>();
  first->class_index = 0;
  const amber::runtime::ExecutionResult first_exec = world.execute(
      1,
      {amber::runtime::Value::integer(7), amber::runtime::Value::integer(11)},
      amber::runtime::Value::instance(first));
  expect(first_exec.ok(), "first shape transition execution failed");
  expect(first_exec.value.is_integer() && first_exec.value.as_integer() == 7,
         "shape transition test should read stored mass");
  expect(first->header.shape != nullptr && !first->header.shape->dead,
         "instance should have a live runtime shape");
  expect(first->header.shape->slot_names.size() == 2,
         "shape should allocate two ivar slots");
  expect(first->header.shape->ivar_slots.at("mass") == 0,
         "mass should occupy first slot");
  expect(first->header.shape->ivar_slots.at("charge") == 1,
         "charge should occupy second slot");
  expect(first->ivar_storage.size() == 2, "ivar storage should be slot-backed");
  expect(first->ivar_storage[0].is_integer() &&
             first->ivar_storage[0].as_integer() == 7,
         "mass slot should contain first argument");
  expect(first->ivar_storage[1].is_integer() &&
             first->ivar_storage[1].as_integer() == 11,
         "charge slot should contain second argument");
  expect(first->ivar_shape_version == first->header.shape->shape_version,
         "legacy shape version mirror should track shape descriptor");
  const std::uint64_t final_shape_id = first->header.shape->shape_id;
  const std::uint64_t final_shape_version = first->header.shape->shape_version;

  auto second = std::make_shared<amber::runtime::InstanceValue>();
  second->class_index = 0;
  const amber::runtime::ExecutionResult second_exec = world.execute(
      1, {amber::runtime::Value::integer(5), amber::runtime::Value::integer(6)},
      amber::runtime::Value::instance(second));
  expect(second_exec.ok(), "second shape transition execution failed");
  expect(second->header.shape != nullptr &&
             second->header.shape->shape_id == final_shape_id,
         "same ivar growth path should reuse the same final shape");
  expect(second->header.shape->shape_version == final_shape_version,
         "reused shape should keep a stable shape version");

  const amber::runtime::ExecutionResult update_exec = world.execute(
      1,
      {amber::runtime::Value::integer(13), amber::runtime::Value::integer(17)},
      amber::runtime::Value::instance(first));
  expect(update_exec.ok(), "existing-slot store execution failed");
  expect(first->header.shape->shape_id == final_shape_id,
         "storing existing ivars should not transition shape");
  expect(first->ivar_storage[0].is_integer() &&
             first->ivar_storage[0].as_integer() == 13,
         "existing mass slot should be overwritten in place");
  expect(first->ivars.at("charge").is_integer() &&
             first->ivars.at("charge").as_integer() == 17,
         "legacy ivar map mirror should stay synchronized");
}

void test_runtime_dead_shape_rejects_ivar_access() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"mass"};

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 2;
  code.instructions.push_back({Opcode::LoadSelf, {{0, false}}});
  code.instructions.push_back(
      {Opcode::LoadIvar, {{1, false}, {0, false}, {0, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects.push_back(code);

  auto dead_shape = std::make_shared<amber::runtime::ShapeDescriptor>();
  dead_shape->dead = true;
  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->header.shape = dead_shape;
  instance->header.flags = amber::runtime::kObjectFlagDead;

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {}, amber::runtime::Value::instance(instance));
  expect(!exec.ok(), "dead-shape ivar load should fail");
  expect(exec.fault.has_value() &&
             exec.fault->error_name == "UseAfterFreeError",
         "dead-shape ivar load should report UseAfterFreeError");
}

void test_runtime_heap_worker_arena_headers() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value list = amber::runtime::Value::null();
  amber::runtime::Value tuple = amber::runtime::Value::null();
  amber::runtime::Value map = amber::runtime::Value::null();
  std::shared_ptr<amber::runtime::InstanceValue> instance;
  std::shared_ptr<amber::runtime::ClosureValue> closure;
  {
    amber::runtime::RuntimeWorkerScope worker(7);
    list = heap.make_list_value({amber::runtime::Value::integer(1)});
    tuple = heap.make_tuple_value({amber::runtime::Value::integer(2)});
    map = heap.make_symbol_map_value({{0, amber::runtime::Value::integer(3)}},
                                     true);
    instance = heap.make_instance_value(4);
    closure = heap.make_closure_value();
  }

  const std::shared_ptr<amber::runtime::ListValue> list_ptr = list.as_list();
  expect(list_ptr->header.allocation_id != 0,
         "list should carry heap allocation id");
  expect(list_ptr->header.arena_worker_id == 7,
         "list should record owner arena worker");
  expect(tuple.as_tuple()->header.arena_worker_id == 7,
         "tuple should record owner arena worker");
  expect(map.as_map()->header.owner.kind ==
             amber::runtime::OwnerTokenKind::Shareable,
         "frozen map should remain shareable");
  expect(instance->header.kind == amber::runtime::HeapObjectKind::Instance &&
             instance->header.class_index == 4,
         "allocator should initialize instance header");
  expect(closure->header.kind == amber::runtime::HeapObjectKind::Closure,
         "allocator should initialize closure header");

  const amber::runtime::RuntimeHeapStats stats = heap.stats();
  const amber::runtime::RuntimeArenaStats *arena = arena_stats_for(stats, 7);
  expect(arena != nullptr, "worker arena stats should exist");
  expect(arena->allocations == 5 && arena->live_objects == 5,
         "worker arena should count live allocations");
  expect(stats.instance_allocations == 1 && stats.array_allocations == 2 &&
             stats.map_allocations == 1 && stats.closure_allocations == 1,
         "heap stats should split allocations by runtime object family");
}

void test_runtime_heap_remote_free_queue_drains_on_owner() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value value = amber::runtime::Value::null();
  {
    amber::runtime::RuntimeWorkerScope owner(1);
    value = heap.make_list_value({amber::runtime::Value::integer(9)});
  }

  {
    amber::runtime::RuntimeWorkerScope other_worker(2);
    value = amber::runtime::Value::null();
  }

  amber::runtime::RuntimeHeapStats queued = heap.stats();
  expect(queued.live_objects == 1, "remote free should retain object memory");
  expect(queued.remote_frees_queued == 1,
         "remote free should be queued for owner arena");
  expect(queued.remote_queue_depth == 1,
         "remote queue depth should track queued free");
  const amber::runtime::RuntimeArenaStats *owner_arena =
      arena_stats_for(queued, 1);
  expect(owner_arena != nullptr && owner_arena->remote_queue_depth == 1,
         "owner arena should own the remote-free queue");

  const std::uint64_t drained = heap.drain_remote_frees(1);
  expect(drained == 1, "owner drain should free one queued object");
  const amber::runtime::RuntimeHeapStats drained_stats = heap.stats();
  expect(drained_stats.live_objects == 0,
         "drained remote free should release memory");
  expect(drained_stats.remote_frees_drained == 1,
         "remote drain count should be recorded");
  expect(drained_stats.remote_queue_depth == 0,
         "remote queue should be empty after drain");
}

void test_runtime_heap_allocation_heavy_smoke() {
  amber::runtime::RuntimeHeap heap;
  {
    amber::runtime::RuntimeWorkerScope worker(3);
    std::vector<amber::runtime::Value> values;
    values.reserve(4096);
    for (std::int64_t i = 0; i < 4096; ++i) {
      values.push_back(
          heap.make_list_value({amber::runtime::Value::integer(i),
                                amber::runtime::Value::integer(i + 1)}));
    }
    const amber::runtime::RuntimeHeapStats live_stats = heap.stats();
    expect(live_stats.allocations == 4096 && live_stats.live_objects == 4096,
           "allocation-heavy smoke should retain all live lists");
    values.clear();
  }

  const amber::runtime::RuntimeHeapStats stats = heap.stats();
  expect(stats.allocations == 4096 && stats.live_objects == 0,
         "allocation-heavy smoke should free all local lists");
  expect(stats.local_frees == 4096,
         "allocation-heavy smoke should use local arena frees");
}

void test_runtime_world_heap_tracks_vm_allocations() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"k"};

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 8;
  code.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::MakeList, {{2, false}, {1, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::MakeTuple, {{3, false}, {1, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::MakeMap, {{4, false}, {1, false}, {0, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::MakeClosure,
       {{5, false}, {2, false}, {1, false}, {0, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::Call,
       {{6, false}, {0, false}, {0, false}, {0, false}, {-1, true}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode closure_code;
  closure_code.code_id = 2;
  closure_code.kind = CodeKind::Block;
  closure_code.reg_count = 1;
  closure_code.instructions.push_back({Opcode::LoadNull, {{0, false}}});
  closure_code.instructions.push_back({Opcode::Return, {{0, false}}});

  module.classes.push_back(BcClass{});
  module.code_objects = {code, closure_code};

  amber::runtime::RuntimeWorld world(module);
  {
    amber::runtime::RuntimeWorkerScope worker(11);
    const amber::runtime::ExecutionResult exec =
        world.execute(1, {amber::runtime::Value::class_object(0)});
    expect(exec.ok(), "VM allocation probe should execute");
    expect(exec.value.is_list(), "VM allocation probe should return list");
    expect(exec.value.as_list()->header.arena_worker_id == 11,
           "VM list should be allocated in current worker arena");

    const amber::runtime::RuntimeHeapStats stats = world.heap_stats();
    const amber::runtime::RuntimeArenaStats *arena = arena_stats_for(stats, 11);
    expect(arena != nullptr && arena->allocations == 5,
           "VM should allocate list/tuple/map/closure/instance through heap");
    expect(stats.instance_allocations == 1 && stats.array_allocations == 2 &&
               stats.map_allocations == 1 && stats.closure_allocations == 1,
           "world heap stats should include all VM heap object families");
  }

  expect(world.heap_stats().live_objects == 0,
         "VM allocation probe should release returned value after scope");
}

void test_runtime_lifecycle_destroy_opcode_is_idempotent() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "value", "destroy!", "mass"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant answer;
  answer.kind = ConstantKind::Integer;
  answer.int_value = 42;
  module.const_pool.push_back(answer);

  Constant destroyed_marker;
  destroyed_marker.kind = ConstantKind::Integer;
  destroyed_marker.int_value = 99;
  module.const_pool.push_back(destroyed_marker);

  BcClass box;
  box.class_name_sym_id = 0;
  box.method_range_start = 0;
  box.method_range_count = 2;
  module.classes.push_back(box);

  BcMethod value_method;
  value_method.selector_sym_id = 1;
  value_method.owner_dispatch_ref = 0;
  value_method.signature_blob_id = 0;
  value_method.entry_code_id = 3;
  value_method.flags = 1;
  module.methods.push_back(value_method);

  BcMethod destroy_method;
  destroy_method.selector_sym_id = 2;
  destroy_method.owner_dispatch_ref = 0;
  destroy_method.signature_blob_id = 0;
  destroy_method.entry_code_id = 4;
  destroy_method.flags = 1;
  module.methods.push_back(destroy_method);

  BcCode destroy;
  destroy.code_id = 1;
  destroy.kind = CodeKind::Method;
  destroy.reg_count = 2;
  destroy.instructions.push_back(
      {Opcode::ObjDestroy, {{1, false}, {0, false}}});
  destroy.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode send_after_destroy;
  send_after_destroy.code_id = 2;
  send_after_destroy.kind = CodeKind::Method;
  send_after_destroy.reg_count = 2;
  send_after_destroy.instructions.push_back({Opcode::Send,
                                             {{1, false},
                                              {0, false},
                                              {1, false},
                                              {0, false},
                                              {0, false},
                                              {-1, true},
                                              {0, false}}});
  send_after_destroy.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode body;
  body.code_id = 3;
  body.kind = CodeKind::Method;
  body.reg_count = 1;
  body.instructions.push_back({Opcode::LoadK, {{0, false}, {1, false}}});
  body.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode destroy_body;
  destroy_body.code_id = 4;
  destroy_body.kind = CodeKind::Method;
  destroy_body.reg_count = 3;
  destroy_body.instructions.push_back({Opcode::LoadSelf, {{0, false}}});
  destroy_body.instructions.push_back(
      {Opcode::LoadK, {{1, false}, {2, false}}});
  destroy_body.instructions.push_back(
      {Opcode::StoreIvar, {{0, false}, {3, false}, {1, false}, {0, false}}});
  destroy_body.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {destroy, send_after_destroy, body, destroy_body};

  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->header.class_index = 0;

  const amber::runtime::ExecutionResult first = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(first.ok(), "OBJ_DESTROY first execution should succeed");
  expect(first.value.is_bool() && first.value.as_bool(),
         "OBJ_DESTROY should return true for the first destroy");
  expect(instance->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Destroyed,
         "OBJ_DESTROY should transition object to destroyed state");
  expect((instance->header.flags & amber::runtime::kObjectFlagDestroyed) != 0U,
         "OBJ_DESTROY should set destroyed flag");
  expect(instance->ivars.find("mass") != instance->ivars.end() &&
             instance->ivars["mass"].is_integer() &&
             instance->ivars["mass"].as_integer() == 99,
         "OBJ_DESTROY should run class-local destroy! body before tombstoning");

  const amber::runtime::ExecutionResult second = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(second.ok(), "OBJ_DESTROY second execution should succeed");
  expect(second.value.is_bool() && !second.value.as_bool(),
         "OBJ_DESTROY should return false after object is already destroyed");

  const amber::runtime::ExecutionResult send = amber::runtime::execute_code(
      module, 2, {amber::runtime::Value::instance(instance)});
  expect(!send.ok(), "ordinary send on destroyed object should fail");
  expect(
      send.fault.has_value() &&
          send.fault->error_name == "DestroyedAccessError",
      "ordinary send on destroyed object should report DestroyedAccessError");
}

void test_runtime_lifecycle_dealloc_opcode_tombstones_instance_payload() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"mass"};

  BcCode dealloc;
  dealloc.code_id = 1;
  dealloc.kind = CodeKind::Method;
  dealloc.reg_count = 2;
  dealloc.instructions.push_back(
      {Opcode::ObjDealloc, {{1, false}, {0, false}}});
  dealloc.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode load_ivar;
  load_ivar.code_id = 2;
  load_ivar.kind = CodeKind::Method;
  load_ivar.reg_count = 2;
  load_ivar.instructions.push_back(
      {Opcode::LoadIvar, {{1, false}, {0, false}, {0, false}, {0, false}}});
  load_ivar.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {dealloc, load_ivar};

  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->header.class_index = 0;
  instance->ivar_storage.push_back(amber::runtime::Value::integer(7));
  instance->ivars["mass"] = amber::runtime::Value::integer(7);

  const amber::runtime::ExecutionResult first = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(first.ok(), "OBJ_DEALLOC first execution should succeed");
  expect(first.value.is_bool() && first.value.as_bool(),
         "OBJ_DEALLOC should return true for first deallocation");
  expect(instance->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Deallocated,
         "OBJ_DEALLOC should transition object to deallocated state");
  expect((instance->header.flags & amber::runtime::kObjectFlagDead) != 0U,
         "OBJ_DEALLOC should set dead flag");
  expect(instance->header.shape != nullptr && instance->header.shape->dead,
         "OBJ_DEALLOC should rewrite shape to DeadShape");
  expect(instance->ivar_storage.empty() && instance->ivars.empty(),
         "OBJ_DEALLOC should release instance ivar payload");

  const amber::runtime::ExecutionResult second = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(second.ok(), "OBJ_DEALLOC second execution should succeed");
  expect(second.value.is_bool() && !second.value.as_bool(),
         "OBJ_DEALLOC should return false for already deallocated object");

  const amber::runtime::ExecutionResult load = amber::runtime::execute_code(
      module, 2, {amber::runtime::Value::instance(instance)});
  expect(!load.ok(), "ivar access on deallocated object should fail");
  expect(load.fault.has_value() &&
             load.fault->error_name == "UseAfterFreeError",
         "ivar access on deallocated object should report UseAfterFreeError");
}

void test_runtime_lifecycle_dealloc_clears_collection_payload() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"empty?"};

  BcCode dealloc;
  dealloc.code_id = 1;
  dealloc.kind = CodeKind::Method;
  dealloc.reg_count = 2;
  dealloc.instructions.push_back(
      {Opcode::ObjDealloc, {{1, false}, {0, false}}});
  dealloc.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode builtin_send;
  builtin_send.code_id = 2;
  builtin_send.kind = CodeKind::Method;
  builtin_send.reg_count = 2;
  builtin_send.instructions.push_back({Opcode::Send,
                                       {{1, false},
                                        {0, false},
                                        {0, false},
                                        {0, false},
                                        {0, false},
                                        {-1, true},
                                        {0, false}}});
  builtin_send.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {dealloc, builtin_send};

  amber::runtime::Value list = amber::runtime::make_list_value(
      {amber::runtime::Value::integer(1), amber::runtime::Value::integer(2)});
  const std::shared_ptr<amber::runtime::ListValue> list_ptr = list.as_list();

  const amber::runtime::ExecutionResult dealloc_result =
      amber::runtime::execute_code(module, 1, {list});
  expect(dealloc_result.ok(), "list OBJ_DEALLOC should succeed");
  expect(dealloc_result.value.is_bool() && dealloc_result.value.as_bool(),
         "list OBJ_DEALLOC should return true");
  expect(list_ptr->items.empty(), "list OBJ_DEALLOC should clear items");
  expect(list_ptr->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Deallocated,
         "list OBJ_DEALLOC should mark list deallocated");

  const amber::runtime::ExecutionResult send =
      amber::runtime::execute_code(module, 2, {list});
  expect(!send.ok(), "builtin send on deallocated list should fail");
  expect(send.fault.has_value() &&
             send.fault->error_name == "UseAfterFreeError",
         "builtin send on deallocated list should report UseAfterFreeError");
}

void test_runtime_world_define_method_invalidates_send_cache() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "value"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  module.const_pool.push_back(two);

  BcClass box;
  box.class_name_sym_id = 0;
  box.method_range_start = 0;
  box.method_range_count = 1;
  module.classes.push_back(box);

  BcMethod original;
  original.selector_sym_id = 1;
  original.owner_dispatch_ref = 0;
  original.signature_blob_id = 0;
  original.entry_code_id = 2;
  original.flags = 1;
  module.methods.push_back(original);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back({Opcode::Send,
                                 {{1, false},
                                  {0, false},
                                  {1, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode body_one;
  body_one.code_id = 2;
  body_one.kind = CodeKind::Method;
  body_one.reg_count = 1;
  body_one.instructions.push_back({Opcode::LoadK, {{0, false}, {1, false}}});
  body_one.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode body_two;
  body_two.code_id = 3;
  body_two.kind = CodeKind::Method;
  body_two.reg_count = 1;
  body_two.instructions.push_back({Opcode::LoadK, {{0, false}, {2, false}}});
  body_two.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body_one, body_two};

  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  amber::runtime::RuntimeWorld world(module);
  expect(world.method_table_size(
             0, amber::runtime::MethodTableSide::Instance) == 1,
         "instance method table should include emitted method");
  expect(world.method_table_size(0, amber::runtime::MethodTableSide::Class) ==
             0,
         "class method table should be empty for instance-only owner");

  const amber::runtime::ExecutionResult before =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(before.ok(), "define_method preflight send failed");
  expect(before.value.is_integer() && before.value.as_integer() == 1,
         "initial method should return original value");

  const std::uint64_t epoch_before = world.world_epoch();
  const std::uint64_t version_before = world.method_version(0);
  BcMethod replacement = original;
  replacement.entry_code_id = 3;
  const amber::runtime::ExecutionResult define_result =
      world.define_instance_method(0, replacement);
  expect(define_result.ok(), "runtime define_instance_method failed");
  expect(world.world_epoch() == epoch_before + 1,
         "define_method should bump world epoch");
  expect(world.method_version(0) == version_before + 1,
         "define_method should bump owner method version");
  expect(world.method_table_size(
             0, amber::runtime::MethodTableSide::Instance) == 1,
         "method replacement should keep a stable method-table slot");

  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(after.ok(), "define_method post-mutation send failed");
  expect(after.value.is_integer() && after.value.as_integer() == 2,
         "send cache should invalidate after method replacement");
}

void test_runtime_world_include_invalidates_send_cache() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "Older", "Newer", "label"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant older_path;
  older_path.kind = ConstantKind::Path;
  older_path.items = {1};
  module.const_pool.push_back(older_path);

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  module.const_pool.push_back(two);

  BcClass box;
  box.class_name_sym_id = 0;
  box.method_range_start = 2;
  box.method_range_count = 0;
  box.direct_include_refs.push_back(1);
  module.classes.push_back(box);

  BcClass older;
  older.class_name_sym_id = 1;
  older.method_range_start = 0;
  older.method_range_count = 1;
  older.flags = kClassFlagMixin;
  module.classes.push_back(older);

  BcClass newer;
  newer.class_name_sym_id = 2;
  newer.method_range_start = 1;
  newer.method_range_count = 1;
  newer.flags = kClassFlagMixin;
  module.classes.push_back(newer);

  BcMethod older_method;
  older_method.selector_sym_id = 3;
  older_method.owner_dispatch_ref = 1;
  older_method.signature_blob_id = 0;
  older_method.entry_code_id = 2;
  older_method.flags = 1;
  module.methods.push_back(older_method);

  BcMethod newer_method = older_method;
  newer_method.owner_dispatch_ref = 2;
  newer_method.entry_code_id = 3;
  module.methods.push_back(newer_method);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back({Opcode::Send,
                                 {{1, false},
                                  {0, false},
                                  {3, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode old_body;
  old_body.code_id = 2;
  old_body.kind = CodeKind::Method;
  old_body.reg_count = 1;
  old_body.instructions.push_back({Opcode::LoadK, {{0, false}, {2, false}}});
  old_body.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode new_body;
  new_body.code_id = 3;
  new_body.kind = CodeKind::Method;
  new_body.reg_count = 1;
  new_body.instructions.push_back({Opcode::LoadK, {{0, false}, {3, false}}});
  new_body.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, old_body, new_body};

  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  amber::runtime::RuntimeWorld world(module);

  const amber::runtime::ExecutionResult before =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(before.ok(), "include preflight send failed");
  expect(before.value.is_integer() && before.value.as_integer() == 1,
         "static included mixin should answer before late include");

  const amber::runtime::ExecutionResult include_result =
      world.include_mixin(0, 2);
  expect(include_result.ok(), "runtime include_mixin failed");

  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(after.ok(), "include post-mutation send failed");
  expect(after.value.is_integer() && after.value.as_integer() == 2,
         "late include should dominate and invalidate cached dispatch");
}

void test_manual_pattern_deconstruct_protocol_sequence() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Pair", "deconstruct"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  Constant nine;
  nine.kind = ConstantKind::Integer;
  nine.int_value = 9;
  module.const_pool.push_back(nine);

  Constant zero;
  zero.kind = ConstantKind::Integer;
  zero.int_value = 0;
  module.const_pool.push_back(zero);

  BcClass pair;
  pair.class_name_sym_id = 0;
  pair.method_range_start = 0;
  pair.method_range_count = 1;
  module.classes.push_back(pair);

  BcMethod deconstruct;
  deconstruct.selector_sym_id = 1;
  deconstruct.owner_dispatch_ref = 0;
  deconstruct.signature_blob_id = 0;
  deconstruct.entry_code_id = 2;
  deconstruct.flags = 1;
  module.methods.push_back(deconstruct);

  BcCode probe;
  probe.code_id = 1;
  probe.kind = CodeKind::Method;
  probe.reg_count = 5;
  probe.instructions.push_back(
      {Opcode::PPrepSeq, {{1, false}, {0, false}, {0, false}, {6, false}}});
  probe.instructions.push_back(
      {Opcode::PCheckLenEq, {{1, false}, {2, false}, {6, false}}});
  probe.instructions.push_back(
      {Opcode::PGetIndex, {{2, false}, {1, false}, {0, false}}});
  probe.instructions.push_back(
      {Opcode::PCheckEq, {{2, false}, {1, false}, {6, false}}});
  probe.instructions.push_back(
      {Opcode::PGetIndex, {{3, false}, {1, false}, {1, false}}});
  probe.instructions.push_back({Opcode::Return, {{3, false}}});
  probe.instructions.push_back({Opcode::LoadK, {{4, false}, {3, false}}});
  probe.instructions.push_back({Opcode::Return, {{4, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 4;
  body.instructions.push_back({Opcode::LoadK, {{0, false}, {1, false}}});
  body.instructions.push_back({Opcode::LoadK, {{1, false}, {2, false}}});
  body.instructions.push_back(
      {Opcode::MakeList, {{2, false}, {0, false}, {2, false}}});
  body.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects = {probe, body};

  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(exec.ok(), "sequence deconstruct protocol execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 9,
         "P_PREP_SEQ should use object deconstruct protocol");
}

void test_manual_pattern_deconstruct_protocol_map() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Payload", "deconstruct_keys", "a", "keys"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant keyset;
  keyset.kind = ConstantKind::KeySet;
  keyset.items = {2};
  module.const_pool.push_back(keyset);

  Constant seven;
  seven.kind = ConstantKind::Integer;
  seven.int_value = 7;
  module.const_pool.push_back(seven);

  Constant zero;
  zero.kind = ConstantKind::Integer;
  zero.int_value = 0;
  module.const_pool.push_back(zero);

  BcClass payload;
  payload.class_name_sym_id = 0;
  payload.method_range_start = 0;
  payload.method_range_count = 1;
  module.classes.push_back(payload);

  BcMethod deconstruct_keys;
  deconstruct_keys.selector_sym_id = 1;
  deconstruct_keys.owner_dispatch_ref = 0;
  deconstruct_keys.signature_blob_id = 0;
  deconstruct_keys.entry_code_id = 2;
  deconstruct_keys.flags = 1;
  deconstruct_keys.params.push_back({3, 0, 0});
  module.methods.push_back(deconstruct_keys);

  BcCode probe;
  probe.code_id = 1;
  probe.kind = CodeKind::Method;
  probe.reg_count = 4;
  probe.instructions.push_back(
      {Opcode::PPrepMap,
       {{1, false}, {0, false}, {1, false}, {0, false}, {4, false}}});
  probe.instructions.push_back(
      {Opcode::PHasKey, {{1, false}, {2, false}, {4, false}}});
  probe.instructions.push_back(
      {Opcode::PGetKey, {{2, false}, {1, false}, {2, false}}});
  probe.instructions.push_back({Opcode::Return, {{2, false}}});
  probe.instructions.push_back({Opcode::LoadK, {{3, false}, {3, false}}});
  probe.instructions.push_back({Opcode::Return, {{3, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 3;
  body.instructions.push_back({Opcode::LoadK, {{1, false}, {2, false}}});
  body.instructions.push_back(
      {Opcode::MakeMap, {{2, false}, {1, false}, {2, false}, {1, false}}});
  body.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects = {probe, body};

  auto instance = std::make_shared<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(exec.ok(), "map deconstruct_keys protocol execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 7,
         "P_PREP_MAP should use object deconstruct_keys protocol");
}

void test_manual_raise_handler_table_recovers() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Boom"};

  Constant recovered;
  recovered.kind = ConstantKind::Integer;
  recovered.int_value = 42;
  module.const_pool.push_back(recovered);

  BcClass boom;
  boom.class_name_sym_id = 0;
  module.classes.push_back(boom);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 1;
  code.instructions.push_back({Opcode::Raise, {{0, false}}});
  code.instructions.push_back({Opcode::Return, {{0, false}}});
  code.handler_table.push_back({0, 1, 1, 2, 0});

  BcCode handler;
  handler.code_id = 2;
  handler.kind = CodeKind::Rescue;
  handler.reg_count = 2;
  handler.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  handler.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {code, handler};

  auto exception = std::make_shared<amber::runtime::InstanceValue>();
  exception->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(exception)});
  expect(exec.ok(), "RAISE handler execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 42,
         "rescue handler should recover with its return value");
}

void test_manual_raise_unwinds_closure_to_outer_handler() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Boom"};

  Constant recovered;
  recovered.kind = ConstantKind::Integer;
  recovered.int_value = 99;
  module.const_pool.push_back(recovered);

  BcClass boom;
  boom.class_name_sym_id = 0;
  module.classes.push_back(boom);

  BcCode outer;
  outer.code_id = 1;
  outer.kind = CodeKind::Method;
  outer.reg_count = 3;
  outer.instructions.push_back(
      {Opcode::MakeClosure, {{1, false}, {2, false}, {0, false}}});
  outer.instructions.push_back({Opcode::Call,
                                {{2, false},
                                 {1, false},
                                 {1, false},
                                 {0, false},
                                 {0, false},
                                 {-1, true},
                                 {0, false}}});
  outer.instructions.push_back({Opcode::Return, {{2, false}}});
  outer.instructions.push_back({Opcode::Return, {{0, false}}});
  outer.handler_table.push_back({1, 2, 3, 3, 0});

  BcCode inner;
  inner.code_id = 2;
  inner.kind = CodeKind::Block;
  inner.reg_count = 1;
  inner.instructions.push_back({Opcode::Raise, {{0, false}}});
  inner.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode handler;
  handler.code_id = 3;
  handler.kind = CodeKind::Rescue;
  handler.reg_count = 2;
  handler.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  handler.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {outer, inner, handler};

  auto exception = std::make_shared<amber::runtime::InstanceValue>();
  exception->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(exception)});
  expect(exec.ok(), "closure RAISE unwind execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 99,
         "outer handler should catch exception from active closure call");
}

void test_manual_raise_unwinds_method_send_to_outer_handler() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "explode"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant recovered;
  recovered.kind = ConstantKind::Integer;
  recovered.int_value = 17;
  module.const_pool.push_back(recovered);

  BcClass box;
  box.class_name_sym_id = 0;
  box.method_range_start = 0;
  box.method_range_count = 1;
  module.classes.push_back(box);

  BcMethod explode;
  explode.selector_sym_id = 1;
  explode.owner_dispatch_ref = 0;
  explode.signature_blob_id = 0;
  explode.entry_code_id = 2;
  explode.flags = 1;
  module.methods.push_back(explode);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 3;
  caller.instructions.push_back({Opcode::Send,
                                 {{2, false},
                                  {0, false},
                                  {1, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{2, false}}});
  caller.instructions.push_back({Opcode::Return, {{0, false}}});
  caller.handler_table.push_back({0, 1, 2, 3, 0});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 1;
  body.instructions.push_back({Opcode::LoadSelf, {{0, false}}});
  body.instructions.push_back({Opcode::Raise, {{0, false}}});
  body.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode handler;
  handler.code_id = 3;
  handler.kind = CodeKind::Rescue;
  handler.reg_count = 2;
  handler.instructions.push_back({Opcode::LoadK, {{1, false}, {1, false}}});
  handler.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {caller, body, handler};

  auto receiver = std::make_shared<amber::runtime::InstanceValue>();
  receiver->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(receiver)});
  expect(exec.ok(), "method SEND RAISE unwind execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 17,
         "outer handler should catch exception from active method SEND");
}

void test_manual_raise_unhandled_fault_trace() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Boom"};

  BcClass boom;
  boom.class_name_sym_id = 0;
  module.classes.push_back(boom);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 1;
  code.instructions.push_back({Opcode::Raise, {{0, false}}});
  code.source_spans.push_back({0, 1, {"raise.am", {3, 5, 10}, {3, 15, 20}}});
  module.code_objects.push_back(code);

  auto exception = std::make_shared<amber::runtime::InstanceValue>();
  exception->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(exception)});
  expect(!exec.ok(), "unhandled RAISE should fail");
  expect(exec.fault.has_value() && exec.fault->error_name == "Boom",
         "unhandled RAISE should use exception class name");
  expect(!exec.fault->trace.empty() && exec.fault->trace[0].code_id == 1 &&
             exec.fault->trace[0].pc == 0 && exec.fault->trace[0].line == 3,
         "unhandled RAISE should include source trace frame");
  expect(exec.fault->trace_text.find("Boom:") != std::string::npos &&
             exec.fault->trace_text.find("c1:0") != std::string::npos,
         "unhandled RAISE should include human-readable trace text");
}

} // namespace

int main() {
  test_execute_emitted_method();
  test_branching_and_last_result();
  test_manual_closure_call_and_capture();
  test_execute_emitted_send_method();
  test_execute_emitted_compare_method();
  test_execute_emitted_default_method();
  test_execute_emitted_keyword_method();
  test_execute_emitted_block_send();
  test_manual_dynamic_send();
  test_execute_emitted_class_method_send();
  test_execute_emitted_constructor_call();
  test_execute_emitted_constructor_auto_assign();
  test_execute_emitted_constructor_default();
  test_execute_emitted_cvar_store_and_load();
  test_execute_emitted_constructor_cvar_auto_assign();
  test_execute_emitted_superclass_dispatch();
  test_execute_emitted_include_linearization();
  test_execute_emitted_extend_linearization();
  test_execute_emitted_method_missing_instance();
  test_execute_emitted_method_missing_class_side();
  test_method_missing_does_not_recurse();
  test_execute_emitted_case_literal();
  test_execute_emitted_case_pin();
  test_execute_emitted_case_bind();
  test_execute_emitted_case_bang_failure();
  test_execute_emitted_case_const_class();
  test_execute_emitted_case_list_exact();
  test_execute_emitted_pattern_assign_list_rest();
  test_execute_emitted_case_map_rest();
  test_execute_emitted_case_map_strict_null();
  test_execute_emitted_clause_method_dispatch();
  test_manual_make_map();
  test_manual_instance_send_dispatch();
  test_manual_store_and_load_ivar();
  test_manual_store_and_load_cvar();
  test_manual_multi_segment_lookup_const();
  test_manual_multi_segment_superclass_dispatch();
  test_manual_send_cache_receiver_class_guard();
  test_manual_ivar_cache_shape_guard();
  test_runtime_ivar_shape_slot_transition_stability();
  test_runtime_dead_shape_rejects_ivar_access();
  test_runtime_heap_worker_arena_headers();
  test_runtime_heap_remote_free_queue_drains_on_owner();
  test_runtime_heap_allocation_heavy_smoke();
  test_runtime_world_heap_tracks_vm_allocations();
  test_runtime_lifecycle_destroy_opcode_is_idempotent();
  test_runtime_lifecycle_dealloc_opcode_tombstones_instance_payload();
  test_runtime_lifecycle_dealloc_clears_collection_payload();
  test_runtime_world_define_method_invalidates_send_cache();
  test_runtime_world_include_invalidates_send_cache();
  test_manual_pattern_deconstruct_protocol_sequence();
  test_manual_pattern_deconstruct_protocol_map();
  test_manual_raise_handler_table_recovers();
  test_manual_raise_unwinds_closure_to_outer_handler();
  test_manual_raise_unwinds_method_send_to_outer_handler();
  test_manual_raise_unhandled_fault_trace();
  std::cout << "vm_tests: ok\n";
  return 0;
}
