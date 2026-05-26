#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "emitter test failed: " << message << "\n";
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

const amber::bytecode::BcCode *
code_by_id(const amber::bytecode::BcModule &module, std::uint32_t code_id) {
  for (const amber::bytecode::BcCode &code : module.code_objects) {
    if (code.code_id == code_id) {
      return &code;
    }
  }
  return nullptr;
}

const amber::bytecode::BcCode *
code_by_kind(const amber::bytecode::BcModule &module,
             amber::bytecode::CodeKind kind) {
  for (const amber::bytecode::BcCode &code : module.code_objects) {
    if (code.kind == kind) {
      return &code;
    }
  }
  return nullptr;
}

bool contains_opcode(const amber::bytecode::BcCode &code,
                     amber::bytecode::Opcode opcode) {
  for (const amber::bytecode::Instruction &instruction : code.instructions) {
    if (instruction.opcode == opcode) {
      return true;
    }
  }
  return false;
}

bool has_diagnostic_code(const amber::bytecode::EmitResult &result,
                         const std::string &code) {
  for (const amber::lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool contains_symbol(const amber::bytecode::BcModule &module,
                     const std::string &symbol) {
  for (const std::string &value : module.symbols) {
    if (value == symbol) {
      return true;
    }
  }
  return false;
}

std::string path_constant_text(const amber::bytecode::BcModule &module,
                               std::uint32_t ref_id) {
  expect(ref_id < module.const_pool.size(), "path const ref is in range");
  const amber::bytecode::Constant &constant = module.const_pool[ref_id];
  expect(constant.kind == amber::bytecode::ConstantKind::Path,
         "constant is path");
  std::string out;
  for (std::size_t i = 0; i < constant.items.size(); ++i) {
    expect(constant.items[i] < module.symbols.size(), "path symbol in range");
    if (i != 0U) {
      out += ".";
    }
    out += module.symbols[constant.items[i]];
  }
  return out;
}

void test_if_and_round_trip() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def choose(x):\n"
                                                          "  if x > 0:\n"
                                                          "    x\n"
                                                          "  else:\n"
                                                          "    0\n");
  expect(emit_result.module.methods.size() == 1, "expected one method");

  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "choose code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::Send),
         "if method emits SEND");
  expect(contains_opcode(*code, amber::bytecode::Opcode::JumpIfFalse),
         "if method emits JUMP_IF_FALSE");
  expect(contains_opcode(*code, amber::bytecode::Opcode::Return),
         "if method emits RETURN");

  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(emit_result.module);
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(bytes);
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));
}

void test_loop_and_safepoint() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def step(x):\n"
                                                          "  while x < 3:\n"
                                                          "    x = x + 1\n"
                                                          "  x\n");
  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "step code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::Safepoint),
         "loop emits SAFEPOINT");
  expect(!code->safepoint_table.empty(), "loop records safepoint table");
  expect(contains_opcode(*code, amber::bytecode::Opcode::Jump),
         "loop emits back-edge jump");
}

void test_closure_capture_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def offsetter(xs, δ):\n"
              "  xs.map: _1 + δ\n");
  expect(emit_result.module.methods.size() == 1,
         "expected one top-level method");
  expect(emit_result.module.code_objects.size() >= 3,
         "expected module init, method, and block code");

  const amber::bytecode::BcCode *method_code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(method_code != nullptr, "offsetter code exists");
  expect(contains_opcode(*method_code, amber::bytecode::Opcode::MakeClosure),
         "method emits MAKE_CLOSURE");
  expect(contains_opcode(*method_code, amber::bytecode::Opcode::Send),
         "method emits SEND for map");

  bool saw_upval_load = false;
  for (const amber::bytecode::BcCode &code : emit_result.module.code_objects) {
    if (code.code_id == method_code->code_id) {
      continue;
    }
    if (contains_opcode(code, amber::bytecode::Opcode::LoadUpval)) {
      saw_upval_load = true;
      break;
    }
  }
  expect(saw_upval_load, "closure body emits LOAD_UPVAL");
}

