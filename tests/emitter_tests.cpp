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

amber::bytecode::EmitResult emit_allow_bytecode_diagnostics(
    const std::string &source) {
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
  return amber::bytecode::emit_program(program, parse_result.module_name);
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

std::size_t count_opcode(const amber::bytecode::BcCode &code,
                         amber::bytecode::Opcode opcode) {
  std::size_t count = 0;
  for (const amber::bytecode::Instruction &instruction : code.instructions) {
    if (instruction.opcode == opcode) {
      ++count;
    }
  }
  return count;
}

bool module_contains_opcode(const amber::bytecode::BcModule &module,
                            amber::bytecode::Opcode opcode) {
  for (const amber::bytecode::BcCode &code : module.code_objects) {
    if (contains_opcode(code, opcode)) {
      return true;
    }
  }
  return false;
}

bool module_contains_callsite_flag(const amber::bytecode::BcModule &module,
                                   std::uint32_t flag) {
  for (const amber::bytecode::BcCode &code : module.code_objects) {
    for (const amber::bytecode::CacheSiteEntry &entry :
         code.call_site_table) {
      if ((entry.flags & flag) != 0U) {
        return true;
      }
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

bool contains_string(const amber::bytecode::BcModule &module,
                     const std::string &text) {
  for (const std::string &value : module.strings) {
    if (value == text) {
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

void test_type_hook_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def f(x as Int) -> Int:\n"
              "  x\n");
  expect(emit_result.module.methods.size() == 1,
         "expected one typed method");
  const amber::bytecode::BcMethod &method = emit_result.module.methods[0];
  expect(method.type_hook_ids.size() == 2,
         "parameter and return type hooks emitted");

  for (std::uint32_t hook_code_id : method.type_hook_ids) {
    const amber::bytecode::BcCode *hook_code =
        code_by_id(emit_result.module, hook_code_id);
    expect(hook_code != nullptr, "type hook code exists");
    expect(hook_code->kind == amber::bytecode::CodeKind::Block,
           "type hook code kind");
    expect(hook_code->reg_count == 1, "type hook value register exists");
    expect(contains_opcode(*hook_code, amber::bytecode::Opcode::TypeCheck),
           "type hook emits TYPECHECK");
    expect(contains_opcode(*hook_code, amber::bytecode::Opcode::Return),
           "type hook emits RETURN");
  }

  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), "typed method bytecode round-trips through verifier");
  expect(decoded.module.methods.size() == 1,
         "typed method survives round-trip");
  expect(decoded.module.methods[0].type_hook_ids.size() == 2,
         "type hook ids survive round-trip");
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
  expect(!contains_opcode(*code, amber::bytecode::Opcode::GetLast),
         "case without explicit last-value avoids GETLAST");
}

void test_last_result_elision_without_explicit_last_value() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def compute():\n"
                                                          "  x = 1\n"
                                                          "  x + 2\n");
  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "compute code exists");
  expect(!contains_opcode(*code, amber::bytecode::Opcode::SetLast),
         "method without $_ avoids SETLAST");
  expect(!contains_opcode(*code, amber::bytecode::Opcode::GetLast),
         "method without $_ avoids GETLAST");
  expect(!code->instructions.empty() &&
             code->instructions.front().opcode ==
                 amber::bytecode::Opcode::LoadK &&
             code->instructions.front().operands.size() >= 2 &&
             code->instructions.front().operands[0].value == 0,
         "constant assignment loads directly into local slot");
}

void test_explicit_last_value_preserves_last_result_updates() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def compute():\n"
                                                          "  1\n"
                                                          "  $_ + 2\n");
  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "last-value code exists");
  expect(count_opcode(*code, amber::bytecode::Opcode::SetLast) >= 1,
         "$_ method emits SETLAST for prior expression");
  expect(contains_opcode(*code, amber::bytecode::Opcode::GetLast),
         "$_ method emits GETLAST");
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

void test_optional_index_emission() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def lookup(xs, i):\n"
                                                          "  xs[?i]\n");
  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "optional index code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::Send),
         "optional index emits SEND");
  expect(contains_symbol(emit_result.module, "[]?"),
         "optional index interns []? selector");
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

