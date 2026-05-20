#pragma once

#include "bytecode/format.h"
#include "optimizer/mir.h"

#include <cstdint>
#include <string>
#include <vector>

namespace amber::native {

struct NativeOwnerAssumption {
  std::uint32_t owner_index = 0;
  std::uint64_t method_version = 0;
};

struct NativeCompileOptions {
  bool requires_frozen_world = true;
  bool emit_jit_patchpoints = true;
  std::uint64_t world_epoch = 0;
  std::uint32_t profile_flags = 0;
  std::vector<NativeOwnerAssumption> owner_assumptions;
};

struct NativeRelocation {
  std::uint32_t offset = 0;
  std::string kind;
  std::string target;
};

struct NativeCallStub {
  std::uint32_t stub_id = 0;
  std::string kind;
  std::uint32_t source_pc = 0;
  std::string helper;
  std::uint32_t selector_symbol_id = 0;
  std::string selector;
  bool reflective = false;
};

struct NativePatchpoint {
  std::uint32_t patchpoint_id = 0;
  std::uint32_t source_pc = 0;
  std::string kind;
  std::string guard;
  std::string action;
  std::uint32_t cache_slot = 0;
  std::uint32_t symbol_id = 0;
  std::string symbol;
};

struct NativeRootMap {
  std::uint32_t ip_offset = 0;
  std::vector<std::uint32_t> local_roots;
  std::vector<std::uint32_t> temp_roots;
  std::vector<std::uint32_t> upvalue_roots;
  std::vector<std::uint32_t> pin_roots;
};

struct NativeExceptionMap {
  std::uint32_t protected_from = 0;
  std::uint32_t protected_to = 0;
  std::uint32_t handler_pc = 0;
  std::uint32_t handler_code_id = 0;
  std::uint32_t flags = 0;
};

struct NativeSafepointMap {
  std::uint32_t ip_offset = 0;
  std::uint32_t flags = 0;
  std::string kind;
};

struct NativeWorldAssumption {
  std::string kind;
  std::uint32_t owner_index = 0;
  std::uint64_t world_epoch = 0;
  std::uint64_t method_version = 0;
};

struct NativeCodeObject {
  std::uint32_t native_id = 0;
  std::uint32_t source_bc_code_id = 0;
  std::string source_code_kind;
  std::string mir_function_id;
  std::string mir_function_name;
  std::string machine_code_blob;
  bool bytecode_trampoline = true;
  bool requires_frozen_world = true;
  std::vector<NativeRelocation> relocation_table;
  std::vector<NativeCallStub> call_stub_table;
  std::vector<NativePatchpoint> patchpoints;
  std::vector<NativeRootMap> root_maps;
  std::vector<NativeExceptionMap> exception_maps;
  std::vector<NativeSafepointMap> safepoint_maps;
  std::vector<NativeWorldAssumption> world_epoch_assumptions;
  std::uint32_t profile_flags = 0;
};

struct NativeModule {
  std::string module_name;
  std::string format = "amber.native.v1";
  bool requires_frozen_world = true;
  std::uint32_t profile_flags = 0;
  std::vector<NativeCodeObject> code_objects;
};

struct NativeDiagnostic {
  std::string code;
  std::string message;
  std::uint32_t native_id = 0;
  std::uint32_t source_bc_code_id = 0;
};

struct NativeValidationResult {
  std::vector<NativeDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

NativeModule compile_native_module(const bytecode::BcModule &bytecode_module,
                                   const mir::Module &mir_module,
                                   const NativeCompileOptions &options = {});

NativeValidationResult
validate_native_module(const NativeModule &module,
                       const bytecode::BcModule *source_module = nullptr);

std::string module_to_json(const NativeModule &module,
                           const std::string &source_hash);
std::string module_to_dump(const NativeModule &module,
                           const std::string &source_hash);
std::string
diagnostics_to_json(const std::vector<NativeDiagnostic> &diagnostics);

} // namespace amber::native
