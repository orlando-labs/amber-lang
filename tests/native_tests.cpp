#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "optimizer/mir.h"
#include "optimizer/native.h"
#include "runtime/native_bridge.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct CompiledArtifacts {
  amber::bytecode::BcModule bytecode_module;
  amber::mir::Module mir_module;
  amber::native::NativeModule native_module;
};

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "native test failed: " << message << "\n";
    std::exit(1);
  }
}

CompiledArtifacts compile_ok(const std::string &source) {
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
  amber::mir::Module mir_module =
      amber::mir::lower_program(program, parse_result.module_name);
  const amber::mir::ValidationResult mir_validation =
      amber::mir::validate_module(mir_module);
  if (!mir_validation.ok()) {
    std::cerr << amber::mir::validation_errors_to_json(mir_validation.errors);
    std::exit(1);
  }

  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(emit_result.diagnostics);
    std::exit(1);
  }
  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(emit_result.module);
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(bytes);
  if (!decoded.ok()) {
    std::cerr << amber::bytecode::verify_errors_to_json(decoded.errors);
    std::exit(1);
  }

  amber::native::NativeModule native_module =
      amber::native::compile_native_module(decoded.module, mir_module);
  const amber::native::NativeValidationResult native_validation =
      amber::native::validate_native_module(native_module, &decoded.module);
  if (!native_validation.ok()) {
    std::cerr << amber::native::diagnostics_to_json(
        native_validation.diagnostics);
    std::exit(1);
  }

  return {decoded.module, std::move(mir_module), std::move(native_module)};
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

const amber::native::NativeCodeObject *
native_code_for_bc(const amber::native::NativeModule &module,
                   std::uint32_t code_id) {
  for (const amber::native::NativeCodeObject &code : module.code_objects) {
    if (code.source_bc_code_id == code_id) {
      return &code;
    }
  }
  return nullptr;
}

bool has_stub_kind(const amber::native::NativeCodeObject &code,
                   const std::string &kind, bool reflective) {
  for (const amber::native::NativeCallStub &stub : code.call_stub_table) {
    if (stub.kind == kind && stub.reflective == reflective) {
      return true;
    }
  }
  return false;
}

bool has_patchpoint_kind(const amber::native::NativeCodeObject &code,
                         const std::string &kind) {
  for (const amber::native::NativePatchpoint &patchpoint : code.patchpoints) {
    if (patchpoint.kind == kind) {
      return true;
    }
  }
  return false;
}

void test_native_metadata_preserves_call_and_root_maps() {
  const CompiledArtifacts artifacts = compile_ok("def add(x, y):\n"
                                                 "  x + y\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(artifacts.bytecode_module, "add");
  expect(method != nullptr, "add method exists");
  const amber::native::NativeCodeObject *code =
      native_code_for_bc(artifacts.native_module, method->entry_code_id);
  expect(code != nullptr, "native code object exists for add");
  expect(!code->machine_code_blob.empty(), "native blob is recorded");
  expect(!code->root_maps.empty(), "root maps are emitted");
  expect(!code->safepoint_maps.empty(), "safepoint maps are emitted");
  expect(has_stub_kind(*code, "send", false), "SEND lowers to call stub");
  expect(has_patchpoint_kind(*code, "call_ic"),
         "SEND gets JIT call-IC patchpoint");
}

void test_reflective_send_dyn_uses_slow_stub_metadata() {
  const CompiledArtifacts artifacts =
      compile_ok("def invoke(recv, selector, value):\n"
                 "  send(recv, selector, value)\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(artifacts.bytecode_module, "invoke");
  expect(method != nullptr, "invoke method exists");
  const amber::native::NativeCodeObject *code =
      native_code_for_bc(artifacts.native_module, method->entry_code_id);
  expect(code != nullptr, "native code object exists for invoke");
  expect(has_stub_kind(*code, "send_dyn", true),
         "SEND_DYN uses reflective slow stub");
  expect(has_patchpoint_kind(*code, "reflective_send_dyn"),
         "SEND_DYN gets reflective patchpoint");
}

void test_native_trampoline_requires_frozen_world_and_executes() {
  const CompiledArtifacts artifacts = compile_ok("def add(x, y):\n"
                                                 "  x + y\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(artifacts.bytecode_module, "add");
  expect(method != nullptr, "add method exists");
  const amber::native::NativeCodeObject *code =
      native_code_for_bc(artifacts.native_module, method->entry_code_id);
  expect(code != nullptr, "native code object exists for add");

  amber::runtime::RuntimeWorld world(artifacts.bytecode_module);
  const amber::runtime::ExecutionResult open_result =
      amber::runtime::execute_native_code(world, artifacts.native_module,
                                          code->native_id,
                                          {amber::runtime::Value::integer(9),
                                           amber::runtime::Value::integer(4)});
  expect(!open_result.ok(), "open world native execution is rejected");
  expect(open_result.fault->error_name == "WorldFrozenError",
         "open world rejection uses WorldFrozenError");

  expect(world.freeze_world().ok(), "freeze_world should succeed");
  const amber::native::NativeModule bound =
      amber::runtime::bind_native_module_to_world(artifacts.native_module,
                                                  world.world_mirror());
  const amber::runtime::ExecutionResult native_result =
      amber::runtime::execute_native_code(world, bound, code->native_id,
                                          {amber::runtime::Value::integer(9),
                                           amber::runtime::Value::integer(4)});
  expect(native_result.ok(), "bound native trampoline execution succeeds");
  expect(native_result.value.is_integer() &&
             native_result.value.as_integer() == 13,
         "native trampoline returns bytecode-equivalent result");
}

void test_stale_native_assumption_can_fall_back_to_bytecode() {
  const CompiledArtifacts artifacts = compile_ok("def id(x):\n"
                                                 "  x\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(artifacts.bytecode_module, "id");
  expect(method != nullptr, "id method exists");
  const amber::native::NativeCodeObject *code =
      native_code_for_bc(artifacts.native_module, method->entry_code_id);
  expect(code != nullptr, "native code object exists for id");

  amber::runtime::RuntimeWorld world(artifacts.bytecode_module);
  const amber::native::NativeModule stale =
      amber::runtime::bind_native_module_to_world(artifacts.native_module,
                                                  world.world_mirror());
  expect(world.freeze_world().ok(), "freeze_world should succeed");

  const amber::runtime::ExecutionResult rejected =
      amber::runtime::execute_native_code(world, stale, code->native_id,
                                          {amber::runtime::Value::integer(5)});
  expect(!rejected.ok(), "stale native assumptions are rejected");
  expect(rejected.fault->error_name == "NativeInvalidationError",
         "stale native assumption reports invalidation");

  amber::runtime::NativeExecutionOptions options;
  options.allow_bytecode_fallback = true;
  const amber::runtime::ExecutionResult fallback =
      amber::runtime::execute_native_code(
          world, stale, code->native_id, {amber::runtime::Value::integer(5)},
          amber::runtime::Value::null(), amber::runtime::Value::null(),
          options);
  expect(fallback.ok(), "stale native code can re-enter bytecode");
  expect(fallback.value.is_integer() && fallback.value.as_integer() == 5,
         "bytecode fallback preserves result");
}

} // namespace

int main() {
  test_native_metadata_preserves_call_and_root_maps();
  test_reflective_send_dyn_uses_slow_stub_metadata();
  test_native_trampoline_requires_frozen_world_and_executes();
  test_stale_native_assumption_can_fall_back_to_bytecode();
  return 0;
}