void test_collection_literal_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("[1, 2]\n"
              "(3, 4)\n"
              "{5}\n"
              "{id: :ok}\n"
              "{\"name\": 5, 1: 6}\n");
  const amber::bytecode::BcCode *code =
      code_by_kind(emit_result.module, amber::bytecode::CodeKind::Module);
  expect(code != nullptr, "module init code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::MakeList),
         "list literal emits MAKE_LIST");
  expect(contains_opcode(*code, amber::bytecode::Opcode::MakeTuple),
         "tuple literal emits MAKE_TUPLE");
  expect(contains_opcode(*code, amber::bytecode::Opcode::MakeSet),
         "set literal emits MAKE_SET");
  expect(contains_opcode(*code, amber::bytecode::Opcode::MakeMap),
         "symbol-only map literal emits MAKE_MAP");
  expect(contains_opcode(*code, amber::bytecode::Opcode::MakeMapDyn),
         "expression-key map literal emits MAKE_MAP_DYN");
  expect(contains_symbol(emit_result.module, "ok"),
         "symbol literal interns symbol");
  expect(contains_symbol(emit_result.module, "id"),
         "identifier map key interns symbol");
  expect(contains_string(emit_result.module, "name"),
         "string map key interns string constant");
}

void test_v20_7_spread_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def take(a, b:, c:):\n"
              "  a\n"
              "args = [2, 3]\n"
              "opts = {b: 2, c: 3}\n"
              "take(1, *args)\n"
              "take(1, **opts)\n"
              "[0, *args]\n"
              "{0, *args}\n"
              "{a: 1, **opts}\n");
  const amber::bytecode::BcCode *code =
      code_by_kind(emit_result.module, amber::bytecode::CodeKind::Module);
  expect(code != nullptr, "module init code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::CallSpread),
         "spread call emits CALL_SPREAD");
  expect(contains_opcode(*code, amber::bytecode::Opcode::MakeListSpread),
         "array spread emits MAKE_LIST_SPREAD");
  expect(contains_opcode(*code, amber::bytecode::Opcode::MakeSetSpread),
         "set spread emits MAKE_SET_SPREAD");
  expect(contains_opcode(*code, amber::bytecode::Opcode::MakeMapSpread),
         "map spread emits MAKE_MAP_SPREAD");

  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));
}

void test_kernel_watch_emission() {
  const amber::bytecode::EmitResult local_result =
      emit_ok("x = 1\n"
              "Kernel.watch(x)\n");
  expect(local_result.module.init.has_entry_code_id,
         "watch local module init exists");
  const amber::bytecode::BcCode *init_code =
      code_by_id(local_result.module, local_result.module.init.entry_code_id);
  expect(init_code != nullptr, "watch local init code exists");
  expect(contains_opcode(*init_code, amber::bytecode::Opcode::WatchLocal),
         "Kernel.watch(local) emits WATCH_LOCAL");
  const amber::bytecode::DecodeResult local_decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(local_result.module));
  expect(local_decoded.ok(),
         amber::bytecode::verify_errors_to_json(local_decoded.errors));

  const amber::bytecode::EmitResult capture_result =
      emit_ok("def watch_each(xs, x):\n"
              "  xs.map: Kernel.watch(x)\n");
  expect(module_contains_opcode(capture_result.module,
                                amber::bytecode::Opcode::WatchUpval),
         "Kernel.watch(capture) emits WATCH_UPVAL");

  const amber::bytecode::EmitResult ivar_result =
      emit_ok("class Particle:\n"
              "  def observe():\n"
              "    Kernel.watch(@mass)\n");
  expect(module_contains_opcode(ivar_result.module,
                                amber::bytecode::Opcode::WatchIvar),
         "Kernel.watch(ivar) emits WATCH_IVAR");
  const amber::bytecode::DecodeResult ivar_decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(ivar_result.module));
  expect(ivar_decoded.ok(),
         amber::bytecode::verify_errors_to_json(ivar_decoded.errors));

  const amber::bytecode::EmitResult invalid_result =
      emit_allow_bytecode_diagnostics("x = 1\n"
                                      "Kernel.watch(x + 1)\n");
  expect(!invalid_result.ok(), "invalid watch target emits diagnostic");
  expect(has_diagnostic_code(invalid_result, "BC2008"),
         "invalid watch target diagnostic code");
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