void test_default_thunk_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def configure(x, y = x + 1):\n"
              "  y\n");
  expect(emit_result.module.methods.size() == 1,
         "expected one method with default thunk");
  const amber::bytecode::BcMethod &method = emit_result.module.methods[0];
  expect(method.params.size() == 2, "expected two serialized params");
  expect((method.params[0].flags &
          amber::bytecode::kMethodParamFlagHasDefault) == 0U,
         "required param carries no default flag");
  expect((method.params[1].flags &
          amber::bytecode::kMethodParamFlagHasDefault) != 0U,
         "defaulted param carries default flag");
  expect(method.default_thunk_ids.size() == 1, "expected one default thunk id");

  const amber::bytecode::BcCode *thunk_code =
      code_by_id(emit_result.module, method.default_thunk_ids[0]);
  expect(thunk_code != nullptr, "default thunk code exists");
  expect(thunk_code->kind == amber::bytecode::CodeKind::DefaultThunk,
         "default thunk code kind");
  expect(code_by_kind(emit_result.module,
                      amber::bytecode::CodeKind::DefaultThunk) == thunk_code,
         "default thunk kind lookup");
  expect(contains_opcode(*thunk_code, amber::bytecode::Opcode::Send),
         "default thunk emits SEND");
  expect(contains_opcode(*thunk_code, amber::bytecode::Opcode::Return),
         "default thunk emits RETURN");
}

void test_keyword_param_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Config:\n"
              "  class_method def build(x, α:, β: 2):\n"
              "    β\n");
  expect(emit_result.module.methods.size() == 1,
         "expected one keyword-bearing method");
  const amber::bytecode::BcMethod &method = emit_result.module.methods[0];
  expect(method.params.size() == 3, "serialized keyword params count");
  expect((method.params[0].flags & amber::bytecode::kMethodParamFlagKeyword) ==
             0U,
         "positional param has no keyword flag");
  expect((method.params[1].flags & amber::bytecode::kMethodParamFlagKeyword) !=
             0U,
         "required keyword param flagged");
  expect((method.params[1].flags &
          amber::bytecode::kMethodParamFlagHasDefault) == 0U,
         "required keyword param has no default flag");
  expect((method.params[2].flags & amber::bytecode::kMethodParamFlagKeyword) !=
             0U,
         "optional keyword param flagged");
  expect((method.params[2].flags &
          amber::bytecode::kMethodParamFlagHasDefault) != 0U,
         "optional keyword param carries default flag");
}

void test_case_emission() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def choose(x):\n"
                                                          "  case x:\n"
                                                          "    when 0:\n"
                                                          "      1\n"
                                                          "    else:\n"
                                                          "      2\n");
  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "choose case code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::PCheckEq),
         "case emits P_CHECK_EQ");
  expect(contains_opcode(*code, amber::bytecode::Opcode::Jump),
         "case emits jumps");
  expect(contains_opcode(*code, amber::bytecode::Opcode::GetLast),
         "case emits GETLAST");
}

void test_pattern_assignment_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def unpack(values):\n"
              "  [head, *tail] = values\n");
  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "unpack code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::PPrepSeq),
         "pattern assignment emits P_PREP_SEQ");
  expect(contains_opcode(*code, amber::bytecode::Opcode::PCheckLenGte),
         "pattern assignment emits P_CHECK_LEN_GTE");
  expect(contains_opcode(*code, amber::bytecode::Opcode::PGetIndex),
         "pattern assignment emits P_GET_INDEX");
  expect(contains_opcode(*code, amber::bytecode::Opcode::PBind),
         "pattern assignment emits P_BIND");
  expect(contains_opcode(*code, amber::bytecode::Opcode::PCommit),
         "pattern assignment emits P_COMMIT");
}

void test_matcher_expr_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def choose(x, limit):\n"
              "  case x:\n"
              "    when limit + 1:\n"
              "      x\n"
              "    else:\n"
              "      0\n");
  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "matcher-expression code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::PTripleEq),
         "matcher-expression emits P_TRIPLE_EQ");
}

void test_dynamic_pattern_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def classify(shape):\n"
              "  case shape:\n"
              "    when pattern(route(\"/users/:id\")) with {id:, **null}:\n"
              "      id\n"
              "    else:\n"
              "      shape\n");
  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "dynamic pattern code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::Send),
         "dynamic pattern emits SEND");
  expect(contains_opcode(*code, amber::bytecode::Opcode::PPrepMap),
         "dynamic pattern emits P_PREP_MAP");
  expect(contains_opcode(*code, amber::bytecode::Opcode::PHasKey),
         "dynamic pattern emits P_HAS_KEY");
  expect(contains_opcode(*code, amber::bytecode::Opcode::PGetKey),
         "dynamic pattern emits P_GET_KEY");
  expect(contains_opcode(*code, amber::bytecode::Opcode::PBind),
         "dynamic pattern emits P_BIND");
  expect(contains_opcode(*code, amber::bytecode::Opcode::Raise),
         "dynamic pattern emits protocol RAISE path");
  expect(contains_symbol(emit_result.module, "match"),
         "dynamic pattern interns :match");
  expect(contains_symbol(emit_result.module, "success"),
         "dynamic pattern interns :success");
  expect(contains_symbol(emit_result.module, "bindings"),
         "dynamic pattern interns :bindings");
  expect(!has_diagnostic_code(emit_result, "BC2007"),
         "dynamic pattern no longer emits BC2007");
}

