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

bool has_slowpath_kind(const amber::native::NativeCodeObject &code,
                       const std::string &kind) {
  for (const amber::native::NativeSlowPath &slowpath : code.slowpath_table) {
    if (slowpath.kind == kind) {
      return true;
    }
  }
  return false;
}

bool has_root_map_for_ip(const amber::native::NativeCodeObject &code,
                         std::uint32_t ip) {
  for (const amber::native::NativeRootMap &root : code.root_maps) {
    if (root.ip_offset == ip) {
      return true;
    }
  }
  return false;
}

bool has_safepoint_kind(const amber::native::NativeCodeObject &code,
                        const std::string &kind) {
  for (const amber::native::NativeSafepointMap &safepoint :
       code.safepoint_maps) {
    if (safepoint.kind == kind) {
      return true;
    }
  }
  return false;
}

bool safepoint_kind_has_root_map(const amber::native::NativeCodeObject &code,
                                 const std::string &kind) {
  for (const amber::native::NativeSafepointMap &safepoint :
       code.safepoint_maps) {
    if (safepoint.kind == kind &&
        has_root_map_for_ip(code, safepoint.ip_offset)) {
      return true;
    }
  }
  return false;
}

bool has_native_diagnostic(const amber::native::NativeValidationResult &result,
                           const std::string &code) {
  for (const amber::native::NativeDiagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
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
  expect(has_slowpath_kind(*code, "send"),
         "SEND records an explicit runtime slowpath");
  expect(has_slowpath_kind(*code, "assumption_invalidation"),
         "frozen native code records bytecode fallback invalidation slowpath");
  const std::string json =
      amber::native::module_to_json(artifacts.native_module, "test-hash");
  expect(json.find("\"slowpath_table\"") != std::string::npos,
         "native JSON exposes slowpath_table");
  const std::string dump =
      amber::native::module_to_dump(artifacts.native_module, "test-hash");
  expect(dump.find("slowpath q") != std::string::npos,
         "native dump exposes slowpath entries");
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
  expect(has_slowpath_kind(*code, "send_dyn"),
         "SEND_DYN records reflective slowpath metadata");
}

void test_exception_edges_have_native_root_and_slowpath_metadata() {
  amber::bytecode::BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};

  amber::bytecode::BcCode code;
  code.code_id = 1;
  code.kind = amber::bytecode::CodeKind::Method;
  code.reg_count = 1;
  code.instructions.push_back({amber::bytecode::Opcode::Raise, {{0, false}}});
  code.instructions.push_back({amber::bytecode::Opcode::GetLast, {{0, false}}});
  code.instructions.push_back({amber::bytecode::Opcode::Return, {{0, false}}});
  code.handler_table.push_back({0, 1, 1, 1, 0});
  module.code_objects.push_back(code);

  amber::mir::Module mir_module;
  mir_module.module_name = "native.manual";
  amber::native::NativeModule native_module =
      amber::native::compile_native_module(module, mir_module);
  const amber::native::NativeValidationResult validation =
      amber::native::validate_native_module(native_module, &module);
  expect(validation.ok(),
         amber::native::diagnostics_to_json(validation.diagnostics));

  const amber::native::NativeCodeObject *native_code =
      native_code_for_bc(native_module, 1);
  expect(native_code != nullptr, "manual native code object exists");
  expect(has_stub_kind(*native_code, "raise", false),
         "RAISE records runtime raise stub");
  expect(has_slowpath_kind(*native_code, "raise"),
         "RAISE records language-error preserving slowpath");
  expect(has_root_map_for_ip(*native_code, 1),
         "exception handler pc has a native root map");
}