void test_property_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("prop answer: 42\n"
              "class User:\n"
              "  prop full_name:\n"
              "    get: @first\n"
              "    set(value): @first = value\n"
              "class Box:\n"
              "  attr var value\n"
              "user.full_name\n"
              "user.full_name = \"Ada\"\n");

  expect(emit_result.module.methods.size() == 5,
         "module getter, property, and attr methods emitted");
  expect((emit_result.module.methods[0].flags &
          amber::bytecode::kMethodFlagPropertyGetter) != 0U,
         "module property getter flag emitted");
  expect((emit_result.module.methods[1].flags &
          amber::bytecode::kMethodFlagPropertyGetter) != 0U,
         "instance property getter flag emitted");
  expect((emit_result.module.methods[1].flags &
          amber::bytecode::kMethodFlagInstance) != 0U,
         "instance property keeps dispatch flag");
  expect((emit_result.module.methods[2].flags &
          amber::bytecode::kMethodFlagPropertySetter) != 0U,
         "instance property setter flag emitted");
  expect((emit_result.module.methods[2].flags &
          amber::bytecode::kMethodFlagInstance) != 0U,
         "instance property setter keeps dispatch flag");
  expect((emit_result.module.methods[3].flags &
          amber::bytecode::kMethodFlagPropertyGetter) != 0U,
         "attr getter flag emitted");
  expect((emit_result.module.methods[4].flags &
          amber::bytecode::kMethodFlagPropertySetter) != 0U,
         "attr setter flag emitted");
  expect(module_contains_callsite_flag(
             emit_result.module,
             amber::bytecode::kCallSiteFlagPropertyAccess),
         "bare member property access callsite flag emitted");
  expect(module_contains_callsite_flag(
             emit_result.module,
             amber::bytecode::kCallSiteFlagPropertyAssignment),
         "member property assignment callsite flag emitted");
}

void test_try_rescue_ensure_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("try:\n"
              "  raise \"boom\"\n"
              "rescue TypeError:\n"
              "  1\n"
              "rescue:\n"
              "  2\n"
              "ensure:\n"
              "  3\n");

  expect(emit_result.module.init.has_entry_code_id,
         "try module init emitted");
  const amber::bytecode::BcCode *code =
      code_by_id(emit_result.module, emit_result.module.init.entry_code_id);
  expect(code != nullptr, "try module init code exists");
  expect(code->handler_table.size() == 1,
         "try emits one protected handler entry");
  expect(amber::bytecode::handler_kind(code->handler_table[0].flags) ==
             amber::bytecode::kHandlerKindRescue,
         "try rescue handler kind is encoded");
  expect(contains_opcode(*code, amber::bytecode::Opcode::Raise),
         "try body emits RAISE");

  const amber::bytecode::BcCode *rescue =
      code_by_kind(emit_result.module, amber::bytecode::CodeKind::Rescue);
  expect(rescue != nullptr, "rescue handler code exists");
  expect(contains_opcode(*rescue, amber::bytecode::Opcode::TripleEq),
         "typed rescue matcher emits TRIPLE_EQ");

  const amber::bytecode::BcCode *ensure =
      code_by_kind(emit_result.module, amber::bytecode::CodeKind::Ensure);
  expect(ensure != nullptr, "ensure handler code exists");
}

void test_throw_catch_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("catch(:enough):\n"
              "  throw :enough, 42\n");

  expect(emit_result.module.init.has_entry_code_id,
         "catch module init emitted");
  const amber::bytecode::BcCode *code =
      code_by_id(emit_result.module, emit_result.module.init.entry_code_id);
  expect(code != nullptr, "catch module init code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::Throw),
         "throw body emits THROW");
  expect(code->handler_table.size() == 1,
         "catch emits one protected handler entry");
  expect(amber::bytecode::handler_kind(code->handler_table[0].flags) ==
             amber::bytecode::kHandlerKindCatch,
         "catch handler kind is encoded");

  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));
}