void test_clause_method_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def fact(0): 1\n"
              "def fact(n) if n > 0: n\n");
  expect(emit_result.module.methods.size() == 1,
         "expected one clause-style method");
  const amber::bytecode::BcMethod &method = emit_result.module.methods[0];
  expect(method.clause_table.size() == 2, "expected two clause entries");
  expect(emit_result.module.pattern_programs.size() == 2,
         "expected two pattern program descriptors");
  expect(emit_result.module.pattern_programs[0].binding_count == 0,
         "literal clause should not bind");
  expect(emit_result.module.pattern_programs[1].binding_count == 1,
         "binder clause should expose one binding");
  expect(code_by_id(emit_result.module, method.entry_code_id) != nullptr,
         "clause else-body code exists");
  expect(code_by_id(emit_result.module,
                    method.clause_table[0].pattern_code_id) != nullptr,
         "clause pattern code exists");
  expect(code_by_id(emit_result.module, method.clause_table[0].guard_code_id) !=
             nullptr,
         "clause guard code exists");
  expect(code_by_id(emit_result.module, method.clause_table[0].body_code_id) !=
             nullptr,
         "clause body code exists");
  const amber::bytecode::BcCode *pattern_code =
      code_by_id(emit_result.module, method.clause_table[1].pattern_code_id);
  expect(pattern_code != nullptr, "binder clause pattern code exists");
  expect(contains_opcode(*pattern_code, amber::bytecode::Opcode::PBind),
         "binder clause pattern emits P_BIND");
  expect(contains_opcode(*pattern_code, amber::bytecode::Opcode::PCommit),
         "binder clause pattern emits P_COMMIT");
}

void test_w13_operator_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def ops(x, xs, a, b):\n"
              "  x in xs\n"
              "  a and b\n"
              "  a or b\n"
              "  1_000 + 0x10\n"
              "  1..10\n");
  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "ops code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::JumpIfFalse),
         "and emits JUMP_IF_FALSE");
  expect(contains_opcode(*code, amber::bytecode::Opcode::JumpIfTrue),
         "or emits JUMP_IF_TRUE");
  expect(contains_symbol(emit_result.module, "contains?"),
         "in interns contains?");
  expect(contains_symbol(emit_result.module, "Range"), "range interns Range");
  expect(contains_symbol(emit_result.module, "new"),
         "range interns constructor selector");
  expect(contains_symbol(emit_result.module, "inclusive_end"),
         "range interns inclusive_end keyword");

  bool saw_1000 = false;
  bool saw_16 = false;
  for (const amber::bytecode::Constant &constant :
       emit_result.module.const_pool) {
    if (constant.kind != amber::bytecode::ConstantKind::Integer) {
      continue;
    }
    saw_1000 = saw_1000 || constant.int_value == 1000;
    saw_16 = saw_16 || constant.int_value == 16;
  }
  expect(saw_1000, "underscored integer canonicalized");
  expect(saw_16, "hex integer canonicalized");
}

void test_block_param_pattern_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def transform(xs):\n"
              "  xs.map |[head, *tail]|: head\n");
  expect(emit_result.module.code_objects.size() >= 3,
         "expected block code for pattern-param closure");

  const amber::bytecode::BcCode *method_code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(method_code != nullptr, "transform code exists");

  const amber::bytecode::BcCode *closure_code = nullptr;
  for (const amber::bytecode::BcCode &code : emit_result.module.code_objects) {
    if (code.code_id != method_code->code_id &&
        contains_opcode(code, amber::bytecode::Opcode::PPrepSeq)) {
      closure_code = &code;
      break;
    }
  }
  expect(closure_code != nullptr, "closure code with pattern prologue exists");
  expect(contains_opcode(*closure_code, amber::bytecode::Opcode::PCheckLenGte),
         "block param pattern emits P_CHECK_LEN_GTE");
  expect(contains_opcode(*closure_code, amber::bytecode::Opcode::PBind),
         "block param pattern emits P_BIND");
  expect(contains_opcode(*closure_code, amber::bytecode::Opcode::PCommit),
         "block param pattern emits P_COMMIT");
  expect(contains_opcode(*closure_code, amber::bytecode::Opcode::PFail),
         "block param mismatch path emits P_FAIL");
}

