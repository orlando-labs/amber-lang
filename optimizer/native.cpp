#include "optimizer/native.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace amber::native {

namespace {

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (unsigned char ch : value) {
    switch (ch) {
    case '\\':
      out << "\\\\";
      break;
    case '"':
      out << "\\\"";
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
      if (ch < 0x20U) {
        const char *hex = "0123456789abcdef";
        out << "\\u00" << hex[(ch >> 4U) & 0x0fU] << hex[ch & 0x0fU];
      } else {
        out << static_cast<char>(ch);
      }
      break;
    }
  }
  return out.str();
}

const char *opcode_stub_kind(bytecode::Opcode opcode) {
  switch (opcode) {
  case bytecode::Opcode::Send:
    return "send";
  case bytecode::Opcode::SendDyn:
    return "send_dyn";
  case bytecode::Opcode::Call:
    return "call";
  case bytecode::Opcode::TypeCheck:
    return "type_hook";
  case bytecode::Opcode::TripleEq:
  case bytecode::Opcode::PTripleEq:
    return "pattern_protocol";
  case bytecode::Opcode::Raise:
    return "raise";
  case bytecode::Opcode::Throw:
    return "throw";
  default:
    return "";
  }
}

bool is_call_stub_opcode(bytecode::Opcode opcode) {
  return opcode_stub_kind(opcode)[0] != '\0';
}

bool is_allocation_opcode(bytecode::Opcode opcode) {
  return opcode == bytecode::Opcode::MakeList ||
         opcode == bytecode::Opcode::MakeTuple ||
         opcode == bytecode::Opcode::MakeMap ||
         opcode == bytecode::Opcode::MakeClosure ||
         opcode == bytecode::Opcode::Freeze;
}

bool is_conditional_jump_opcode(bytecode::Opcode opcode) {
  return opcode == bytecode::Opcode::JumpIfTrue ||
         opcode == bytecode::Opcode::JumpIfFalse ||
         opcode == bytecode::Opcode::JumpIfNull;
}

std::string helper_for_stub_kind(const std::string &kind) {
  if (kind == "send") {
    return "amber_runtime_call_packet_send";
  }
  if (kind == "send_dyn") {
    return "amber_runtime_reflective_send_dyn";
  }
  if (kind == "call") {
    return "amber_runtime_call_packet_call";
  }
  if (kind == "type_hook") {
    return "amber_runtime_type_hook";
  }
  if (kind == "pattern_protocol") {
    return "amber_runtime_pattern_protocol";
  }
  if (kind == "raise") {
    return "amber_runtime_raise";
  }
  if (kind == "throw") {
    return "amber_runtime_throw";
  }
  return "amber_runtime_slow_stub";
}

std::string symbol_or_empty(const bytecode::BcModule &module,
                            std::uint32_t symbol_id) {
  if (symbol_id >= module.symbols.size()) {
    return "";
  }
  return module.symbols[symbol_id];
}

const bytecode::CacheSiteEntry *call_site_for_pc(const bytecode::BcCode &code,
                                                 std::uint32_t pc) {
  for (const bytecode::CacheSiteEntry &entry : code.call_site_table) {
    if (entry.pc == pc) {
      return &entry;
    }
  }
  return nullptr;
}

const bytecode::BcCode *source_code_by_id(const bytecode::BcModule &module,
                                          std::uint32_t code_id) {
  for (const bytecode::BcCode &code : module.code_objects) {
    if (code.code_id == code_id) {
      return &code;
    }
  }
  return nullptr;
}

bool has_root_map_for_ip(const NativeCodeObject &code, std::uint32_t ip) {
  for (const NativeRootMap &entry : code.root_maps) {
    if (entry.ip_offset == ip) {
      return true;
    }
  }
  return false;
}

bool has_slowpath_for_stub(const NativeCodeObject &code,
                           const NativeCallStub &stub) {
  for (const NativeSlowPath &entry : code.slowpath_table) {
    if (entry.source_pc == stub.source_pc && entry.helper == stub.helper) {
      return true;
    }
  }
  return false;
}

bool has_invalidation_slowpath(const NativeCodeObject &code) {
  for (const NativeSlowPath &entry : code.slowpath_table) {
    if (entry.kind == "assumption_invalidation" && entry.may_reenter_bytecode) {
      return true;
    }
  }
  return false;
}