void test_integer_specialized_send_emission() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def fast():\n"
              "  x = 10\n"
              "  y = 3\n"
              "  z = x + y\n"
              "  q = z - 2\n"
              "  product = x * y\n"
              "  quotient = product / y\n"
              "  remainder = product % y\n"
              "  floored = product // y\n"
              "  anded = x & y\n"
              "  ored = x | 4\n"
              "  xored = x ^ y\n"
              "  shifted = x << 2\n"
              "  unshifted = shifted >> 1\n"
              "  cmp = x <=> y\n"
              "  le = q <= x\n"
              "  ge = q >= 0\n"
              "  eq = x == 10\n"
              "  ne = x != y\n"
              "  (q < x) and (q > 0) and le and ge and eq and ne and "
              "(cmp > 0) and (floored > 0) and (remainder >= 0) and "
              "(quotient > 0) and (anded > 0) and (ored > 0) and "
              "(xored > 0) and (shifted > unshifted)\n");

  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "fast code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IAdd),
         "integer local addition emits IADD");
  expect(contains_opcode(*code, amber::bytecode::Opcode::ISubK),
         "integer literal subtraction emits ISUBK");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IMul),
         "integer local multiplication emits IMUL");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IDiv),
         "integer local division emits IDIV");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IMod),
         "integer local modulo emits IMOD");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IFloorDiv),
         "integer local floor division emits IFLOORDIV");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IBitAnd),
         "integer local bit and emits IBITAND");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IBitOrK),
         "integer literal bit or emits IBITORK");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IBitXor),
         "integer local xor emits IBITXOR");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IShlK),
         "integer literal left shift emits ISHLK");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IShrK),
         "integer literal right shift emits ISHRK");
  expect(contains_opcode(*code, amber::bytecode::Opcode::ILt),
         "integer local comparison emits ILT");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IGtK),
         "integer literal comparison emits IGTK");
  expect(contains_opcode(*code, amber::bytecode::Opcode::ILe),
         "integer <= comparison emits ILE");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IGeK),
         "integer >= literal comparison emits IGEK");
  expect(contains_opcode(*code, amber::bytecode::Opcode::IEqK),
         "integer equality literal comparison emits IEQK");
  expect(contains_opcode(*code, amber::bytecode::Opcode::INe),
         "integer inequality local comparison emits INE");
  expect(contains_opcode(*code, amber::bytecode::Opcode::ICmp),
         "integer spaceship comparison emits ICMP");
  expect(!contains_opcode(*code, amber::bytecode::Opcode::Send),
         "integer-only method avoids generic SEND");
}

void test_unknown_integer_send_stays_dynamic() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def add(x, y):\n"
              "  x + y\n");

  const amber::bytecode::BcCode *code = code_by_id(
      emit_result.module, emit_result.module.methods[0].entry_code_id);
  expect(code != nullptr, "add code exists");
  expect(contains_opcode(*code, amber::bytecode::Opcode::Send),
         "unknown parameter addition keeps SEND");
  expect(!contains_opcode(*code, amber::bytecode::Opcode::IAdd),
         "unknown parameter addition does not emit IADD");
}

} // namespace

int main() {
  test_if_and_round_trip();
  test_loop_and_safepoint();
  test_closure_capture_emission();
  test_default_thunk_emission();
  test_type_hook_emission();
  test_keyword_param_emission();
  test_case_emission();
  test_last_result_elision_without_explicit_last_value();
  test_explicit_last_value_preserves_last_result_updates();
  test_pattern_assignment_emission();
  test_optional_index_emission();
  test_matcher_expr_emission();
  test_dynamic_pattern_emission();
  test_clause_method_emission();
  test_w13_operator_emission();
  test_collection_literal_emission();
  test_v20_7_spread_emission();
  test_kernel_watch_emission();
  test_block_param_pattern_emission();
  test_simple_block_param_emission();
  test_object_model_emission();
  test_property_emission();
  test_try_rescue_ensure_emission();
  test_throw_catch_emission();
  test_integer_specialized_send_emission();
  test_unknown_integer_send_stays_dynamic();
  std::cout << "emitter_tests: ok\n";
  return 0;
}