void test_simple_block_param_emission() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def transform(xs):\n"
                                                          "  xs.map |x|: x\n");
  expect(emit_result.module.code_objects.size() >= 3,
         "expected block code for simple closure param");

  const amber::bytecode::BcCode *method_code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(method_code != nullptr, "simple transform code exists");

  const amber::bytecode::BcCode *closure_code = nullptr;
  for (const amber::bytecode::BcCode &code : emit_result.module.code_objects) {
    if (code.code_id != method_code->code_id &&
        contains_opcode(code, amber::bytecode::Opcode::PCommit)) {
      closure_code = &code;
      break;
    }
  }
  expect(closure_code != nullptr, "simple closure code exists");
  expect(contains_opcode(*closure_code, amber::bytecode::Opcode::PBind),
         "simple block param emits P_BIND");
  expect(contains_opcode(*closure_code, amber::bytecode::Opcode::PCommit),
         "simple block param emits P_COMMIT");
}

void test_object_model_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("package physics.core\n"
              "export Timestamped, Particle\n"
              "\n"
              "mixin Timestamped:\n"
              "  def touch!():\n"
              "    noop\n"
              "\n"
              "class Particle < Entity:\n"
              "  include Timestamped\n"
              "  extend Serializable\n"
              "  class_method def find(id):\n"
              "    id\n"
              "  def init(@масса, α = 1):\n"
              "    pass\n");

  expect(emit_result.module.classes.size() == 2,
         "expected mixin and class descriptors");
  expect(emit_result.module.methods.size() == 3,
         "expected mixin method plus two class methods");

  const amber::bytecode::BcClass &mixin = emit_result.module.classes[0];
  expect((mixin.flags & amber::bytecode::kClassFlagMixin) != 0U,
         "mixin descriptor carries mixin flag");
  expect(mixin.method_range_start == 0 && mixin.method_range_count == 1,
         "mixin method range");
  expect(mixin.direct_include_refs.empty(),
         "mixin has no direct includes here");

  const amber::bytecode::BcClass &klass = emit_result.module.classes[1];
  expect((klass.flags & amber::bytecode::kClassFlagMixin) == 0U,
         "class descriptor has no mixin flag");
  expect(klass.has_superclass_ref, "class keeps superclass ref");
  expect(path_constant_text(emit_result.module, klass.superclass_ref) ==
             "Entity",
         "superclass path preserved");
  expect(klass.direct_include_refs.size() == 1, "class preserves include refs");
  expect(path_constant_text(emit_result.module, klass.direct_include_refs[0]) ==
             "Timestamped",
         "include path preserved");
  expect(klass.direct_extend_refs.size() == 1, "class preserves extend refs");
  expect(path_constant_text(emit_result.module, klass.direct_extend_refs[0]) ==
             "Serializable",
         "extend path preserved");
  expect(klass.method_range_start == 1 && klass.method_range_count == 2,
         "class method range");

  expect(emit_result.module.methods[0].owner_dispatch_ref == 0 &&
             emit_result.module.methods[0].flags == 1,
         "mixin method owned by mixin instance side");
  expect(emit_result.module.methods[1].owner_dispatch_ref == 1 &&
             emit_result.module.methods[1].flags == 2,
         "class-side method owned by class descriptor");
  expect(emit_result.module.methods[2].owner_dispatch_ref == 1 &&
             emit_result.module.methods[2].flags == 1,
         "instance method owned by class descriptor");

  expect(emit_result.module.exports.size() == 2,
         "mixin and class exports emitted");
  expect(emit_result.module.exports[0].target_index == 0,
         "mixin export targets first descriptor");
  expect(emit_result.module.exports[1].target_index == 1,
         "class export targets second descriptor");
}

} // namespace

int main() {
  test_if_and_round_trip();
  test_loop_and_safepoint();
  test_closure_capture_emission();
  test_default_thunk_emission();
  test_keyword_param_emission();
  test_case_emission();
  test_pattern_assignment_emission();
  test_matcher_expr_emission();
  test_dynamic_pattern_emission();
  test_clause_method_emission();
  test_w13_operator_emission();
  test_block_param_pattern_emission();
  test_simple_block_param_emission();
  test_object_model_emission();
  std::cout << "emitter_tests: ok\n";
  return 0;
}