std::vector<NativeOwnerAssumption>
owner_assumptions_for(const bytecode::BcModule &module,
                      const NativeCompileOptions &options) {
  if (!options.owner_assumptions.empty()) {
    std::vector<NativeOwnerAssumption> out = options.owner_assumptions;
    std::sort(out.begin(), out.end(),
              [](const NativeOwnerAssumption &left,
                 const NativeOwnerAssumption &right) {
                return left.owner_index < right.owner_index;
              });
    return out;
  }
  std::vector<NativeOwnerAssumption> out;
  out.reserve(module.classes.size());
  for (std::uint32_t index = 0; index < module.classes.size(); ++index) {
    out.push_back({index, 1});
  }
  return out;
}

std::map<std::uint32_t, std::pair<std::string, std::string>>
mir_function_by_entry_code(const bytecode::BcModule &module,
                           const mir::Module &mir_module) {
  std::map<std::uint32_t, std::pair<std::string, std::string>> out;
  for (const bytecode::BcMethod &method : module.methods) {
    const std::string selector =
        symbol_or_empty(module, method.selector_sym_id);
    for (const mir::Function &function : mir_module.functions) {
      if (function.name == selector) {
        out[method.entry_code_id] = {function.id, function.name};
        break;
      }
    }
  }
  return out;
}

void append_unique_safepoint(std::vector<NativeSafepointMap> *maps,
                             std::set<std::uint32_t> *seen, std::uint32_t pc,
                             std::uint32_t flags, std::string kind) {
  if (!seen->insert(pc).second) {
    for (NativeSafepointMap &entry : *maps) {
      if (entry.ip_offset == pc) {
        entry.flags |= flags;
        if (entry.kind.find(kind) == std::string::npos) {
          entry.kind += "+" + kind;
        }
        break;
      }
    }
    return;
  }
  maps->push_back({pc, flags, std::move(kind)});
}

NativeRootMap root_map_for(const bytecode::BcCode &code, std::uint32_t ip) {
  NativeRootMap map;
  map.ip_offset = ip;

  std::set<std::uint32_t> local_slots;
  for (const bytecode::SlotLayoutEntry &entry : code.local_layout) {
    if (entry.slot < code.reg_count) {
      map.local_roots.push_back(entry.slot);
      local_slots.insert(entry.slot);
    }
  }
  for (std::uint32_t reg = 0; reg < code.reg_count; ++reg) {
    if (local_slots.find(reg) == local_slots.end()) {
      map.temp_roots.push_back(reg);
    }
  }
  for (const bytecode::CaptureLayoutEntry &entry : code.capture_layout) {
    map.upvalue_roots.push_back(entry.slot);
  }
  return map;
}

void append_call_stubs(const bytecode::BcModule &module,
                       const bytecode::BcCode &code, NativeCodeObject *out) {
  for (std::uint32_t pc = 0; pc < code.instructions.size(); ++pc) {
    const bytecode::Instruction &instruction = code.instructions[pc];
    if (!is_call_stub_opcode(instruction.opcode)) {
      continue;
    }
    NativeCallStub stub;
    stub.stub_id = static_cast<std::uint32_t>(out->call_stub_table.size());
    stub.kind = opcode_stub_kind(instruction.opcode);
    stub.source_pc = pc;
    stub.helper = helper_for_stub_kind(stub.kind);
    stub.reflective = instruction.opcode == bytecode::Opcode::SendDyn ||
                      instruction.opcode == bytecode::Opcode::TypeCheck ||
                      instruction.opcode == bytecode::Opcode::TripleEq ||
                      instruction.opcode == bytecode::Opcode::PTripleEq;
    if (const bytecode::CacheSiteEntry *site = call_site_for_pc(code, pc)) {
      stub.selector_symbol_id = site->symbol_id;
      stub.selector = symbol_or_empty(module, site->symbol_id);
    }
    out->call_stub_table.push_back(std::move(stub));
  }
}