void test_throw_edges_have_native_slowpath_metadata() {
  amber::bytecode::BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};

  amber::bytecode::BcCode code;
  code.code_id = 1;
  code.kind = amber::bytecode::CodeKind::Method;
  code.reg_count = 2;
  code.instructions.push_back(
      {amber::bytecode::Opcode::Throw, {{0, false}, {1, false}}});
  code.instructions.push_back({amber::bytecode::Opcode::Return, {{1, false}}});
  module.code_objects.push_back(code);

  amber::mir::Module mir_module;
  mir_module.module_name = "native.manual";
  amber::native::NativeModule native_module =
      amber::native::compile_native_module(module, mir_module);
  const amber::native::NativeValidationResult validation =
      amber::native::validate_native_module(native_module, &module);
  expect(validation.ok(),
         amber::native::diagnostics_to_json(validation.diagnostics));

  const amber::native::NativeCodeObject *native_code =
      native_code_for_bc(native_module, 1);
  expect(native_code != nullptr, "manual throw native code object exists");
  expect(has_stub_kind(*native_code, "throw", false),
         "THROW records runtime throw stub");
  expect(has_slowpath_kind(*native_code, "throw"),
         "THROW records language-control slowpath");
}

void test_native_root_maps_cover_gc_boundary_kinds() {
  amber::bytecode::BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};

  amber::bytecode::Constant one;
  one.kind = amber::bytecode::ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  amber::bytecode::BcCode code;
  code.code_id = 1;
  code.kind = amber::bytecode::CodeKind::Method;
  code.reg_count = 5;
  code.instructions.push_back(
      {amber::bytecode::Opcode::LoadK, {{0, false}, {0, false}}});
  code.instructions.push_back({amber::bytecode::Opcode::MakeList,
                               {{1, false}, {0, false}, {1, false}}});
  code.instructions.push_back({amber::bytecode::Opcode::MakeClosure,
                               {{2, false}, {2, false}, {0, false}}});
  code.instructions.push_back({amber::bytecode::Opcode::Call,
                               {{3, false},
                                {2, false},
                                {0, false},
                                {0, false},
                                {-1, true},
                                {0, false}}});
  code.instructions.push_back(
      {amber::bytecode::Opcode::LoadBool, {{4, false}, {1, false}}});
  code.instructions.push_back({amber::bytecode::Opcode::Safepoint, {}});
  code.instructions.push_back(
      {amber::bytecode::Opcode::JumpIfFalse, {{4, false}, {9, false}}});
  code.instructions.push_back(
      {amber::bytecode::Opcode::LoadBool, {{4, false}, {0, false}}});
  code.instructions.push_back({amber::bytecode::Opcode::Jump, {{5, false}}});
  code.instructions.push_back({amber::bytecode::Opcode::Return, {{1, false}}});
  code.safepoint_table.push_back({5, 0});

  amber::bytecode::BcCode callee;
  callee.code_id = 2;
  callee.kind = amber::bytecode::CodeKind::Block;
  callee.reg_count = 1;
  callee.instructions.push_back(
      {amber::bytecode::Opcode::LoadK, {{0, false}, {0, false}}});
  callee.instructions.push_back(
      {amber::bytecode::Opcode::Return, {{0, false}}});
  module.code_objects = {code, callee};

  amber::mir::Module mir_module;
  mir_module.module_name = "native.boundaries";
  const amber::native::NativeModule native_module =
      amber::native::compile_native_module(module, mir_module);
  const amber::native::NativeValidationResult validation =
      amber::native::validate_native_module(native_module, &module);
  expect(validation.ok(),
         amber::native::diagnostics_to_json(validation.diagnostics));

  const amber::native::NativeCodeObject *native_code =
      native_code_for_bc(native_module, 1);
  expect(native_code != nullptr, "boundary native code object exists");
  expect(has_safepoint_kind(*native_code, "allocation"),
         "native metadata records allocation safepoint");
  expect(has_safepoint_kind(*native_code, "call"),
         "native metadata records call safepoint");
  expect(has_safepoint_kind(*native_code, "backedge"),
         "native metadata records backedge safepoint");
  expect(safepoint_kind_has_root_map(*native_code, "allocation"),
         "allocation safepoint should have a root map");
  expect(safepoint_kind_has_root_map(*native_code, "call"),
         "call safepoint should have a root map");
  expect(safepoint_kind_has_root_map(*native_code, "backedge"),
         "backedge safepoint should have a root map");
}