void append_patchpoints(const bytecode::BcModule &module,
                        const bytecode::BcCode &code, NativeCodeObject *out) {
  for (const bytecode::CacheSiteEntry &entry : code.call_site_table) {
    NativePatchpoint patchpoint;
    patchpoint.patchpoint_id =
        static_cast<std::uint32_t>(out->patchpoints.size());
    patchpoint.source_pc = entry.pc;
    patchpoint.kind = "call_ic";
    patchpoint.guard = "receiver_class+method_version+world_epoch";
    patchpoint.action = "call_stub";
    patchpoint.cache_slot = entry.slot;
    patchpoint.symbol_id = entry.symbol_id;
    patchpoint.symbol = symbol_or_empty(module, entry.symbol_id);
    if (entry.symbol_id < module.symbols.size() &&
        module.symbols[entry.symbol_id] == "<dynamic>") {
      patchpoint.kind = "reflective_send_dyn";
      patchpoint.guard = "world_epoch";
      patchpoint.action = "reflective_slow_stub";
    }
    out->patchpoints.push_back(std::move(patchpoint));
  }
  for (const bytecode::CacheSiteEntry &entry : code.ivar_site_table) {
    NativePatchpoint patchpoint;
    patchpoint.patchpoint_id =
        static_cast<std::uint32_t>(out->patchpoints.size());
    patchpoint.source_pc = entry.pc;
    patchpoint.kind = "ivar_ic";
    patchpoint.guard = "shape_id+shape_version";
    patchpoint.action = "slot_load_store_or_slowpath";
    patchpoint.cache_slot = entry.slot;
    patchpoint.symbol_id = entry.symbol_id;
    patchpoint.symbol = symbol_or_empty(module, entry.symbol_id);
    out->patchpoints.push_back(std::move(patchpoint));
  }
}

std::string slowpath_reason_for_stub_kind(const std::string &kind) {
  if (kind == "send") {
    return "ordinary send dispatch uses runtime call-packet helper";
  }
  if (kind == "send_dyn") {
    return "reflective send remains a runtime slow stub";
  }
  if (kind == "call") {
    return "callable protocol dispatch uses runtime call-packet helper";
  }
  if (kind == "type_hook") {
    return "TypeTerm hook remains a runtime slow stub";
  }
  if (kind == "pattern_protocol") {
    return "dynamic pattern protocol remains a runtime slow stub";
  }
  if (kind == "raise") {
    return "language raise path preserves runtime exception object";
  }
  if (kind == "throw") {
    return "language throw path preserves tagged non-exception unwind";
  }
  return "runtime helper slow path";
}

std::string patchpoint_helper_for_kind(const std::string &kind) {
  if (kind == "ivar_ic") {
    return "amber_runtime_ivar_slowpath";
  }
  if (kind == "reflective_send_dyn") {
    return "amber_runtime_reflective_send_dyn";
  }
  return "amber_runtime_patchpoint_guard_miss";
}

void append_slowpath(NativeCodeObject *out, std::uint32_t source_pc,
                     std::string kind, std::string reason, std::string helper,
                     bool may_reenter_bytecode,
                     bool preserves_language_error = true) {
  NativeSlowPath slowpath;
  slowpath.slowpath_id = static_cast<std::uint32_t>(out->slowpath_table.size());
  slowpath.source_pc = source_pc;
  slowpath.kind = std::move(kind);
  slowpath.reason = std::move(reason);
  slowpath.helper = std::move(helper);
  slowpath.may_reenter_bytecode = may_reenter_bytecode;
  slowpath.preserves_language_error = preserves_language_error;
  out->slowpath_table.push_back(std::move(slowpath));
}

void append_slowpaths(const bytecode::BcCode &code, NativeCodeObject *out) {
  for (const NativeCallStub &stub : out->call_stub_table) {
    append_slowpath(out, stub.source_pc, stub.kind,
                    slowpath_reason_for_stub_kind(stub.kind), stub.helper,
                    false, true);
  }
  for (const NativePatchpoint &patchpoint : out->patchpoints) {
    append_slowpath(out, patchpoint.source_pc, patchpoint.kind + "_miss",
                    "JIT patchpoint guard miss uses runtime helper",
                    patchpoint_helper_for_kind(patchpoint.kind), false, true);
  }
  if (out->requires_frozen_world && !code.instructions.empty()) {
    append_slowpath(
        out, 0, "assumption_invalidation",
        "stale frozen-world assumptions discard native code and re-enter "
        "bytecode at a safe boundary",
        "amber_runtime_reenter_bytecode_after_native_invalidation", true, true);
  }
}

void append_relocations(const bytecode::BcCode &code, NativeCodeObject *out) {
  for (std::uint32_t pc = 0; pc < code.instructions.size(); ++pc) {
    const bytecode::Instruction &instruction = code.instructions[pc];
    if (instruction.opcode == bytecode::Opcode::MakeClosure &&
        instruction.operands.size() >= 2U) {
      out->relocation_table.push_back(
          {pc, "code_ref",
           "bc_code:" + std::to_string(instruction.operands[1].value)});
    }
    if ((instruction.opcode == bytecode::Opcode::Send ||
         instruction.opcode == bytecode::Opcode::SendDyn ||
         instruction.opcode == bytecode::Opcode::Call) &&
        !instruction.operands.empty()) {
      out->relocation_table.push_back({pc, "runtime_stub", "call_packet"});
    }
  }
}

void append_safepoints_and_roots(const bytecode::BcCode &code,
                                 NativeCodeObject *out) {
  std::set<std::uint32_t> seen;
  append_unique_safepoint(&out->safepoint_maps, &seen, 0, 0, "entry");
  for (const bytecode::SafepointEntry &entry : code.safepoint_table) {
    append_unique_safepoint(&out->safepoint_maps, &seen, entry.pc, entry.flags,
                            "explicit");
  }
  for (const bytecode::HandlerEntry &entry : code.handler_table) {
    append_unique_safepoint(&out->safepoint_maps, &seen, entry.protected_from,
                            0, "exception_edge");
    append_unique_safepoint(&out->safepoint_maps, &seen, entry.handler_pc, 0,
                            "exception_handler");
  }
  for (std::uint32_t pc = 0; pc < code.instructions.size(); ++pc) {
    const bytecode::Instruction &instruction = code.instructions[pc];
    if (is_call_stub_opcode(instruction.opcode)) {
      append_unique_safepoint(&out->safepoint_maps, &seen, pc, 0, "call");
    }
    if (is_allocation_opcode(instruction.opcode)) {
      append_unique_safepoint(&out->safepoint_maps, &seen, pc, 0, "allocation");
    }
    if ((instruction.opcode == bytecode::Opcode::Jump ||
         is_conditional_jump_opcode(instruction.opcode)) &&
        !instruction.operands.empty()) {
      const bytecode::InstructionOperand &target = instruction.operands.back();
      if (!target.signed_immediate &&
          target.value <= static_cast<std::int64_t>(pc)) {
        append_unique_safepoint(&out->safepoint_maps, &seen, pc, 0, "backedge");
      }
    }
  }
  std::sort(
      out->safepoint_maps.begin(), out->safepoint_maps.end(),
      [](const NativeSafepointMap &left, const NativeSafepointMap &right) {
        if (left.ip_offset != right.ip_offset) {
          return left.ip_offset < right.ip_offset;
        }
        return left.kind < right.kind;
      });
  for (const NativeSafepointMap &entry : out->safepoint_maps) {
    out->root_maps.push_back(root_map_for(code, entry.ip_offset));
  }
}

void append_exception_maps(const bytecode::BcCode &code,
                           NativeCodeObject *out) {
  for (const bytecode::HandlerEntry &entry : code.handler_table) {
    out->exception_maps.push_back({entry.protected_from, entry.protected_to,
                                   entry.handler_pc, entry.handler_code_id,
                                   entry.flags});
  }
}

void append_world_assumptions(const bytecode::BcModule &module,
                              const NativeCompileOptions &options,
                              NativeCodeObject *out) {
  out->world_epoch_assumptions.push_back(
      {"world_epoch", 0, options.world_epoch, 0});
  for (const NativeOwnerAssumption &owner :
       owner_assumptions_for(module, options)) {
    out->world_epoch_assumptions.push_back(
        {"owner_method_version", owner.owner_index, 0, owner.method_version});
  }
}

void append_diagnostic(std::vector<NativeDiagnostic> *diagnostics,
                       std::string code, std::string message,
                       const NativeCodeObject &object) {
  diagnostics->push_back({std::move(code), std::move(message), object.native_id,
                          object.source_bc_code_id});
}