void test_native_trampoline_safepoint_preserves_heap_argument_root() {
  amber::bytecode::BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};

  amber::bytecode::Constant one;
  one.kind = amber::bytecode::ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  amber::bytecode::BcCode identity;
  identity.code_id = 1;
  identity.kind = amber::bytecode::CodeKind::Method;
  identity.reg_count = 1;
  identity.instructions.push_back({amber::bytecode::Opcode::Safepoint, {}});
  identity.instructions.push_back(
      {amber::bytecode::Opcode::Return, {{0, false}}});
  identity.safepoint_table.push_back({0, 0});

  amber::bytecode::BcCode make_list;
  make_list.code_id = 2;
  make_list.kind = amber::bytecode::CodeKind::Method;
  make_list.reg_count = 2;
  make_list.instructions.push_back(
      {amber::bytecode::Opcode::LoadK, {{0, false}, {0, false}}});
  make_list.instructions.push_back({amber::bytecode::Opcode::MakeList,
                                    {{1, false}, {0, false}, {1, false}}});
  make_list.instructions.push_back(
      {amber::bytecode::Opcode::Return, {{1, false}}});
  module.code_objects = {identity, make_list};

  amber::mir::Module mir_module;
  mir_module.module_name = "native.trampoline_roots";
  const amber::native::NativeModule native_module =
      amber::native::compile_native_module(module, mir_module);
  const amber::native::NativeValidationResult validation =
      amber::native::validate_native_module(native_module, &module);
  expect(validation.ok(),
         amber::native::diagnostics_to_json(validation.diagnostics));
  const amber::native::NativeCodeObject *native_code =
      native_code_for_bc(native_module, 1);
  expect(native_code != nullptr, "trampoline root native code object exists");

  amber::runtime::RuntimeWorld world(module);
  amber::runtime::ExecutionResult made = world.execute(2);
  expect(made.ok() && made.value.is_list(),
         "trampoline root probe should allocate a world list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list = made.value.as_list();

  expect(world.freeze_world().ok(), "trampoline root world should freeze");
  const amber::native::NativeModule bound =
      amber::runtime::bind_native_module_to_world(native_module,
                                                  world.world_mirror());
  world.request_garbage_collection(amber::runtime::RuntimeGcCycle::Full);
  const amber::runtime::ExecutionResult result =
      amber::runtime::execute_native_code(world, bound, native_code->native_id,
                                          {made.value});
  expect(result.ok(), "native trampoline safepoint should execute");
  expect(result.value.is_list() && result.value.as_list() == list,
         "native trampoline should return original heap argument");
  expect(list->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "native trampoline safepoint should preserve argument root");
  const amber::runtime::RuntimeHeapStats stats = world.heap_stats();
  expect(stats.gc_safepoint_collections == 1,
         "native trampoline safepoint should run requested GC");
  expect(stats.gc_reclaimed_objects == 0,
         "native trampoline safepoint should not reclaim argument root");
}

void test_native_validation_rejects_missing_slowpath_metadata() {
  const CompiledArtifacts artifacts = compile_ok("def add(x, y):\n"
                                                 "  x + y\n");
  amber::native::NativeModule broken = artifacts.native_module;
  broken.code_objects[0].slowpath_table.clear();
  const amber::native::NativeValidationResult validation =
      amber::native::validate_native_module(broken, &artifacts.bytecode_module);
  expect(!validation.ok(), "missing slowpath metadata should be rejected");
  expect(has_native_diagnostic(validation, "NATIVE1017") ||
             has_native_diagnostic(validation, "NATIVE1013"),
         "native validation should report missing W15 slowpath metadata");
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
  test_exception_edges_have_native_root_and_slowpath_metadata();
  test_throw_edges_have_native_slowpath_metadata();
  test_native_root_maps_cover_gc_boundary_kinds();
  test_native_trampoline_safepoint_preserves_heap_argument_root();
  test_native_validation_rejects_missing_slowpath_metadata();
  test_native_trampoline_requires_frozen_world_and_executes();
  test_stale_native_assumption_can_fall_back_to_bytecode();
  return 0;
}