void append_string_array(std::ostringstream &out,
                         const std::vector<std::uint32_t> &values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << values[i];
  }
  out << "]";
}

} // namespace

NativeModule compile_native_module(const bytecode::BcModule &bytecode_module,
                                   const mir::Module &mir_module,
                                   const NativeCompileOptions &options) {
  NativeModule module;
  module.module_name = mir_module.module_name;
  module.requires_frozen_world = options.requires_frozen_world;
  module.profile_flags = options.profile_flags;

  const std::map<std::uint32_t, std::pair<std::string, std::string>>
      mir_by_code = mir_function_by_entry_code(bytecode_module, mir_module);

  for (const bytecode::BcCode &code : bytecode_module.code_objects) {
    NativeCodeObject object;
    object.native_id = static_cast<std::uint32_t>(module.code_objects.size());
    object.source_bc_code_id = code.code_id;
    object.source_code_kind = bytecode::code_kind_name(code.kind);
    object.machine_code_blob =
        "trampoline:amber.native.v1:bc=" + std::to_string(code.code_id) +
        ":insns=" + std::to_string(code.instructions.size());
    object.bytecode_trampoline = true;
    object.requires_frozen_world = options.requires_frozen_world;
    object.profile_flags = options.profile_flags;

    const auto mir_found = mir_by_code.find(code.code_id);
    if (mir_found != mir_by_code.end()) {
      object.mir_function_id = mir_found->second.first;
      object.mir_function_name = mir_found->second.second;
    }

    append_call_stubs(bytecode_module, code, &object);
    if (options.emit_jit_patchpoints) {
      append_patchpoints(bytecode_module, code, &object);
    }
    append_slowpaths(code, &object);
    append_relocations(code, &object);
    append_safepoints_and_roots(code, &object);
    append_exception_maps(code, &object);
    append_world_assumptions(bytecode_module, options, &object);

    module.code_objects.push_back(std::move(object));
  }
  return module;
}

NativeValidationResult
validate_native_module(const NativeModule &module,
                       const bytecode::BcModule *source_module) {
  NativeValidationResult result;
  std::set<std::uint32_t> native_ids;
  std::set<std::uint32_t> source_ids;
  for (const NativeCodeObject &object : module.code_objects) {
    if (!native_ids.insert(object.native_id).second) {
      append_diagnostic(&result.diagnostics, "NATIVE1001",
                        "duplicate native code object id", object);
    }
    if (!source_ids.insert(object.source_bc_code_id).second) {
      append_diagnostic(&result.diagnostics, "NATIVE1002",
                        "duplicate source bytecode code id", object);
    }
    if (object.machine_code_blob.empty()) {
      append_diagnostic(&result.diagnostics, "NATIVE1003",
                        "native code object has empty machine_code_blob",
                        object);
    }
    if (object.requires_frozen_world &&
        object.world_epoch_assumptions.empty()) {
      append_diagnostic(&result.diagnostics, "NATIVE1004",
                        "frozen native code has no world assumptions", object);
    }
    if (object.safepoint_maps.empty()) {
      append_diagnostic(&result.diagnostics, "NATIVE1005",
                        "native code object has no safepoint maps", object);
    }
    if (object.root_maps.empty()) {
      append_diagnostic(&result.diagnostics, "NATIVE1006",
                        "native code object has no root maps", object);
    }
    for (const NativeSafepointMap &safepoint : object.safepoint_maps) {
      if (!has_root_map_for_ip(object, safepoint.ip_offset)) {
        append_diagnostic(&result.diagnostics, "NATIVE1007",
                          "safepoint has no matching root map", object);
      }
    }
    for (const NativeCallStub &stub : object.call_stub_table) {
      if (stub.helper.empty()) {
        append_diagnostic(&result.diagnostics, "NATIVE1008",
                          "call stub has no runtime helper", object);
      }
      if (!has_slowpath_for_stub(object, stub)) {
        append_diagnostic(&result.diagnostics, "NATIVE1013",
                          "call stub has no matching slowpath", object);
      }
    }
    for (const NativePatchpoint &patchpoint : object.patchpoints) {
      if (patchpoint.guard.empty() || patchpoint.action.empty()) {
        append_diagnostic(&result.diagnostics, "NATIVE1009",
                          "patchpoint must declare guard and action", object);
      }
    }
    for (const NativeSlowPath &slowpath : object.slowpath_table) {
      if (slowpath.helper.empty()) {
        append_diagnostic(&result.diagnostics, "NATIVE1014",
                          "slowpath has no runtime helper", object);
      }
      if (!slowpath.preserves_language_error) {
        append_diagnostic(&result.diagnostics, "NATIVE1018",
                          "slowpath must preserve language errors", object);
      }
    }
    if (object.requires_frozen_world && !has_invalidation_slowpath(object)) {
      append_diagnostic(&result.diagnostics, "NATIVE1017",
                        "frozen native code has no invalidation slowpath",
                        object);
    }
    if (source_module == nullptr) {
      continue;
    }
    const bytecode::BcCode *source =
        source_code_by_id(*source_module, object.source_bc_code_id);
    if (source == nullptr) {
      append_diagnostic(&result.diagnostics, "NATIVE1010",
                        "source bytecode code id is missing", object);
      continue;
    }
    for (const NativeSafepointMap &safepoint : object.safepoint_maps) {
      if (safepoint.ip_offset >= source->instructions.size()) {
        append_diagnostic(&result.diagnostics, "NATIVE1011",
                          "safepoint ip is outside source bytecode", object);
      }
    }
    for (const NativeSlowPath &slowpath : object.slowpath_table) {
      if (slowpath.source_pc >= source->instructions.size()) {
        append_diagnostic(&result.diagnostics, "NATIVE1015",
                          "slowpath ip is outside source bytecode", object);
      }
    }
    for (const NativeExceptionMap &entry : object.exception_maps) {
      if (entry.protected_from >= entry.protected_to ||
          entry.protected_to > source->instructions.size() ||
          entry.handler_pc >= source->instructions.size()) {
        append_diagnostic(&result.diagnostics, "NATIVE1012",
                          "exception map range is invalid", object);
      }
      if (!has_root_map_for_ip(object, entry.handler_pc)) {
        append_diagnostic(&result.diagnostics, "NATIVE1016",
                          "exception handler has no matching root map", object);
      }
    }
  }
  return result;
}

std::string module_to_json(const NativeModule &module,
                           const std::string &source_hash) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"format\": \"" << json_escape(module.format) << "\",\n";
  out << "  \"module\": ";
  if (module.module_name.empty()) {
    out << "null";
  } else {
    out << "\"" << json_escape(module.module_name) << "\"";
  }
  out << ",\n";
  out << "  \"requires_frozen_world\": "
      << (module.requires_frozen_world ? "true" : "false") << ",\n";
  out << "  \"profile_flags\": " << module.profile_flags << ",\n";
  out << "  \"source_hash\": \"" << json_escape(source_hash) << "\",\n";
  out << "  \"code_objects\": [\n";
  for (std::size_t i = 0; i < module.code_objects.size(); ++i) {
    const NativeCodeObject &code = module.code_objects[i];
    if (i != 0U) {
      out << ",\n";
    }
    out << "    {\"native_id\":" << code.native_id
        << ",\"source_bc_code_id\":" << code.source_bc_code_id
        << ",\"source_code_kind\":\"" << json_escape(code.source_code_kind)
        << "\",\"mir_function_id\":";
    if (code.mir_function_id.empty()) {
      out << "null";
    } else {
      out << "\"" << json_escape(code.mir_function_id) << "\"";
    }
    out << ",\"mir_function_name\":";
    if (code.mir_function_name.empty()) {
      out << "null";
    } else {
      out << "\"" << json_escape(code.mir_function_name) << "\"";
    }
    out << ",\"machine_code_blob\":\"" << json_escape(code.machine_code_blob)
        << "\",\"bytecode_trampoline\":"
        << (code.bytecode_trampoline ? "true" : "false")
        << ",\"requires_frozen_world\":"
        << (code.requires_frozen_world ? "true" : "false")
        << ",\"profile_flags\":" << code.profile_flags;

    out << ",\"relocation_table\":[";
    for (std::size_t j = 0; j < code.relocation_table.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      const NativeRelocation &entry = code.relocation_table[j];
      out << "{\"offset\":" << entry.offset << ",\"kind\":\""
          << json_escape(entry.kind) << "\",\"target\":\""
          << json_escape(entry.target) << "\"}";
    }
    out << "]";

    out << ",\"call_stub_table\":[";
    for (std::size_t j = 0; j < code.call_stub_table.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      const NativeCallStub &stub = code.call_stub_table[j];
      out << "{\"stub_id\":" << stub.stub_id << ",\"kind\":\""
          << json_escape(stub.kind) << "\",\"source_pc\":" << stub.source_pc
          << ",\"helper\":\"" << json_escape(stub.helper)
          << "\",\"selector_symbol_id\":" << stub.selector_symbol_id
          << ",\"selector\":\"" << json_escape(stub.selector)
          << "\",\"reflective\":" << (stub.reflective ? "true" : "false")
          << "}";
    }
    out << "]";

    out << ",\"patchpoints\":[";
    for (std::size_t j = 0; j < code.patchpoints.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      const NativePatchpoint &patchpoint = code.patchpoints[j];
      out << "{\"patchpoint_id\":" << patchpoint.patchpoint_id
          << ",\"source_pc\":" << patchpoint.source_pc << ",\"kind\":\""
          << json_escape(patchpoint.kind) << "\",\"guard\":\""
          << json_escape(patchpoint.guard) << "\",\"action\":\""
          << json_escape(patchpoint.action)
          << "\",\"cache_slot\":" << patchpoint.cache_slot
          << ",\"symbol_id\":" << patchpoint.symbol_id << ",\"symbol\":\""
          << json_escape(patchpoint.symbol) << "\"}";
    }
    out << "]";

    out << ",\"slowpath_table\":[";
    for (std::size_t j = 0; j < code.slowpath_table.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      const NativeSlowPath &slowpath = code.slowpath_table[j];
      out << "{\"slowpath_id\":" << slowpath.slowpath_id
          << ",\"source_pc\":" << slowpath.source_pc << ",\"kind\":\""
          << json_escape(slowpath.kind) << "\",\"reason\":\""
          << json_escape(slowpath.reason) << "\",\"helper\":\""
          << json_escape(slowpath.helper) << "\",\"may_reenter_bytecode\":"
          << (slowpath.may_reenter_bytecode ? "true" : "false")
          << ",\"preserves_language_error\":"
          << (slowpath.preserves_language_error ? "true" : "false") << "}";
    }
    out << "]";

    out << ",\"root_maps\":[";
    for (std::size_t j = 0; j < code.root_maps.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      const NativeRootMap &root = code.root_maps[j];
      out << "{\"ip_offset\":" << root.ip_offset << ",\"local_roots\":";
      append_string_array(out, root.local_roots);
      out << ",\"temp_roots\":";
      append_string_array(out, root.temp_roots);
      out << ",\"upvalue_roots\":";
      append_string_array(out, root.upvalue_roots);
      out << ",\"pin_roots\":";
      append_string_array(out, root.pin_roots);
      out << "}";
    }
    out << "]";

    out << ",\"exception_maps\":[";
    for (std::size_t j = 0; j < code.exception_maps.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      const NativeExceptionMap &entry = code.exception_maps[j];
      out << "{\"protected_from\":" << entry.protected_from
          << ",\"protected_to\":" << entry.protected_to
          << ",\"handler_pc\":" << entry.handler_pc
          << ",\"handler_code_id\":" << entry.handler_code_id
          << ",\"flags\":" << entry.flags << "}";
    }
    out << "]";

    out << ",\"safepoint_maps\":[";
    for (std::size_t j = 0; j < code.safepoint_maps.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      const NativeSafepointMap &entry = code.safepoint_maps[j];
      out << "{\"ip_offset\":" << entry.ip_offset
          << ",\"flags\":" << entry.flags << ",\"kind\":\""
          << json_escape(entry.kind) << "\"}";
    }
    out << "]";

    out << ",\"world_epoch_assumptions\":[";
    for (std::size_t j = 0; j < code.world_epoch_assumptions.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      const NativeWorldAssumption &entry = code.world_epoch_assumptions[j];
      out << "{\"kind\":\"" << json_escape(entry.kind)
          << "\",\"owner_index\":" << entry.owner_index
          << ",\"world_epoch\":" << entry.world_epoch
          << ",\"method_version\":" << entry.method_version << "}";
    }
    out << "]}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

std::string module_to_dump(const NativeModule &module,
                           const std::string &source_hash) {
  std::ostringstream out;
  out << "; amber.native.v1 sha256=" << source_hash << "\n";
  out << ".module ";
  if (module.module_name.empty()) {
    out << "<none>";
  } else {
    out << module.module_name;
  }
  out << " frozen=" << (module.requires_frozen_world ? "true" : "false")
      << " profile_flags=" << module.profile_flags << "\n";
  for (const NativeCodeObject &code : module.code_objects) {
    out << "native n" << code.native_id << " bc=c" << code.source_bc_code_id
        << " kind=" << code.source_code_kind
        << " trampoline=" << (code.bytecode_trampoline ? "true" : "false");
    if (!code.mir_function_id.empty()) {
      out << " mir=@" << code.mir_function_id;
    }
    out << "\n";
    out << "  blob \"" << json_escape(code.machine_code_blob) << "\"\n";
    for (const NativeCallStub &stub : code.call_stub_table) {
      out << "  stub s" << stub.stub_id << " pc=" << stub.source_pc
          << " kind=" << stub.kind << " helper=" << stub.helper
          << " reflective=" << (stub.reflective ? "true" : "false");
      if (!stub.selector.empty()) {
        out << " selector=:" << stub.selector;
      }
      out << "\n";
    }
    for (const NativePatchpoint &patchpoint : code.patchpoints) {
      out << "  patch p" << patchpoint.patchpoint_id
          << " pc=" << patchpoint.source_pc << " kind=" << patchpoint.kind
          << " guard=\"" << json_escape(patchpoint.guard) << "\" action=\""
          << json_escape(patchpoint.action) << "\"";
      if (!patchpoint.symbol.empty()) {
        out << " symbol=:" << patchpoint.symbol;
      }
      out << "\n";
    }
    for (const NativeSlowPath &slowpath : code.slowpath_table) {
      out << "  slowpath q" << slowpath.slowpath_id
          << " pc=" << slowpath.source_pc << " kind=" << slowpath.kind
          << " helper=" << slowpath.helper << " reenter_bytecode="
          << (slowpath.may_reenter_bytecode ? "true" : "false")
          << " preserves_error="
          << (slowpath.preserves_language_error ? "true" : "false")
          << " reason=\"" << json_escape(slowpath.reason) << "\"\n";
    }
    for (const NativeSafepointMap &safepoint : code.safepoint_maps) {
      out << "  safepoint pc=" << safepoint.ip_offset
          << " kind=" << safepoint.kind << " flags=" << safepoint.flags << "\n";
    }
    for (const NativeRootMap &root : code.root_maps) {
      out << "  roots pc=" << root.ip_offset
          << " locals=" << root.local_roots.size()
          << " temps=" << root.temp_roots.size()
          << " upvalues=" << root.upvalue_roots.size()
          << " pins=" << root.pin_roots.size() << "\n";
    }
    for (const NativeWorldAssumption &assumption :
         code.world_epoch_assumptions) {
      out << "  assume " << assumption.kind;
      if (assumption.kind == "world_epoch") {
        out << " epoch=" << assumption.world_epoch;
      } else {
        out << " owner=" << assumption.owner_index
            << " method_version=" << assumption.method_version;
      }
      out << "\n";
    }
  }
  return out.str();
}

std::string
diagnostics_to_json(const std::vector<NativeDiagnostic> &diagnostics) {
  std::ostringstream out;
  out << "{\n  \"format\": \"amber.native.diagnostics.v1\",\n";
  out << "  \"errors\": [\n";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    if (i != 0U) {
      out << ",\n";
    }
    const NativeDiagnostic &diagnostic = diagnostics[i];
    out << "    {\"code\":\"" << json_escape(diagnostic.code)
        << "\",\"message\":\"" << json_escape(diagnostic.message)
        << "\",\"native_id\":" << diagnostic.native_id
        << ",\"source_bc_code_id\":" << diagnostic.source_bc_code_id << "}";
  }
  out << "\n  ]\n}\n";
  return out.str();
}

} // namespace amber::native
