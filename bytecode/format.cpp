#include "bytecode/format.h"

#include "frontend/lexer/token.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amber::bytecode {

namespace {

struct SectionPayload {
  SectionKind kind;
  std::vector<std::uint8_t> bytes;
  std::uint32_t align = 1;
  std::uint32_t flags = 0;
};

bool constant_is_path_ref(const BcModule &module, std::uint32_t ref_id);

struct Reader {
  const std::vector<std::uint8_t> &bytes;
  std::size_t pos = 0;
  std::size_t end = 0;
  std::uint64_t base_offset = 0;
  const char *section = "";
  std::vector<VerifyError> *errors = nullptr;

  void fail(const std::string &code, const std::string &message,
            std::uint64_t offset = 0) {
    if (errors == nullptr) {
      return;
    }
    errors->push_back({code, message, section, base_offset + offset});
  }

  bool remaining(std::size_t need) const {
    return pos <= end && need <= end - pos;
  }

  bool read_u8(std::uint8_t &value) {
    if (!remaining(1)) {
      fail("BC1002", "unexpected end of file", pos);
      return false;
    }
    value = bytes[pos++];
    return true;
  }

  bool read_u16(std::uint16_t &value) {
    if (!remaining(2)) {
      fail("BC1002", "unexpected end of file", pos);
      return false;
    }
    value = static_cast<std::uint16_t>(bytes[pos]) |
            (static_cast<std::uint16_t>(bytes[pos + 1U]) << 8U);
    pos += 2U;
    return true;
  }

  bool read_u32(std::uint32_t &value) {
    if (!remaining(4)) {
      fail("BC1002", "unexpected end of file", pos);
      return false;
    }
    value = static_cast<std::uint32_t>(bytes[pos]) |
            (static_cast<std::uint32_t>(bytes[pos + 1U]) << 8U) |
            (static_cast<std::uint32_t>(bytes[pos + 2U]) << 16U) |
            (static_cast<std::uint32_t>(bytes[pos + 3U]) << 24U);
    pos += 4U;
    return true;
  }

  bool read_u64(std::uint64_t &value) {
    if (!remaining(8)) {
      fail("BC1002", "unexpected end of file", pos);
      return false;
    }
    value = static_cast<std::uint64_t>(bytes[pos]) |
            (static_cast<std::uint64_t>(bytes[pos + 1U]) << 8U) |
            (static_cast<std::uint64_t>(bytes[pos + 2U]) << 16U) |
            (static_cast<std::uint64_t>(bytes[pos + 3U]) << 24U) |
            (static_cast<std::uint64_t>(bytes[pos + 4U]) << 32U) |
            (static_cast<std::uint64_t>(bytes[pos + 5U]) << 40U) |
            (static_cast<std::uint64_t>(bytes[pos + 6U]) << 48U) |
            (static_cast<std::uint64_t>(bytes[pos + 7U]) << 56U);
    pos += 8U;
    return true;
  }

  bool read_bytes(std::size_t count, std::vector<std::uint8_t> &value) {
    if (!remaining(count)) {
      fail("BC1002", "unexpected end of file", pos);
      return false;
    }
    value.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                 bytes.begin() + static_cast<std::ptrdiff_t>(pos + count));
    pos += count;
    return true;
  }

  bool read_string(std::string &value) {
    std::uint32_t size = 0;
    if (!read_u32(size)) {
      return false;
    }
    if (!remaining(size)) {
      fail("BC1002", "unexpected end of file", pos);
      return false;
    }
    value.assign(reinterpret_cast<const char *>(&bytes[pos]), size);
    pos += size;
    return true;
  }

  bool read_uleb(std::uint64_t &value) {
    value = 0;
    std::uint32_t shift = 0;
    while (true) {
      std::uint8_t byte = 0;
      if (!read_u8(byte)) {
        return false;
      }
      value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
      if ((byte & 0x80U) == 0U) {
        return true;
      }
      shift += 7U;
      if (shift >= 64U) {
        fail("BC1006", "ULEB128 operand is too large", pos - 1U);
        return false;
      }
    }
  }

  bool read_sleb(std::int64_t &value) {
    value = 0;
    std::uint32_t shift = 0;
    std::uint8_t byte = 0;
    while (true) {
      if (!read_u8(byte)) {
        return false;
      }
      value |= static_cast<std::int64_t>(byte & 0x7fU) << shift;
      shift += 7U;
      if ((byte & 0x80U) == 0U) {
        break;
      }
      if (shift >= 64U) {
        fail("BC1007", "SLEB128 operand is too large", pos - 1U);
        return false;
      }
    }
    if (shift < 64U && (byte & 0x40U) != 0U) {
      value |= -((static_cast<std::int64_t>(1) << shift));
    }
    return true;
  }
};

void append_u8(std::vector<std::uint8_t> &out, std::uint8_t value) {
  out.push_back(value);
}

void append_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xffU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xffU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void append_u32_array(std::vector<std::uint8_t> &out,
                      const std::vector<std::uint32_t> &values) {
  append_u32(out, static_cast<std::uint32_t>(values.size()));
  for (std::uint32_t value : values) {
    append_u32(out, value);
  }
}

void append_u64(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void append_string(std::vector<std::uint8_t> &out, const std::string &value) {
  append_u32(out, static_cast<std::uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

void append_bytes(std::vector<std::uint8_t> &out,
                  const std::vector<std::uint8_t> &value) {
  out.insert(out.end(), value.begin(), value.end());
}

void append_uleb(std::vector<std::uint8_t> &out, std::uint64_t value) {
  do {
    std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7fU);
    value >>= 7U;
    if (value != 0) {
      byte |= 0x80U;
    }
    out.push_back(byte);
  } while (value != 0);
}

void append_sleb(std::vector<std::uint8_t> &out, std::int64_t value) {
  bool more = true;
  while (more) {
    std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7f);
    const bool sign_bit = (byte & 0x40U) != 0U;
    value >>= 7;
    more = !((value == 0 && !sign_bit) || (value == -1 && sign_bit));
    if (more) {
      byte |= 0x80U;
    }
    out.push_back(byte);
  }
}

std::uint64_t align_up(std::uint64_t value, std::uint32_t align) {
  if (align <= 1U) {
    return value;
  }
  const std::uint64_t remainder = value % align;
  if (remainder == 0U) {
    return value;
  }
  return value + (align - remainder);
}

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
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<int>(ch) << std::dec << std::setfill(' ');
      } else {
        out << static_cast<char>(ch);
      }
      break;
    }
  }
  return out.str();
}

std::string bytes_to_hex(const std::vector<std::uint8_t> &bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::uint8_t byte : bytes) {
    out << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return out.str();
}

std::string bytes_to_hex(const std::array<std::uint8_t, 32> &bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::uint8_t byte : bytes) {
    out << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return out.str();
}

const char *section_tag(SectionKind kind) {
  switch (kind) {
  case SectionKind::Strs:
    return "STRS";
  case SectionKind::Syms:
    return "SYMS";
  case SectionKind::Kons:
    return "KONS";
  case SectionKind::Code:
    return "CODE";
  case SectionKind::Meth:
    return "METH";
  case SectionKind::Clas:
    return "CLAS";
  case SectionKind::Deps:
    return "DEPS";
  case SectionKind::Expt:
    return "EXPT";
  case SectionKind::Init:
    return "INIT";
  case SectionKind::Pats:
    return "PATS";
  case SectionKind::Span:
    return "SPAN";
  case SectionKind::Line:
    return "LINE";
  case SectionKind::Locs:
    return "LOCS";
  case SectionKind::Attr:
    return "ATTR";
  case SectionKind::Prof:
    return "PROF";
  case SectionKind::Caps:
    return "CAPS";
  case SectionKind::Efct:
    return "EFCT";
  case SectionKind::Obsv:
    return "OBSV";
  case SectionKind::Rply:
    return "RPLY";
  case SectionKind::Scma:
    return "SCMA";
  case SectionKind::Tabl:
    return "TABL";
  case SectionKind::Wasm:
    return "WASM";
  case SectionKind::Accl:
    return "ACCL";
  case SectionKind::Agnt:
    return "AGNT";
  case SectionKind::Cntr:
    return "CNTR";
  case SectionKind::Priv:
    return "PRIV";
  case SectionKind::Wflw:
    return "WFLW";
  case SectionKind::Hash:
    return "HASH";
  }
  return "UNKN";
}

bool decode_section_kind(const std::array<char, 4> &tag, SectionKind &kind) {
  const std::string value(tag.begin(), tag.end());
  if (value == "STRS") {
    kind = SectionKind::Strs;
    return true;
  }
  if (value == "SYMS") {
    kind = SectionKind::Syms;
    return true;
  }
  if (value == "KONS") {
    kind = SectionKind::Kons;
    return true;
  }
  if (value == "CODE") {
    kind = SectionKind::Code;
    return true;
  }
  if (value == "METH") {
    kind = SectionKind::Meth;
    return true;
  }
  if (value == "CLAS") {
    kind = SectionKind::Clas;
    return true;
  }
  if (value == "DEPS") {
    kind = SectionKind::Deps;
    return true;
  }
  if (value == "EXPT") {
    kind = SectionKind::Expt;
    return true;
  }
  if (value == "INIT") {
    kind = SectionKind::Init;
    return true;
  }
  if (value == "PATS") {
    kind = SectionKind::Pats;
    return true;
  }
  if (value == "SPAN") {
    kind = SectionKind::Span;
    return true;
  }
  if (value == "LINE") {
    kind = SectionKind::Line;
    return true;
  }
  if (value == "LOCS") {
    kind = SectionKind::Locs;
    return true;
  }
  if (value == "ATTR") {
    kind = SectionKind::Attr;
    return true;
  }
  if (value == "PROF") {
    kind = SectionKind::Prof;
    return true;
  }
  if (value == "CAPS") {
    kind = SectionKind::Caps;
    return true;
  }
  if (value == "EFCT") {
    kind = SectionKind::Efct;
    return true;
  }
  if (value == "OBSV") {
    kind = SectionKind::Obsv;
    return true;
  }
  if (value == "RPLY") {
    kind = SectionKind::Rply;
    return true;
  }
  if (value == "SCMA") {
    kind = SectionKind::Scma;
    return true;
  }
  if (value == "TABL") {
    kind = SectionKind::Tabl;
    return true;
  }
  if (value == "WASM") {
    kind = SectionKind::Wasm;
    return true;
  }
  if (value == "ACCL") {
    kind = SectionKind::Accl;
    return true;
  }
  if (value == "AGNT") {
    kind = SectionKind::Agnt;
    return true;
  }
  if (value == "CNTR") {
    kind = SectionKind::Cntr;
    return true;
  }
  if (value == "PRIV") {
    kind = SectionKind::Priv;
    return true;
  }
  if (value == "WFLW") {
    kind = SectionKind::Wflw;
    return true;
  }
  if (value == "HASH") {
    kind = SectionKind::Hash;
    return true;
  }
  return false;
}

std::vector<SectionKind> required_sections() {
  return {SectionKind::Strs, SectionKind::Syms, SectionKind::Kons,
          SectionKind::Code, SectionKind::Meth, SectionKind::Clas,
          SectionKind::Deps, SectionKind::Expt, SectionKind::Init};
}

bool optional_section_present(const BcModule &module, SectionKind kind) {
  switch (kind) {
  case SectionKind::Pats:
    return !module.pattern_programs.empty();
  case SectionKind::Span:
    for (const BcCode &code : module.code_objects) {
      if (!code.source_spans.empty()) {
        return true;
      }
    }
    return false;
  case SectionKind::Line:
    return !module.line_table.empty();
  case SectionKind::Locs:
    return !module.local_debug.empty();
  case SectionKind::Attr:
    return !module.attrs.empty();
  case SectionKind::Prof:
    return !module.required_features.empty() ||
           !module.optional_features.empty() ||
           !module.forbidden_features.empty();
  case SectionKind::Caps:
    return !module.capabilities.empty();
  case SectionKind::Efct:
    return !module.effects.empty();
  case SectionKind::Obsv:
    return !module.observability_sites.empty();
  case SectionKind::Rply:
    return !module.replay_metadata.required_event_names.empty() ||
           !module.replay_metadata.deterministic_sources.empty() ||
           module.replay_metadata.flags != 0U;
  case SectionKind::Scma:
    return !module.schemas.empty() || !module.schema_migrations.empty();
  case SectionKind::Tabl:
    return !module.table_plans.empty();
  case SectionKind::Wasm:
    return !module.wasm_components.empty();
  case SectionKind::Accl:
    return !module.accelerator_kernels.empty();
  case SectionKind::Agnt:
    return !module.agent_symbols.empty() || !module.agent_patches.empty() ||
           !module.provenance_records.empty();
  case SectionKind::Cntr:
    return !module.contracts.empty() || !module.properties.empty();
  case SectionKind::Priv:
    return !module.privacy_labels.empty() || !module.privacy_policies.empty() ||
           !module.lineage_nodes.empty();
  case SectionKind::Wflw:
    return !module.workflow_steps.empty() || !module.workflow_history.empty();
  case SectionKind::Hash:
    return !module.hashes.empty();
  default:
    return true;
  }
}

std::vector<std::uint8_t>
serialize_strings(const std::vector<std::string> &strings) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(strings.size()));
  for (const std::string &value : strings) {
    append_string(out, value);
  }
  return out;
}

std::vector<std::uint8_t>
serialize_constants(const std::vector<Constant> &constants) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(constants.size()));
  for (const Constant &constant : constants) {
    append_u8(out, static_cast<std::uint8_t>(constant.kind));
    switch (constant.kind) {
    case ConstantKind::Null:
      break;
    case ConstantKind::Bool:
      append_u8(out, constant.bool_value ? 1U : 0U);
      break;
    case ConstantKind::Integer:
      append_sleb(out, constant.int_value);
      break;
    case ConstantKind::Float: {
      std::uint64_t bits = 0;
      std::memcpy(&bits, &constant.float_value, sizeof(bits));
      append_u64(out, bits);
      break;
    }
    case ConstantKind::SymbolRef:
    case ConstantKind::StringRef:
    case ConstantKind::CodeRef:
      append_u32(out, constant.ref_id);
      break;
    case ConstantKind::KeySet:
    case ConstantKind::Path:
      append_u32(out, static_cast<std::uint32_t>(constant.items.size()));
      for (std::uint32_t item : constant.items) {
        append_u32(out, item);
      }
      break;
    }
  }
  return out;
}

std::vector<std::uint8_t> serialize_code(const std::vector<BcCode> &codes) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(codes.size()));
  for (const BcCode &code : codes) {
    append_u32(out, code.code_id);
    append_u8(out, static_cast<std::uint8_t>(code.kind));
    append_u32(out, code.reg_count);
    append_u32(out, code.flags);

    append_u32(out, static_cast<std::uint32_t>(code.local_layout.size()));
    for (const SlotLayoutEntry &entry : code.local_layout) {
      append_u32(out, entry.slot);
      append_u32(out, entry.name_str_id);
      append_u32(out, entry.role_str_id);
      append_u32(out, entry.binding_kind_str_id);
    }

    append_u32(out, static_cast<std::uint32_t>(code.capture_layout.size()));
    for (const CaptureLayoutEntry &entry : code.capture_layout) {
      append_u32(out, entry.slot);
      append_u32(out, entry.name_str_id);
      append_u32(out, entry.source_kind_str_id);
      append_u32(out, entry.source_name_str_id);
    }

    append_u32(out, static_cast<std::uint32_t>(code.instructions.size()));
    for (const Instruction &instruction : code.instructions) {
      append_u8(out, static_cast<std::uint8_t>(instruction.opcode));
      append_u32(out, static_cast<std::uint32_t>(instruction.operands.size()));
      for (const InstructionOperand &operand : instruction.operands) {
        append_u8(out, operand.signed_immediate ? 1U : 0U);
        if (operand.signed_immediate) {
          append_sleb(out, operand.value);
        } else {
          append_uleb(out, static_cast<std::uint64_t>(operand.value));
        }
      }
    }

    append_u32(out, static_cast<std::uint32_t>(code.handler_table.size()));
    for (const HandlerEntry &entry : code.handler_table) {
      append_u32(out, entry.protected_from);
      append_u32(out, entry.protected_to);
      append_u32(out, entry.handler_pc);
      append_u32(out, entry.handler_code_id);
      append_u32(out, entry.flags);
    }

    append_u32(out, static_cast<std::uint32_t>(code.call_site_table.size()));
    for (const CacheSiteEntry &entry : code.call_site_table) {
      append_u32(out, entry.pc);
      append_u32(out, entry.slot);
      append_u32(out, entry.symbol_id);
      append_u32(out, entry.flags);
    }

    append_u32(out, static_cast<std::uint32_t>(code.ivar_site_table.size()));
    for (const CacheSiteEntry &entry : code.ivar_site_table) {
      append_u32(out, entry.pc);
      append_u32(out, entry.slot);
      append_u32(out, entry.symbol_id);
      append_u32(out, entry.flags);
    }

    append_u32(out, static_cast<std::uint32_t>(code.safepoint_table.size()));
    for (const SafepointEntry &entry : code.safepoint_table) {
      append_u32(out, entry.pc);
      append_u32(out, entry.flags);
    }
  }
  return out;
}

std::vector<std::uint8_t>
serialize_methods(const std::vector<BcMethod> &methods) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(methods.size()));
  for (const BcMethod &method : methods) {
    append_u32(out, method.selector_sym_id);
    append_u32(out, method.owner_dispatch_ref);
    append_u32(out, method.signature_blob_id);

    append_u32(out, static_cast<std::uint32_t>(method.params.size()));
    for (const MethodParamEntry &entry : method.params) {
      append_u32(out, entry.external_name_sym_id);
      append_u32(out, entry.local_name_str_id);
      append_u32(out, entry.flags);
    }

    append_u32(out,
               static_cast<std::uint32_t>(method.default_thunk_ids.size()));
    for (std::uint32_t code_id : method.default_thunk_ids) {
      append_u32(out, code_id);
    }

    append_u32(out, static_cast<std::uint32_t>(method.type_hook_ids.size()));
    for (std::uint32_t code_id : method.type_hook_ids) {
      append_u32(out, code_id);
    }

    append_u32(out, static_cast<std::uint32_t>(method.clause_table.size()));
    for (const ClauseEntry &entry : method.clause_table) {
      append_u32(out, entry.pattern_program_id);
      append_u32(out, entry.pattern_code_id);
      append_u32(out, entry.guard_code_id);
      append_u32(out, entry.body_code_id);
      append_u32(out, entry.flags);
    }

    append_u32(out, static_cast<std::uint32_t>(method.auto_assign_desc.size()));
    for (const AutoAssignEntry &entry : method.auto_assign_desc) {
      append_u32(out, entry.local_name_str_id);
      append_u32(out, entry.target_name_str_id);
      append_u32(out, entry.flags);
    }

    append_u32(out, method.entry_code_id);
    append_u32(out, method.flags);
  }
  return out;
}

std::vector<std::uint8_t>
serialize_classes(const std::vector<BcClass> &classes) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(classes.size()));
  for (const BcClass &klass : classes) {
    append_u32(out, klass.class_name_sym_id);
    append_u8(out, klass.has_superclass_ref ? 1U : 0U);
    append_u32(out, klass.superclass_ref);
    append_u32(out, klass.ivar_schema_id);
    append_u32(out, klass.method_range_start);
    append_u32(out, klass.method_range_count);
    append_u32_array(out, klass.direct_include_refs);
    append_u32_array(out, klass.direct_extend_refs);
    append_u32(out, klass.flags);
    append_u8(out, klass.has_class_init_code_id ? 1U : 0U);
    append_u32(out, klass.class_init_code_id);
  }
  return out;
}

std::vector<std::uint8_t>
serialize_dependencies(const std::vector<DepEntry> &dependencies) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(dependencies.size()));
  for (const DepEntry &entry : dependencies) {
    append_u32(out, entry.module_name_str_id);
    append_u16(out, entry.required_format.major);
    append_u16(out, entry.required_format.minor);
    append_u16(out, entry.min_language_version.major);
    append_u16(out, entry.min_language_version.minor);
    append_u8(out, entry.has_max_language_version ? 1U : 0U);
    append_u16(out, entry.max_language_version.major);
    append_u16(out, entry.max_language_version.minor);
    append_u8(out, entry.has_abi_requirement ? 1U : 0U);
    out.insert(out.end(), entry.abi_requirement.begin(),
               entry.abi_requirement.end());
    append_u32(out, entry.flags);
  }
  return out;
}

std::vector<std::uint8_t>
serialize_exports(const std::vector<ExportEntry> &exports) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(exports.size()));
  for (const ExportEntry &entry : exports) {
    append_u32(out, entry.symbol_id);
    append_u32(out, entry.target_kind_str_id);
    append_u32(out, entry.target_index);
    append_u32(out, entry.visibility_flags);
    append_u8(out, entry.has_reexport_module_name ? 1U : 0U);
    append_u32(out, entry.reexport_module_name_str_id);
  }
  return out;
}

std::vector<std::uint8_t> serialize_init(const InitEntry &entry) {
  std::vector<std::uint8_t> out;
  append_u8(out, entry.has_entry_code_id ? 1U : 0U);
  append_u32(out, entry.entry_code_id);
  append_u32(out, entry.flags);
  return out;
}

std::vector<std::uint8_t>
serialize_pattern_programs(const std::vector<PatternProgramEntry> &programs) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(programs.size()));
  for (const PatternProgramEntry &entry : programs) {
    append_u32(out, entry.pattern_id);
    append_u32(out, entry.binding_count);
    append_u32(out, entry.flags);
  }
  return out;
}

std::vector<std::uint8_t> serialize_spans(const std::vector<BcCode> &codes) {
  std::vector<std::uint8_t> out;
  std::uint32_t count = 0;
  for (const BcCode &code : codes) {
    count += static_cast<std::uint32_t>(code.source_spans.size());
  }
  append_u32(out, count);
  for (const BcCode &code : codes) {
    for (const SourceSpanEntry &entry : code.source_spans) {
      append_u32(out, code.code_id);
      append_u32(out, entry.pc_from);
      append_u32(out, entry.pc_to);
      append_string(out, entry.span.file);
      append_u32(out, static_cast<std::uint32_t>(entry.span.start.line));
      append_u32(out, static_cast<std::uint32_t>(entry.span.start.col));
      append_u32(out, static_cast<std::uint32_t>(entry.span.start.offset));
      append_u32(out, static_cast<std::uint32_t>(entry.span.end.line));
      append_u32(out, static_cast<std::uint32_t>(entry.span.end.col));
      append_u32(out, static_cast<std::uint32_t>(entry.span.end.offset));
    }
  }
  return out;
}

std::vector<std::uint8_t>
serialize_line_table(const std::vector<LineEntry> &lines) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(lines.size()));
  for (const LineEntry &entry : lines) {
    append_u32(out, entry.code_id);
    append_u32(out, entry.pc);
    append_u32(out, entry.line);
  }
  return out;
}

std::vector<std::uint8_t>
serialize_local_debug(const std::vector<LocalDebugEntry> &locals) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(locals.size()));
  for (const LocalDebugEntry &entry : locals) {
    append_u32(out, entry.code_id);
    append_u32(out, entry.slot);
    append_u32(out, entry.name_str_id);
    append_u32(out, entry.start_pc);
    append_u32(out, entry.end_pc);
  }
  return out;
}

std::vector<std::uint8_t> serialize_attrs(const std::vector<AttrEntry> &attrs) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(attrs.size()));
  for (const AttrEntry &entry : attrs) {
    append_u32(out, entry.key_str_id);
    append_u32(out, entry.value_str_id);
  }
  return out;
}

void append_profile_vector(std::vector<std::uint8_t> &out,
                           const std::vector<std::string> &values) {
  append_u32(out, static_cast<std::uint32_t>(values.size()));
  for (const std::string &value : values) {
    append_string(out, value);
  }
}

std::vector<std::uint8_t>
serialize_profile_metadata(const std::vector<std::string> &required,
                           const std::vector<std::string> &optional,
                           const std::vector<std::string> &forbidden) {
  std::vector<std::uint8_t> out;
  append_profile_vector(out, required);
  append_profile_vector(out, optional);
  append_profile_vector(out, forbidden);
  return out;
}

std::vector<std::uint8_t> serialize_capabilities(
    const std::vector<capability::CapabilityRequest> &capabilities) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(capabilities.size()));
  for (const capability::CapabilityRequest &entry : capabilities) {
    append_string(out, entry.name);
    append_string(out, entry.target);
    append_string(out, entry.reason);
    append_u32(out, entry.flags);
  }
  return out;
}

std::vector<std::uint8_t>
serialize_effects(const std::vector<effect::EffectSummary> &effects) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(effects.size()));
  for (const effect::EffectSummary &entry : effects) {
    append_string(out, entry.owner);
    append_string(out, entry.kind);
    const std::vector<std::string> declared =
        effect::normalize_effects(entry.declared_effects);
    append_u32(out, static_cast<std::uint32_t>(declared.size()));
    for (const std::string &label : declared) {
      append_string(out, label);
    }
    const std::vector<std::string> observed =
        effect::normalize_effects(entry.observed_effects);
    append_u32(out, static_cast<std::uint32_t>(observed.size()));
    for (const std::string &label : observed) {
      append_string(out, label);
    }
    append_u32(out, entry.flags);
  }
  return out;
}

std::vector<std::uint8_t> serialize_observability_sites(
    const std::vector<replay::ObservabilitySite> &sites) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(sites.size()));
  for (const replay::ObservabilitySite &entry : sites) {
    append_u32(out, entry.site_id);
    append_string(out, entry.event_name);
    append_string(out, entry.kind);
    append_string(out, entry.owner);
    append_string(out, entry.source.file);
    append_u32(out, entry.source.line);
    append_u32(out, entry.source.column);
    append_u32(out, entry.flags);
  }
  return out;
}

std::vector<std::uint8_t>
serialize_replay_metadata(const replay::ReplayMetadata &metadata) {
  const replay::ReplayMetadata normalized =
      replay::normalize_metadata(metadata);
  std::vector<std::uint8_t> out;
  append_u32(out, normalized.flags);
  append_u32(
      out, static_cast<std::uint32_t>(normalized.required_event_names.size()));
  for (const std::string &event_name : normalized.required_event_names) {
    append_string(out, event_name);
  }
  append_u32(
      out, static_cast<std::uint32_t>(normalized.deterministic_sources.size()));
  for (const std::string &source : normalized.deterministic_sources) {
    append_string(out, source);
  }
  return out;
}

std::vector<std::uint8_t> serialize_schema_metadata(
    const std::vector<data::SchemaDefinition> &schemas,
    const std::vector<data::SchemaMigration> &migrations) {
  const data::SchemaValidationResult validated =
      data::validate_schemas(schemas, migrations);
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(validated.schemas.size()));
  for (const data::SchemaDefinition &schema : validated.schemas) {
    append_string(out, schema.name);
    append_u32(out, schema.version);
    append_u32(out, schema.flags);
    append_u32(out, static_cast<std::uint32_t>(schema.fields.size()));
    for (const data::SchemaField &field : schema.fields) {
      append_string(out, field.name);
      append_string(out, field.type);
      append_u8(out, field.required ? 1U : 0U);
      append_u8(out, field.nullable ? 1U : 0U);
      append_string(out, field.default_value);
      append_u32(out, field.flags);
    }
  }
  append_u32(out, static_cast<std::uint32_t>(validated.migrations.size()));
  for (const data::SchemaMigration &migration : validated.migrations) {
    append_string(out, migration.schema_name);
    append_u32(out, migration.from_version);
    append_u32(out, migration.to_version);
    append_string(out, migration.kind);
    append_u32(out, migration.flags);
  }
  return out;
}

std::vector<std::uint8_t>
serialize_table_plans(const std::vector<data::TablePlan> &plans) {
  const data::TablePlanValidationResult validated =
      data::validate_table_plans(plans);
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(validated.plans.size()));
  for (const data::TablePlan &plan : validated.plans) {
    append_string(out, plan.plan_id);
    append_string(out, plan.op);
    append_u32(out, plan.flags);
    append_u32(out, static_cast<std::uint32_t>(plan.input_refs.size()));
    for (const std::string &input : plan.input_refs) {
      append_string(out, input);
    }
    append_u32(out, static_cast<std::uint32_t>(plan.arguments.size()));
    for (const std::string &argument : plan.arguments) {
      append_string(out, argument);
    }
    append_u32(out,
               static_cast<std::uint32_t>(plan.column_dependencies.size()));
    for (const data::ColumnDependency &dependency : plan.column_dependencies) {
      append_string(out, dependency.table_ref);
      append_string(out, dependency.column_ref);
    }
    append_u32(out, static_cast<std::uint32_t>(plan.effect_row.size()));
    for (const std::string &effect_label : plan.effect_row) {
      append_string(out, effect_label);
    }
  }
  return out;
}

std::vector<std::uint8_t> serialize_wasm_components(
    const std::vector<wasm_accel::WasmComponent> &components) {
  std::vector<wasm_accel::WasmComponent> normalized;
  normalized.reserve(components.size());
  for (wasm_accel::WasmComponent component : components) {
    normalized.push_back(
        wasm_accel::normalize_wasm_component(std::move(component)));
  }
  std::sort(normalized.begin(), normalized.end(),
            [](const wasm_accel::WasmComponent &left,
               const wasm_accel::WasmComponent &right) {
              return left.name < right.name;
            });

  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(normalized.size()));
  for (const wasm_accel::WasmComponent &component : normalized) {
    append_string(out, component.name);
    append_string(out, component.world);
    append_u32(out, component.flags);

    append_u32(out, static_cast<std::uint32_t>(component.imports.size()));
    for (const wasm_accel::WasmInterfaceEntry &entry : component.imports) {
      append_string(out, entry.name);
      append_string(out, entry.kind);
      append_string(out, entry.type_signature);
      append_string(out, entry.schema_name);
      append_string(out, entry.capability.name);
      append_string(out, entry.capability.target);
      append_string(out, entry.capability.reason);
      append_u32(out, entry.capability.flags);
      append_u32(out, static_cast<std::uint32_t>(entry.effect_row.size()));
      for (const std::string &label : entry.effect_row) {
        append_string(out, label);
      }
      append_u32(out, entry.flags);
    }

    append_u32(out, static_cast<std::uint32_t>(component.exports.size()));
    for (const wasm_accel::WasmInterfaceEntry &entry : component.exports) {
      append_string(out, entry.name);
      append_string(out, entry.kind);
      append_string(out, entry.type_signature);
      append_string(out, entry.schema_name);
      append_string(out, entry.capability.name);
      append_string(out, entry.capability.target);
      append_string(out, entry.capability.reason);
      append_u32(out, entry.capability.flags);
      append_u32(out, static_cast<std::uint32_t>(entry.effect_row.size()));
      for (const std::string &label : entry.effect_row) {
        append_string(out, label);
      }
      append_u32(out, entry.flags);
    }
  }
  return out;
}

std::vector<std::uint8_t> serialize_accelerator_kernels(
    const std::vector<wasm_accel::AcceleratorKernel> &kernels) {
  std::vector<wasm_accel::AcceleratorKernel> normalized;
  normalized.reserve(kernels.size());
  for (wasm_accel::AcceleratorKernel kernel : kernels) {
    normalized.push_back(
        wasm_accel::normalize_accelerator_kernel(std::move(kernel)));
  }
  std::sort(normalized.begin(), normalized.end(),
            [](const wasm_accel::AcceleratorKernel &left,
               const wasm_accel::AcceleratorKernel &right) {
              return left.kernel_id < right.kernel_id;
            });

  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(normalized.size()));
  for (const wasm_accel::AcceleratorKernel &kernel : normalized) {
    append_string(out, kernel.kernel_id);
    append_string(out, kernel.entry);
    append_string(out, kernel.target);
    append_u32(out, kernel.flags);

    append_u32(out, static_cast<std::uint32_t>(kernel.params.size()));
    for (const wasm_accel::AcceleratorValue &value : kernel.params) {
      append_string(out, value.name);
      append_string(out, value.type);
      append_string(out, value.address_space);
      append_u32(out, value.flags);
    }

    append_u32(out, static_cast<std::uint32_t>(kernel.captures.size()));
    for (const wasm_accel::AcceleratorValue &value : kernel.captures) {
      append_string(out, value.name);
      append_string(out, value.type);
      append_string(out, value.address_space);
      append_u32(out, value.flags);
    }

    append_u32(out, static_cast<std::uint32_t>(kernel.effect_row.size()));
    for (const std::string &label : kernel.effect_row) {
      append_string(out, label);
    }

    append_u32(out,
               static_cast<std::uint32_t>(kernel.forbidden_features.size()));
    for (const std::string &feature : kernel.forbidden_features) {
      append_string(out, feature);
    }
  }
  return out;
}

void append_source_location(std::vector<std::uint8_t> &out,
                            const modern::SourceLocation &source) {
  append_string(out, source.file);
  append_u32(out, source.line);
  append_u32(out, source.column);
}

void append_string_vector(std::vector<std::uint8_t> &out,
                          const std::vector<std::string> &values) {
  append_u32(out, static_cast<std::uint32_t>(values.size()));
  for (const std::string &value : values) {
    append_string(out, value);
  }
}

std::vector<std::uint8_t> serialize_agent_metadata(
    const std::vector<modern::AgentSymbol> &symbols,
    const std::vector<modern::AgentPatch> &patches,
    const std::vector<modern::ProvenanceRecord> &provenance) {
  const modern::AgentValidationResult validated =
      modern::validate_agent_metadata(symbols, patches, provenance);
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(validated.symbols.size()));
  for (const modern::AgentSymbol &symbol : validated.symbols) {
    append_string(out, symbol.symbol_id);
    append_string(out, symbol.name);
    append_string(out, symbol.kind);
    append_string(out, symbol.module);
    append_string(out, symbol.visibility);
    append_source_location(out, symbol.source);
    append_string(out, symbol.defined_in);
    append_u32(out, static_cast<std::uint32_t>(symbol.references.size()));
    for (const modern::SourceLocation &reference : symbol.references) {
      append_source_location(out, reference);
    }
    append_string(out, symbol.type_summary);
    append_string(out, symbol.effect_summary);
    append_string(out, symbol.schema_summary);
    append_string(out, symbol.doc_summary);
    append_u32(out, symbol.flags);
  }
  append_u32(out, static_cast<std::uint32_t>(validated.patches.size()));
  for (const modern::AgentPatch &patch : validated.patches) {
    append_string(out, patch.patch_id);
    append_string(out, patch.intent);
    append_string(out, patch.tool);
    append_string(out, patch.request_digest);
    append_string_vector(out, patch.capabilities);
    append_u32(out, static_cast<std::uint32_t>(patch.operations.size()));
    for (const modern::AgentPatchOperation &operation : patch.operations) {
      append_string(out, operation.op);
      append_string(out, operation.symbol_id);
      append_string(out, operation.new_name);
      append_u32(out, operation.flags);
    }
    append_u32(out, patch.flags);
  }
  append_u32(out, static_cast<std::uint32_t>(validated.provenance.size()));
  for (const modern::ProvenanceRecord &record : validated.provenance) {
    append_string(out, record.patch_id);
    append_string(out, record.tool);
    append_string(out, record.request_digest);
    append_string_vector(out, record.files_changed);
    append_string_vector(out, record.symbols_changed);
    append_string_vector(out, record.diagnostics_before);
    append_string_vector(out, record.diagnostics_after);
    append_string_vector(out, record.checks_run);
    append_string_vector(out, record.artifact_digests);
    append_string(out, record.human_approval);
    append_u32(out, record.flags);
  }
  return out;
}

std::vector<std::uint8_t> serialize_contract_metadata(
    const std::vector<modern::ContractSpec> &contracts,
    const std::vector<modern::PropertySpec> &properties) {
  const modern::ContractValidationResult validated =
      modern::validate_contract_metadata(contracts, properties);
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(validated.contracts.size()));
  for (const modern::ContractSpec &contract : validated.contracts) {
    append_string(out, contract.owner);
    append_string(out, contract.kind);
    append_string(out, contract.expression);
    append_string_vector(out, contract.effect_row);
    append_source_location(out, contract.source);
    append_u32(out, contract.flags);
  }
  append_u32(out, static_cast<std::uint32_t>(validated.properties.size()));
  for (const modern::PropertySpec &property : validated.properties) {
    append_string(out, property.name);
    append_string(out, property.owner);
    append_u64(out, property.seed);
    append_string(out, property.generator);
    append_string(out, property.shrinker_path);
    append_string(out, property.counterexample);
    append_string_vector(out, property.profile_set);
    append_string_vector(out, property.dependency_fingerprints);
    append_u32(out, property.flags);
  }
  return out;
}

std::vector<std::uint8_t> serialize_privacy_metadata(
    const std::vector<modern::PrivacyLabel> &labels,
    const std::vector<modern::PrivacyPolicyRule> &policies,
    const std::vector<modern::LineageNode> &lineage) {
  const modern::PrivacyValidationResult validated =
      modern::validate_privacy_metadata(labels, policies, lineage);
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(validated.labels.size()));
  for (const modern::PrivacyLabel &label : validated.labels) {
    append_string(out, label.name);
    append_string(out, label.kind);
    append_u32(out, label.flags);
  }
  append_u32(out, static_cast<std::uint32_t>(validated.policies.size()));
  for (const modern::PrivacyPolicyRule &rule : validated.policies) {
    append_string(out, rule.policy);
    append_string(out, rule.action);
    append_string(out, rule.label);
    append_string(out, rule.aggregate);
    append_u32(out, rule.min_group);
    append_u32(out, rule.flags);
  }
  append_u32(out, static_cast<std::uint32_t>(validated.lineage.size()));
  for (const modern::LineageNode &node : validated.lineage) {
    append_string(out, node.node_id);
    append_string(out, node.kind);
    append_string_vector(out, node.inputs);
    append_string(out, node.output);
    append_string(out, node.schema_fingerprint);
    append_string_vector(out, node.labels);
    append_source_location(out, node.source);
    append_string(out, node.trace_span);
    append_u32(out, node.flags);
  }
  return out;
}

std::vector<std::uint8_t> serialize_workflow_metadata(
    const std::vector<modern::WorkflowStep> &steps,
    const std::vector<modern::WorkflowHistoryEvent> &history) {
  const modern::WorkflowValidationResult validated =
      modern::validate_workflow_metadata(steps, history);
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(validated.steps.size()));
  for (const modern::WorkflowStep &step : validated.steps) {
    append_string(out, step.workflow);
    append_string(out, step.name);
    append_string_vector(out, step.effect_row);
    append_string_vector(out, step.depends_on);
    append_string(out, step.retry_policy);
    append_string(out, step.idempotency_key);
    append_u32(out, step.timeout_ms);
    append_u32(out, step.flags);
  }
  append_u32(out, static_cast<std::uint32_t>(validated.history.size()));
  for (const modern::WorkflowHistoryEvent &event : validated.history) {
    append_string(out, event.workflow_id);
    append_u32(out, event.workflow_version);
    append_string(out, event.step);
    append_string(out, event.event);
    append_string(out, event.input_digest);
    append_string(out, event.output_digest);
    append_string(out, event.schema_version);
    append_string_vector(out, event.effect_grants);
    append_string(out, event.idempotency_key);
    append_string(out, event.trace_id);
    append_u32(out, event.flags);
  }
  return out;
}

std::vector<std::uint8_t>
serialize_hashes(const std::vector<HashEntry> &hashes) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(hashes.size()));
  for (const HashEntry &entry : hashes) {
    const char *tag = section_tag(entry.section);
    out.insert(out.end(), tag, tag + 4);
    append_u32(out, static_cast<std::uint32_t>(entry.digest.size()));
    append_bytes(out, entry.digest);
  }
  return out;
}

std::vector<SectionPayload> build_sections(const BcModule &module) {
  std::vector<SectionPayload> sections;
  sections.push_back(
      {SectionKind::Strs, serialize_strings(module.strings), 1, 0});
  sections.push_back(
      {SectionKind::Syms, serialize_strings(module.symbols), 1, 0});
  sections.push_back(
      {SectionKind::Kons, serialize_constants(module.const_pool), 1, 0});
  sections.push_back(
      {SectionKind::Code, serialize_code(module.code_objects), 1, 0});
  sections.push_back(
      {SectionKind::Meth, serialize_methods(module.methods), 1, 0});
  sections.push_back(
      {SectionKind::Clas, serialize_classes(module.classes), 1, 0});
  sections.push_back(
      {SectionKind::Deps, serialize_dependencies(module.dependencies), 1, 0});
  sections.push_back(
      {SectionKind::Expt, serialize_exports(module.exports), 1, 0});
  sections.push_back({SectionKind::Init, serialize_init(module.init), 1, 0});
  if (optional_section_present(module, SectionKind::Pats)) {
    sections.push_back({SectionKind::Pats,
                        serialize_pattern_programs(module.pattern_programs), 1,
                        0});
  }
  if (optional_section_present(module, SectionKind::Span)) {
    sections.push_back(
        {SectionKind::Span, serialize_spans(module.code_objects), 1, 0});
  }
  if (optional_section_present(module, SectionKind::Line)) {
    sections.push_back(
        {SectionKind::Line, serialize_line_table(module.line_table), 1, 0});
  }
  if (optional_section_present(module, SectionKind::Locs)) {
    sections.push_back(
        {SectionKind::Locs, serialize_local_debug(module.local_debug), 1, 0});
  }
  if (optional_section_present(module, SectionKind::Attr)) {
    sections.push_back(
        {SectionKind::Attr, serialize_attrs(module.attrs), 1, 0});
  }
  if (optional_section_present(module, SectionKind::Prof)) {
    sections.push_back({SectionKind::Prof,
                        serialize_profile_metadata(module.required_features,
                                                   module.optional_features,
                                                   module.forbidden_features),
                        1, 0});
  }
  if (optional_section_present(module, SectionKind::Caps)) {
    sections.push_back(
        {SectionKind::Caps, serialize_capabilities(module.capabilities), 1, 0});
  }
  if (optional_section_present(module, SectionKind::Efct)) {
    sections.push_back(
        {SectionKind::Efct, serialize_effects(module.effects), 1, 0});
  }
  if (optional_section_present(module, SectionKind::Obsv)) {
    sections.push_back(
        {SectionKind::Obsv,
         serialize_observability_sites(module.observability_sites), 1, 0});
  }
  if (optional_section_present(module, SectionKind::Rply)) {
    sections.push_back({SectionKind::Rply,
                        serialize_replay_metadata(module.replay_metadata), 1,
                        0});
  }
  if (optional_section_present(module, SectionKind::Scma)) {
    sections.push_back(
        {SectionKind::Scma,
         serialize_schema_metadata(module.schemas, module.schema_migrations), 1,
         0});
  }
  if (optional_section_present(module, SectionKind::Tabl)) {
    sections.push_back(
        {SectionKind::Tabl, serialize_table_plans(module.table_plans), 1, 0});
  }
  if (optional_section_present(module, SectionKind::Wasm)) {
    sections.push_back({SectionKind::Wasm,
                        serialize_wasm_components(module.wasm_components), 1,
                        0});
  }
  if (optional_section_present(module, SectionKind::Accl)) {
    sections.push_back(
        {SectionKind::Accl,
         serialize_accelerator_kernels(module.accelerator_kernels), 1, 0});
  }
  if (optional_section_present(module, SectionKind::Agnt)) {
    sections.push_back(
        {SectionKind::Agnt,
         serialize_agent_metadata(module.agent_symbols, module.agent_patches,
                                  module.provenance_records),
         1, 0});
  }
  if (optional_section_present(module, SectionKind::Cntr)) {
    sections.push_back(
        {SectionKind::Cntr,
         serialize_contract_metadata(module.contracts, module.properties), 1,
         0});
  }
  if (optional_section_present(module, SectionKind::Priv)) {
    sections.push_back({SectionKind::Priv,
                        serialize_privacy_metadata(module.privacy_labels,
                                                   module.privacy_policies,
                                                   module.lineage_nodes),
                        1, 0});
  }
  if (optional_section_present(module, SectionKind::Wflw)) {
    sections.push_back({SectionKind::Wflw,
                        serialize_workflow_metadata(module.workflow_steps,
                                                    module.workflow_history),
                        1, 0});
  }
  if (optional_section_present(module, SectionKind::Hash)) {
    sections.push_back(
        {SectionKind::Hash, serialize_hashes(module.hashes), 1, 0});
  }
  return sections;
}

bool parse_string_pool(Reader &reader, std::vector<std::string> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string value;
    if (!reader.read_string(value)) {
      return false;
    }
    out.push_back(std::move(value));
  }
  return true;
}

bool decode_constant_kind(std::uint8_t raw, ConstantKind &kind) {
  switch (raw) {
  case 0:
    kind = ConstantKind::Null;
    return true;
  case 1:
    kind = ConstantKind::Bool;
    return true;
  case 2:
    kind = ConstantKind::Integer;
    return true;
  case 3:
    kind = ConstantKind::Float;
    return true;
  case 4:
    kind = ConstantKind::SymbolRef;
    return true;
  case 5:
    kind = ConstantKind::StringRef;
    return true;
  case 6:
    kind = ConstantKind::CodeRef;
    return true;
  case 7:
    kind = ConstantKind::KeySet;
    return true;
  case 8:
    kind = ConstantKind::Path;
    return true;
  default:
    break;
  }
  return false;
}

bool decode_opcode(std::uint8_t raw, Opcode &opcode) {
  switch (raw) {
  case 0x01:
    opcode = Opcode::LoadK;
    return true;
  case 0x02:
    opcode = Opcode::LoadNull;
    return true;
  case 0x03:
    opcode = Opcode::LoadBool;
    return true;
  case 0x04:
    opcode = Opcode::Move;
    return true;
  case 0x05:
    opcode = Opcode::LoadSelf;
    return true;
  case 0x06:
    opcode = Opcode::GetLast;
    return true;
  case 0x07:
    opcode = Opcode::SetLast;
    return true;
  case 0x08:
    opcode = Opcode::MakeList;
    return true;
  case 0x09:
    opcode = Opcode::MakeTuple;
    return true;
  case 0x0A:
    opcode = Opcode::MakeMap;
    return true;
  case 0x0B:
    opcode = Opcode::Freeze;
    return true;
  case 0x0C:
    opcode = Opcode::MakeSet;
    return true;
  case 0x0D:
    opcode = Opcode::MakeMapDyn;
    return true;
  case 0x10:
    opcode = Opcode::LoadUpval;
    return true;
  case 0x11:
    opcode = Opcode::StoreUpval;
    return true;
  case 0x12:
    opcode = Opcode::LoadIvar;
    return true;
  case 0x13:
    opcode = Opcode::StoreIvar;
    return true;
  case 0x14:
    opcode = Opcode::LoadCvar;
    return true;
  case 0x15:
    opcode = Opcode::StoreCvar;
    return true;
  case 0x16:
    opcode = Opcode::LookupConst;
    return true;
  case 0x17:
    opcode = Opcode::MakeClosure;
    return true;
  case 0x18:
    opcode = Opcode::ObjDestroy;
    return true;
  case 0x19:
    opcode = Opcode::ObjDealloc;
    return true;
  case 0x1A:
    opcode = Opcode::CloseUpvalues;
    return true;
  case 0x1B:
    opcode = Opcode::WatchLocal;
    return true;
  case 0x1C:
    opcode = Opcode::WatchUpval;
    return true;
  case 0x1D:
    opcode = Opcode::WatchIvar;
    return true;
  case 0x20:
    opcode = Opcode::Send;
    return true;
  case 0x21:
    opcode = Opcode::SendDyn;
    return true;
  case 0x22:
    opcode = Opcode::Call;
    return true;
  case 0x23:
    opcode = Opcode::InOp;
    return true;
  case 0x24:
    opcode = Opcode::TripleEq;
    return true;
  case 0x25:
    opcode = Opcode::TypeCheck;
    return true;
  case 0x26:
    opcode = Opcode::IAdd;
    return true;
  case 0x27:
    opcode = Opcode::ISub;
    return true;
  case 0x28:
    opcode = Opcode::ILt;
    return true;
  case 0x29:
    opcode = Opcode::IGt;
    return true;
  case 0x2A:
    opcode = Opcode::IAddK;
    return true;
  case 0x2B:
    opcode = Opcode::ISubK;
    return true;
  case 0x2C:
    opcode = Opcode::ILtK;
    return true;
  case 0x2D:
    opcode = Opcode::IGtK;
    return true;
  case 0x50:
    opcode = Opcode::IMul;
    return true;
  case 0x51:
    opcode = Opcode::IDiv;
    return true;
  case 0x52:
    opcode = Opcode::IMod;
    return true;
  case 0x53:
    opcode = Opcode::IFloorDiv;
    return true;
  case 0x54:
    opcode = Opcode::ILe;
    return true;
  case 0x55:
    opcode = Opcode::IGe;
    return true;
  case 0x56:
    opcode = Opcode::IEq;
    return true;
  case 0x57:
    opcode = Opcode::INe;
    return true;
  case 0x58:
    opcode = Opcode::ICmp;
    return true;
  case 0x59:
    opcode = Opcode::IMulK;
    return true;
  case 0x5A:
    opcode = Opcode::IDivK;
    return true;
  case 0x5B:
    opcode = Opcode::IModK;
    return true;
  case 0x5C:
    opcode = Opcode::IFloorDivK;
    return true;
  case 0x5D:
    opcode = Opcode::ILeK;
    return true;
  case 0x5E:
    opcode = Opcode::IGeK;
    return true;
  case 0x5F:
    opcode = Opcode::IEqK;
    return true;
  case 0x60:
    opcode = Opcode::INeK;
    return true;
  case 0x61:
    opcode = Opcode::ICmpK;
    return true;
  case 0x30:
    opcode = Opcode::Jump;
    return true;
  case 0x31:
    opcode = Opcode::JumpIfTrue;
    return true;
  case 0x32:
    opcode = Opcode::JumpIfFalse;
    return true;
  case 0x33:
    opcode = Opcode::JumpIfNull;
    return true;
  case 0x34:
    opcode = Opcode::Return;
    return true;
  case 0x35:
    opcode = Opcode::Raise;
    return true;
  case 0x36:
    opcode = Opcode::Safepoint;
    return true;
  case 0x40:
    opcode = Opcode::PPrepSeq;
    return true;
  case 0x41:
    opcode = Opcode::PPrepMap;
    return true;
  case 0x42:
    opcode = Opcode::PCheckEq;
    return true;
  case 0x43:
    opcode = Opcode::PCheckPin;
    return true;
  case 0x44:
    opcode = Opcode::PCheckLenEq;
    return true;
  case 0x45:
    opcode = Opcode::PCheckLenGte;
    return true;
  case 0x46:
    opcode = Opcode::PGetIndex;
    return true;
  case 0x47:
    opcode = Opcode::PHasKey;
    return true;
  case 0x48:
    opcode = Opcode::PGetKey;
    return true;
  case 0x49:
    opcode = Opcode::PTripleEq;
    return true;
  case 0x4A:
    opcode = Opcode::PBind;
    return true;
  case 0x4B:
    opcode = Opcode::PCommit;
    return true;
  case 0x4C:
    opcode = Opcode::PFail;
    return true;
  default:
    return false;
  }
}

bool decode_code_kind(std::uint8_t raw, CodeKind &kind) {
  switch (raw) {
  case 0:
    kind = CodeKind::Module;
    return true;
  case 1:
    kind = CodeKind::Method;
    return true;
  case 2:
    kind = CodeKind::Block;
    return true;
  case 3:
    kind = CodeKind::Ensure;
    return true;
  case 4:
    kind = CodeKind::Rescue;
    return true;
  case 5:
    kind = CodeKind::DefaultThunk;
    return true;
  default:
    return false;
  }
}

bool parse_constants(Reader &reader, std::vector<Constant> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint8_t raw_kind = 0;
    if (!reader.read_u8(raw_kind)) {
      return false;
    }
    ConstantKind kind = ConstantKind::Null;
    if (!decode_constant_kind(raw_kind, kind)) {
      reader.fail("BC1210", "unknown constant kind", reader.pos - 1U);
      return false;
    }
    Constant constant;
    constant.kind = kind;
    switch (kind) {
    case ConstantKind::Null:
      break;
    case ConstantKind::Bool: {
      std::uint8_t raw = 0;
      if (!reader.read_u8(raw)) {
        return false;
      }
      constant.bool_value = raw != 0U;
      break;
    }
    case ConstantKind::Integer:
      if (!reader.read_sleb(constant.int_value)) {
        return false;
      }
      break;
    case ConstantKind::Float: {
      std::uint64_t bits = 0;
      if (!reader.read_u64(bits)) {
        return false;
      }
      std::memcpy(&constant.float_value, &bits, sizeof(bits));
      break;
    }
    case ConstantKind::SymbolRef:
    case ConstantKind::StringRef:
    case ConstantKind::CodeRef:
      if (!reader.read_u32(constant.ref_id)) {
        return false;
      }
      break;
    case ConstantKind::KeySet:
    case ConstantKind::Path: {
      std::uint32_t item_count = 0;
      if (!reader.read_u32(item_count)) {
        return false;
      }
      constant.items.reserve(item_count);
      for (std::uint32_t item = 0; item < item_count; ++item) {
        std::uint32_t value = 0;
        if (!reader.read_u32(value)) {
          return false;
        }
        constant.items.push_back(value);
      }
      break;
    }
    }
    out.push_back(std::move(constant));
  }
  return true;
}

bool parse_code(Reader &reader, std::vector<BcCode> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    BcCode code;
    std::uint8_t raw_kind = 0;
    if (!reader.read_u32(code.code_id) || !reader.read_u8(raw_kind) ||
        !reader.read_u32(code.reg_count) || !reader.read_u32(code.flags)) {
      return false;
    }
    if (!decode_code_kind(raw_kind, code.kind)) {
      reader.fail("BC1308", "unknown code kind", reader.pos - 1U);
      return false;
    }

    std::uint32_t local_count = 0;
    if (!reader.read_u32(local_count)) {
      return false;
    }
    code.local_layout.reserve(local_count);
    for (std::uint32_t j = 0; j < local_count; ++j) {
      SlotLayoutEntry entry;
      if (!reader.read_u32(entry.slot) || !reader.read_u32(entry.name_str_id) ||
          !reader.read_u32(entry.role_str_id) ||
          !reader.read_u32(entry.binding_kind_str_id)) {
        return false;
      }
      code.local_layout.push_back(entry);
    }

    std::uint32_t capture_count = 0;
    if (!reader.read_u32(capture_count)) {
      return false;
    }
    code.capture_layout.reserve(capture_count);
    for (std::uint32_t j = 0; j < capture_count; ++j) {
      CaptureLayoutEntry entry;
      if (!reader.read_u32(entry.slot) || !reader.read_u32(entry.name_str_id) ||
          !reader.read_u32(entry.source_kind_str_id) ||
          !reader.read_u32(entry.source_name_str_id)) {
        return false;
      }
      code.capture_layout.push_back(entry);
    }

    std::uint32_t insn_count = 0;
    if (!reader.read_u32(insn_count)) {
      return false;
    }
    code.instructions.reserve(insn_count);
    for (std::uint32_t j = 0; j < insn_count; ++j) {
      std::uint8_t raw_opcode = 0;
      std::uint32_t operand_count = 0;
      if (!reader.read_u8(raw_opcode) || !reader.read_u32(operand_count)) {
        return false;
      }
      Instruction instruction;
      if (!decode_opcode(raw_opcode, instruction.opcode)) {
        reader.fail("BC1301", "unknown opcode", reader.pos - 5U);
        return false;
      }
      instruction.operands.reserve(operand_count);
      for (std::uint32_t operand_index = 0; operand_index < operand_count;
           ++operand_index) {
        std::uint8_t signed_flag = 0;
        if (!reader.read_u8(signed_flag)) {
          return false;
        }
        InstructionOperand operand;
        operand.signed_immediate = signed_flag != 0U;
        if (operand.signed_immediate) {
          if (!reader.read_sleb(operand.value)) {
            return false;
          }
        } else {
          std::uint64_t raw = 0;
          if (!reader.read_uleb(raw)) {
            return false;
          }
          operand.value = static_cast<std::int64_t>(raw);
        }
        instruction.operands.push_back(operand);
      }
      code.instructions.push_back(std::move(instruction));
    }

    std::uint32_t handler_count = 0;
    if (!reader.read_u32(handler_count)) {
      return false;
    }
    code.handler_table.reserve(handler_count);
    for (std::uint32_t j = 0; j < handler_count; ++j) {
      HandlerEntry entry;
      if (!reader.read_u32(entry.protected_from) ||
          !reader.read_u32(entry.protected_to) ||
          !reader.read_u32(entry.handler_pc) ||
          !reader.read_u32(entry.handler_code_id) ||
          !reader.read_u32(entry.flags)) {
        return false;
      }
      code.handler_table.push_back(entry);
    }

    std::uint32_t call_count = 0;
    if (!reader.read_u32(call_count)) {
      return false;
    }
    code.call_site_table.reserve(call_count);
    for (std::uint32_t j = 0; j < call_count; ++j) {
      CacheSiteEntry entry;
      if (!reader.read_u32(entry.pc) || !reader.read_u32(entry.slot) ||
          !reader.read_u32(entry.symbol_id) || !reader.read_u32(entry.flags)) {
        return false;
      }
      code.call_site_table.push_back(entry);
    }

    std::uint32_t ivar_count = 0;
    if (!reader.read_u32(ivar_count)) {
      return false;
    }
    code.ivar_site_table.reserve(ivar_count);
    for (std::uint32_t j = 0; j < ivar_count; ++j) {
      CacheSiteEntry entry;
      if (!reader.read_u32(entry.pc) || !reader.read_u32(entry.slot) ||
          !reader.read_u32(entry.symbol_id) || !reader.read_u32(entry.flags)) {
        return false;
      }
      code.ivar_site_table.push_back(entry);
    }

    std::uint32_t safepoint_count = 0;
    if (!reader.read_u32(safepoint_count)) {
      return false;
    }
    code.safepoint_table.reserve(safepoint_count);
    for (std::uint32_t j = 0; j < safepoint_count; ++j) {
      SafepointEntry entry;
      if (!reader.read_u32(entry.pc) || !reader.read_u32(entry.flags)) {
        return false;
      }
      code.safepoint_table.push_back(entry);
    }

    out.push_back(std::move(code));
  }
  return true;
}

bool parse_methods(Reader &reader, std::vector<BcMethod> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    BcMethod method;
    if (!reader.read_u32(method.selector_sym_id) ||
        !reader.read_u32(method.owner_dispatch_ref) ||
        !reader.read_u32(method.signature_blob_id)) {
      return false;
    }

    std::uint32_t param_count = 0;
    if (!reader.read_u32(param_count)) {
      return false;
    }
    method.params.reserve(param_count);
    for (std::uint32_t j = 0; j < param_count; ++j) {
      MethodParamEntry entry;
      if (!reader.read_u32(entry.external_name_sym_id) ||
          !reader.read_u32(entry.local_name_str_id) ||
          !reader.read_u32(entry.flags)) {
        return false;
      }
      method.params.push_back(entry);
    }

    std::uint32_t thunk_count = 0;
    if (!reader.read_u32(thunk_count)) {
      return false;
    }
    method.default_thunk_ids.reserve(thunk_count);
    for (std::uint32_t j = 0; j < thunk_count; ++j) {
      std::uint32_t code_id = 0;
      if (!reader.read_u32(code_id)) {
        return false;
      }
      method.default_thunk_ids.push_back(code_id);
    }

    std::uint32_t hook_count = 0;
    if (!reader.read_u32(hook_count)) {
      return false;
    }
    method.type_hook_ids.reserve(hook_count);
    for (std::uint32_t j = 0; j < hook_count; ++j) {
      std::uint32_t code_id = 0;
      if (!reader.read_u32(code_id)) {
        return false;
      }
      method.type_hook_ids.push_back(code_id);
    }

    std::uint32_t clause_count = 0;
    if (!reader.read_u32(clause_count)) {
      return false;
    }
    method.clause_table.reserve(clause_count);
    for (std::uint32_t j = 0; j < clause_count; ++j) {
      ClauseEntry entry;
      if (!reader.read_u32(entry.pattern_program_id) ||
          !reader.read_u32(entry.pattern_code_id) ||
          !reader.read_u32(entry.guard_code_id) ||
          !reader.read_u32(entry.body_code_id) ||
          !reader.read_u32(entry.flags)) {
        return false;
      }
      method.clause_table.push_back(entry);
    }

    std::uint32_t assign_count = 0;
    if (!reader.read_u32(assign_count)) {
      return false;
    }
    method.auto_assign_desc.reserve(assign_count);
    for (std::uint32_t j = 0; j < assign_count; ++j) {
      AutoAssignEntry entry;
      if (!reader.read_u32(entry.local_name_str_id) ||
          !reader.read_u32(entry.target_name_str_id) ||
          !reader.read_u32(entry.flags)) {
        return false;
      }
      method.auto_assign_desc.push_back(entry);
    }

    if (!reader.read_u32(method.entry_code_id) ||
        !reader.read_u32(method.flags)) {
      return false;
    }
    out.push_back(std::move(method));
  }
  return true;
}

bool parse_classes(Reader &reader, std::vector<BcClass> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    BcClass klass;
    std::uint8_t has_super = 0;
    std::uint8_t has_init = 0;
    std::uint32_t include_count = 0;
    std::uint32_t extend_count = 0;
    if (!reader.read_u32(klass.class_name_sym_id) ||
        !reader.read_u8(has_super) || !reader.read_u32(klass.superclass_ref) ||
        !reader.read_u32(klass.ivar_schema_id) ||
        !reader.read_u32(klass.method_range_start) ||
        !reader.read_u32(klass.method_range_count) ||
        !reader.read_u32(include_count)) {
      return false;
    }
    klass.direct_include_refs.reserve(include_count);
    for (std::uint32_t j = 0; j < include_count; ++j) {
      std::uint32_t value = 0;
      if (!reader.read_u32(value)) {
        return false;
      }
      klass.direct_include_refs.push_back(value);
    }
    if (!reader.read_u32(extend_count)) {
      return false;
    }
    klass.direct_extend_refs.reserve(extend_count);
    for (std::uint32_t j = 0; j < extend_count; ++j) {
      std::uint32_t value = 0;
      if (!reader.read_u32(value)) {
        return false;
      }
      klass.direct_extend_refs.push_back(value);
    }
    if (!reader.read_u32(klass.flags) || !reader.read_u8(has_init) ||
        !reader.read_u32(klass.class_init_code_id)) {
      return false;
    }
    klass.has_superclass_ref = has_super != 0U;
    klass.has_class_init_code_id = has_init != 0U;
    out.push_back(klass);
  }
  return true;
}

bool parse_dependencies(Reader &reader, std::vector<DepEntry> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    DepEntry entry;
    std::uint8_t has_max = 0;
    std::uint8_t has_abi = 0;
    std::vector<std::uint8_t> abi;
    if (!reader.read_u32(entry.module_name_str_id) ||
        !reader.read_u16(entry.required_format.major) ||
        !reader.read_u16(entry.required_format.minor) ||
        !reader.read_u16(entry.min_language_version.major) ||
        !reader.read_u16(entry.min_language_version.minor) ||
        !reader.read_u8(has_max) ||
        !reader.read_u16(entry.max_language_version.major) ||
        !reader.read_u16(entry.max_language_version.minor) ||
        !reader.read_u8(has_abi) || !reader.read_bytes(32, abi) ||
        !reader.read_u32(entry.flags)) {
      return false;
    }
    entry.has_max_language_version = has_max != 0U;
    entry.has_abi_requirement = has_abi != 0U;
    std::copy(abi.begin(), abi.end(), entry.abi_requirement.begin());
    out.push_back(entry);
  }
  return true;
}

bool parse_exports(Reader &reader, std::vector<ExportEntry> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ExportEntry entry;
    std::uint8_t has_reexport = 0;
    if (!reader.read_u32(entry.symbol_id) ||
        !reader.read_u32(entry.target_kind_str_id) ||
        !reader.read_u32(entry.target_index) ||
        !reader.read_u32(entry.visibility_flags) ||
        !reader.read_u8(has_reexport) ||
        !reader.read_u32(entry.reexport_module_name_str_id)) {
      return false;
    }
    entry.has_reexport_module_name = has_reexport != 0U;
    out.push_back(entry);
  }
  return true;
}

bool parse_init(Reader &reader, InitEntry &out) {
  std::uint8_t has_entry = 0;
  if (!reader.read_u8(has_entry) || !reader.read_u32(out.entry_code_id) ||
      !reader.read_u32(out.flags)) {
    return false;
  }
  out.has_entry_code_id = has_entry != 0U;
  return true;
}

bool parse_pattern_programs(Reader &reader,
                            std::vector<PatternProgramEntry> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    PatternProgramEntry entry;
    if (!reader.read_u32(entry.pattern_id) ||
        !reader.read_u32(entry.binding_count) ||
        !reader.read_u32(entry.flags)) {
      return false;
    }
    out.push_back(entry);
  }
  return true;
}

bool parse_spans(Reader &reader, std::vector<BcCode> &codes) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  std::unordered_map<std::uint32_t, BcCode *> by_id;
  for (BcCode &code : codes) {
    by_id.emplace(code.code_id, &code);
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t code_id = 0;
    std::uint32_t start_line = 0;
    std::uint32_t start_col = 0;
    std::uint32_t start_offset = 0;
    std::uint32_t end_line = 0;
    std::uint32_t end_col = 0;
    std::uint32_t end_offset = 0;
    SourceSpanEntry entry;
    if (!reader.read_u32(code_id) || !reader.read_u32(entry.pc_from) ||
        !reader.read_u32(entry.pc_to) || !reader.read_string(entry.span.file) ||
        !reader.read_u32(start_line) || !reader.read_u32(start_col) ||
        !reader.read_u32(start_offset) || !reader.read_u32(end_line) ||
        !reader.read_u32(end_col) || !reader.read_u32(end_offset)) {
      return false;
    }
    entry.span.start.line = start_line;
    entry.span.start.col = start_col;
    entry.span.start.offset = start_offset;
    entry.span.end.line = end_line;
    entry.span.end.col = end_col;
    entry.span.end.offset = end_offset;
    const auto found = by_id.find(code_id);
    if (found == by_id.end()) {
      reader.fail("BC1204", "SPAN references unknown code id", reader.pos);
      return false;
    }
    found->second->source_spans.push_back(std::move(entry));
  }
  return true;
}

bool parse_line_table(Reader &reader, std::vector<LineEntry> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    LineEntry entry;
    if (!reader.read_u32(entry.code_id) || !reader.read_u32(entry.pc) ||
        !reader.read_u32(entry.line)) {
      return false;
    }
    out.push_back(entry);
  }
  return true;
}

bool parse_local_debug(Reader &reader, std::vector<LocalDebugEntry> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    LocalDebugEntry entry;
    if (!reader.read_u32(entry.code_id) || !reader.read_u32(entry.slot) ||
        !reader.read_u32(entry.name_str_id) ||
        !reader.read_u32(entry.start_pc) || !reader.read_u32(entry.end_pc)) {
      return false;
    }
    out.push_back(entry);
  }
  return true;
}

bool parse_attrs(Reader &reader, std::vector<AttrEntry> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    AttrEntry entry;
    if (!reader.read_u32(entry.key_str_id) ||
        !reader.read_u32(entry.value_str_id)) {
      return false;
    }
    out.push_back(entry);
  }
  return true;
}

bool parse_profile_vector(Reader &reader, std::vector<std::string> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string value;
    if (!reader.read_string(value)) {
      return false;
    }
    out.push_back(std::move(value));
  }
  return true;
}

bool parse_profile_metadata(Reader &reader, BcModule &module) {
  return parse_profile_vector(reader, module.required_features) &&
         parse_profile_vector(reader, module.optional_features) &&
         parse_profile_vector(reader, module.forbidden_features);
}

bool parse_capabilities(Reader &reader,
                        std::vector<capability::CapabilityRequest> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    capability::CapabilityRequest entry;
    if (!reader.read_string(entry.name) || !reader.read_string(entry.target) ||
        !reader.read_string(entry.reason) || !reader.read_u32(entry.flags)) {
      return false;
    }
    out.push_back(std::move(entry));
  }
  return true;
}

bool parse_effects(Reader &reader, std::vector<effect::EffectSummary> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    effect::EffectSummary entry;
    if (!reader.read_string(entry.owner) || !reader.read_string(entry.kind)) {
      return false;
    }
    std::uint32_t declared_count = 0;
    if (!reader.read_u32(declared_count)) {
      return false;
    }
    entry.declared_effects.reserve(declared_count);
    for (std::uint32_t j = 0; j < declared_count; ++j) {
      std::string label;
      if (!reader.read_string(label)) {
        return false;
      }
      entry.declared_effects.push_back(std::move(label));
    }
    std::uint32_t observed_count = 0;
    if (!reader.read_u32(observed_count)) {
      return false;
    }
    entry.observed_effects.reserve(observed_count);
    for (std::uint32_t j = 0; j < observed_count; ++j) {
      std::string label;
      if (!reader.read_string(label)) {
        return false;
      }
      entry.observed_effects.push_back(std::move(label));
    }
    if (!reader.read_u32(entry.flags)) {
      return false;
    }
    out.push_back(std::move(entry));
  }
  return true;
}

bool parse_observability_sites(Reader &reader,
                               std::vector<replay::ObservabilitySite> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    replay::ObservabilitySite entry;
    if (!reader.read_u32(entry.site_id) ||
        !reader.read_string(entry.event_name) ||
        !reader.read_string(entry.kind) || !reader.read_string(entry.owner) ||
        !reader.read_string(entry.source.file) ||
        !reader.read_u32(entry.source.line) ||
        !reader.read_u32(entry.source.column) ||
        !reader.read_u32(entry.flags)) {
      return false;
    }
    out.push_back(replay::normalize_site(std::move(entry)));
  }
  return true;
}

bool parse_replay_metadata(Reader &reader, replay::ReplayMetadata &out) {
  replay::ReplayMetadata metadata;
  if (!reader.read_u32(metadata.flags)) {
    return false;
  }
  std::uint32_t required_count = 0;
  if (!reader.read_u32(required_count)) {
    return false;
  }
  metadata.required_event_names.reserve(required_count);
  for (std::uint32_t i = 0; i < required_count; ++i) {
    std::string event_name;
    if (!reader.read_string(event_name)) {
      return false;
    }
    metadata.required_event_names.push_back(std::move(event_name));
  }
  std::uint32_t source_count = 0;
  if (!reader.read_u32(source_count)) {
    return false;
  }
  metadata.deterministic_sources.reserve(source_count);
  for (std::uint32_t i = 0; i < source_count; ++i) {
    std::string source;
    if (!reader.read_string(source)) {
      return false;
    }
    metadata.deterministic_sources.push_back(std::move(source));
  }
  out = replay::normalize_metadata(std::move(metadata));
  return true;
}

bool parse_schema_metadata(Reader &reader,
                           std::vector<data::SchemaDefinition> &schemas,
                           std::vector<data::SchemaMigration> &migrations) {
  std::uint32_t schema_count = 0;
  if (!reader.read_u32(schema_count)) {
    return false;
  }
  std::vector<data::SchemaDefinition> parsed_schemas;
  parsed_schemas.reserve(schema_count);
  for (std::uint32_t i = 0; i < schema_count; ++i) {
    data::SchemaDefinition schema;
    if (!reader.read_string(schema.name) || !reader.read_u32(schema.version) ||
        !reader.read_u32(schema.flags)) {
      return false;
    }
    std::uint32_t field_count = 0;
    if (!reader.read_u32(field_count)) {
      return false;
    }
    schema.fields.reserve(field_count);
    for (std::uint32_t j = 0; j < field_count; ++j) {
      data::SchemaField field;
      std::uint8_t required = 0;
      std::uint8_t nullable = 0;
      if (!reader.read_string(field.name) || !reader.read_string(field.type) ||
          !reader.read_u8(required) || !reader.read_u8(nullable) ||
          !reader.read_string(field.default_value) ||
          !reader.read_u32(field.flags)) {
        return false;
      }
      field.required = required != 0U;
      field.nullable = nullable != 0U;
      schema.fields.push_back(data::normalize_schema_field(std::move(field)));
    }
    parsed_schemas.push_back(data::normalize_schema(std::move(schema)));
  }

  std::uint32_t migration_count = 0;
  if (!reader.read_u32(migration_count)) {
    return false;
  }
  std::vector<data::SchemaMigration> parsed_migrations;
  parsed_migrations.reserve(migration_count);
  for (std::uint32_t i = 0; i < migration_count; ++i) {
    data::SchemaMigration migration;
    if (!reader.read_string(migration.schema_name) ||
        !reader.read_u32(migration.from_version) ||
        !reader.read_u32(migration.to_version) ||
        !reader.read_string(migration.kind) ||
        !reader.read_u32(migration.flags)) {
      return false;
    }
    parsed_migrations.push_back(
        data::normalize_schema_migration(std::move(migration)));
  }

  const data::SchemaValidationResult validated =
      data::validate_schemas(parsed_schemas, parsed_migrations);
  schemas = validated.schemas;
  migrations = validated.migrations;
  return true;
}

bool parse_table_plans(Reader &reader, std::vector<data::TablePlan> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  std::vector<data::TablePlan> plans;
  plans.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    data::TablePlan plan;
    if (!reader.read_string(plan.plan_id) || !reader.read_string(plan.op) ||
        !reader.read_u32(plan.flags)) {
      return false;
    }
    std::uint32_t input_count = 0;
    if (!reader.read_u32(input_count)) {
      return false;
    }
    plan.input_refs.reserve(input_count);
    for (std::uint32_t j = 0; j < input_count; ++j) {
      std::string input;
      if (!reader.read_string(input)) {
        return false;
      }
      plan.input_refs.push_back(std::move(input));
    }
    std::uint32_t arg_count = 0;
    if (!reader.read_u32(arg_count)) {
      return false;
    }
    plan.arguments.reserve(arg_count);
    for (std::uint32_t j = 0; j < arg_count; ++j) {
      std::string argument;
      if (!reader.read_string(argument)) {
        return false;
      }
      plan.arguments.push_back(std::move(argument));
    }
    std::uint32_t dep_count = 0;
    if (!reader.read_u32(dep_count)) {
      return false;
    }
    plan.column_dependencies.reserve(dep_count);
    for (std::uint32_t j = 0; j < dep_count; ++j) {
      data::ColumnDependency dependency;
      if (!reader.read_string(dependency.table_ref) ||
          !reader.read_string(dependency.column_ref)) {
        return false;
      }
      plan.column_dependencies.push_back(std::move(dependency));
    }
    std::uint32_t effect_count = 0;
    if (!reader.read_u32(effect_count)) {
      return false;
    }
    plan.effect_row.reserve(effect_count);
    for (std::uint32_t j = 0; j < effect_count; ++j) {
      std::string effect_label;
      if (!reader.read_string(effect_label)) {
        return false;
      }
      plan.effect_row.push_back(std::move(effect_label));
    }
    plans.push_back(data::normalize_table_plan(std::move(plan)));
  }
  out = data::validate_table_plans(plans).plans;
  return true;
}

bool parse_wasm_interface_entries(
    Reader &reader, std::vector<wasm_accel::WasmInterfaceEntry> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    wasm_accel::WasmInterfaceEntry entry;
    if (!reader.read_string(entry.name) || !reader.read_string(entry.kind) ||
        !reader.read_string(entry.type_signature) ||
        !reader.read_string(entry.schema_name) ||
        !reader.read_string(entry.capability.name) ||
        !reader.read_string(entry.capability.target) ||
        !reader.read_string(entry.capability.reason) ||
        !reader.read_u32(entry.capability.flags)) {
      return false;
    }
    std::uint32_t effect_count = 0;
    if (!reader.read_u32(effect_count)) {
      return false;
    }
    entry.effect_row.reserve(effect_count);
    for (std::uint32_t j = 0; j < effect_count; ++j) {
      std::string effect_label;
      if (!reader.read_string(effect_label)) {
        return false;
      }
      entry.effect_row.push_back(std::move(effect_label));
    }
    if (!reader.read_u32(entry.flags)) {
      return false;
    }
    out.push_back(wasm_accel::normalize_wasm_interface_entry(std::move(entry)));
  }
  return true;
}

bool parse_wasm_components(Reader &reader,
                           std::vector<wasm_accel::WasmComponent> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  std::vector<wasm_accel::WasmComponent> components;
  components.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    wasm_accel::WasmComponent component;
    if (!reader.read_string(component.name) ||
        !reader.read_string(component.world) ||
        !reader.read_u32(component.flags)) {
      return false;
    }
    if (!parse_wasm_interface_entries(reader, component.imports) ||
        !parse_wasm_interface_entries(reader, component.exports)) {
      return false;
    }
    components.push_back(
        wasm_accel::normalize_wasm_component(std::move(component)));
  }
  out = wasm_accel::validate_wasm_components(components).components;
  return true;
}

bool parse_accelerator_values(Reader &reader,
                              std::vector<wasm_accel::AcceleratorValue> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    wasm_accel::AcceleratorValue value;
    if (!reader.read_string(value.name) || !reader.read_string(value.type) ||
        !reader.read_string(value.address_space) ||
        !reader.read_u32(value.flags)) {
      return false;
    }
    out.push_back(wasm_accel::normalize_accelerator_value(std::move(value)));
  }
  return true;
}

bool parse_accelerator_kernels(
    Reader &reader, std::vector<wasm_accel::AcceleratorKernel> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  std::vector<wasm_accel::AcceleratorKernel> kernels;
  kernels.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    wasm_accel::AcceleratorKernel kernel;
    if (!reader.read_string(kernel.kernel_id) ||
        !reader.read_string(kernel.entry) ||
        !reader.read_string(kernel.target) || !reader.read_u32(kernel.flags)) {
      return false;
    }
    if (!parse_accelerator_values(reader, kernel.params) ||
        !parse_accelerator_values(reader, kernel.captures)) {
      return false;
    }
    std::uint32_t effect_count = 0;
    if (!reader.read_u32(effect_count)) {
      return false;
    }
    kernel.effect_row.reserve(effect_count);
    for (std::uint32_t j = 0; j < effect_count; ++j) {
      std::string label;
      if (!reader.read_string(label)) {
        return false;
      }
      kernel.effect_row.push_back(std::move(label));
    }
    std::uint32_t forbidden_count = 0;
    if (!reader.read_u32(forbidden_count)) {
      return false;
    }
    kernel.forbidden_features.reserve(forbidden_count);
    for (std::uint32_t j = 0; j < forbidden_count; ++j) {
      std::string feature;
      if (!reader.read_string(feature)) {
        return false;
      }
      kernel.forbidden_features.push_back(std::move(feature));
    }
    kernels.push_back(
        wasm_accel::normalize_accelerator_kernel(std::move(kernel)));
  }
  out = wasm_accel::validate_accelerator_kernels(kernels).kernels;
  return true;
}

bool parse_source_location(Reader &reader, modern::SourceLocation &out) {
  return reader.read_string(out.file) && reader.read_u32(out.line) &&
         reader.read_u32(out.column);
}

bool parse_string_vector(Reader &reader, std::vector<std::string> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string value;
    if (!reader.read_string(value)) {
      return false;
    }
    out.push_back(std::move(value));
  }
  return true;
}

bool parse_agent_metadata(Reader &reader,
                          std::vector<modern::AgentSymbol> &symbols_out,
                          std::vector<modern::AgentPatch> &patches_out,
                          std::vector<modern::ProvenanceRecord> &prov_out) {
  std::uint32_t symbol_count = 0;
  if (!reader.read_u32(symbol_count)) {
    return false;
  }
  std::vector<modern::AgentSymbol> symbols;
  symbols.reserve(symbol_count);
  for (std::uint32_t i = 0; i < symbol_count; ++i) {
    modern::AgentSymbol symbol;
    if (!reader.read_string(symbol.symbol_id) ||
        !reader.read_string(symbol.name) || !reader.read_string(symbol.kind) ||
        !reader.read_string(symbol.module) ||
        !reader.read_string(symbol.visibility) ||
        !parse_source_location(reader, symbol.source) ||
        !reader.read_string(symbol.defined_in)) {
      return false;
    }
    std::uint32_t reference_count = 0;
    if (!reader.read_u32(reference_count)) {
      return false;
    }
    symbol.references.reserve(reference_count);
    for (std::uint32_t j = 0; j < reference_count; ++j) {
      modern::SourceLocation reference;
      if (!parse_source_location(reader, reference)) {
        return false;
      }
      symbol.references.push_back(std::move(reference));
    }
    if (!reader.read_string(symbol.type_summary) ||
        !reader.read_string(symbol.effect_summary) ||
        !reader.read_string(symbol.schema_summary) ||
        !reader.read_string(symbol.doc_summary) ||
        !reader.read_u32(symbol.flags)) {
      return false;
    }
    symbols.push_back(modern::normalize_agent_symbol(std::move(symbol)));
  }

  std::uint32_t patch_count = 0;
  if (!reader.read_u32(patch_count)) {
    return false;
  }
  std::vector<modern::AgentPatch> patches;
  patches.reserve(patch_count);
  for (std::uint32_t i = 0; i < patch_count; ++i) {
    modern::AgentPatch patch;
    if (!reader.read_string(patch.patch_id) ||
        !reader.read_string(patch.intent) || !reader.read_string(patch.tool) ||
        !reader.read_string(patch.request_digest) ||
        !parse_string_vector(reader, patch.capabilities)) {
      return false;
    }
    std::uint32_t operation_count = 0;
    if (!reader.read_u32(operation_count)) {
      return false;
    }
    patch.operations.reserve(operation_count);
    for (std::uint32_t j = 0; j < operation_count; ++j) {
      modern::AgentPatchOperation operation;
      if (!reader.read_string(operation.op) ||
          !reader.read_string(operation.symbol_id) ||
          !reader.read_string(operation.new_name) ||
          !reader.read_u32(operation.flags)) {
        return false;
      }
      patch.operations.push_back(std::move(operation));
    }
    if (!reader.read_u32(patch.flags)) {
      return false;
    }
    patches.push_back(modern::normalize_agent_patch(std::move(patch)));
  }

  std::uint32_t provenance_count = 0;
  if (!reader.read_u32(provenance_count)) {
    return false;
  }
  std::vector<modern::ProvenanceRecord> provenance;
  provenance.reserve(provenance_count);
  for (std::uint32_t i = 0; i < provenance_count; ++i) {
    modern::ProvenanceRecord record;
    if (!reader.read_string(record.patch_id) ||
        !reader.read_string(record.tool) ||
        !reader.read_string(record.request_digest) ||
        !parse_string_vector(reader, record.files_changed) ||
        !parse_string_vector(reader, record.symbols_changed) ||
        !parse_string_vector(reader, record.diagnostics_before) ||
        !parse_string_vector(reader, record.diagnostics_after) ||
        !parse_string_vector(reader, record.checks_run) ||
        !parse_string_vector(reader, record.artifact_digests) ||
        !reader.read_string(record.human_approval) ||
        !reader.read_u32(record.flags)) {
      return false;
    }
    provenance.push_back(
        modern::normalize_provenance_record(std::move(record)));
  }

  const modern::AgentValidationResult validated =
      modern::validate_agent_metadata(symbols, patches, provenance);
  symbols_out = validated.symbols;
  patches_out = validated.patches;
  prov_out = validated.provenance;
  return true;
}

bool parse_contract_metadata(
    Reader &reader, std::vector<modern::ContractSpec> &contracts_out,
    std::vector<modern::PropertySpec> &properties_out) {
  std::uint32_t contract_count = 0;
  if (!reader.read_u32(contract_count)) {
    return false;
  }
  std::vector<modern::ContractSpec> contracts;
  contracts.reserve(contract_count);
  for (std::uint32_t i = 0; i < contract_count; ++i) {
    modern::ContractSpec contract;
    if (!reader.read_string(contract.owner) ||
        !reader.read_string(contract.kind) ||
        !reader.read_string(contract.expression) ||
        !parse_string_vector(reader, contract.effect_row) ||
        !parse_source_location(reader, contract.source) ||
        !reader.read_u32(contract.flags)) {
      return false;
    }
    contracts.push_back(modern::normalize_contract_spec(std::move(contract)));
  }
  std::uint32_t property_count = 0;
  if (!reader.read_u32(property_count)) {
    return false;
  }
  std::vector<modern::PropertySpec> properties;
  properties.reserve(property_count);
  for (std::uint32_t i = 0; i < property_count; ++i) {
    modern::PropertySpec property;
    if (!reader.read_string(property.name) ||
        !reader.read_string(property.owner) ||
        !reader.read_u64(property.seed) ||
        !reader.read_string(property.generator) ||
        !reader.read_string(property.shrinker_path) ||
        !reader.read_string(property.counterexample) ||
        !parse_string_vector(reader, property.profile_set) ||
        !parse_string_vector(reader, property.dependency_fingerprints) ||
        !reader.read_u32(property.flags)) {
      return false;
    }
    properties.push_back(modern::normalize_property_spec(std::move(property)));
  }
  const modern::ContractValidationResult validated =
      modern::validate_contract_metadata(contracts, properties);
  contracts_out = validated.contracts;
  properties_out = validated.properties;
  return true;
}

bool parse_privacy_metadata(
    Reader &reader, std::vector<modern::PrivacyLabel> &labels_out,
    std::vector<modern::PrivacyPolicyRule> &policies_out,
    std::vector<modern::LineageNode> &lineage_out) {
  std::uint32_t label_count = 0;
  if (!reader.read_u32(label_count)) {
    return false;
  }
  std::vector<modern::PrivacyLabel> labels;
  labels.reserve(label_count);
  for (std::uint32_t i = 0; i < label_count; ++i) {
    modern::PrivacyLabel label;
    if (!reader.read_string(label.name) || !reader.read_string(label.kind) ||
        !reader.read_u32(label.flags)) {
      return false;
    }
    labels.push_back(modern::normalize_privacy_label(std::move(label)));
  }
  std::uint32_t policy_count = 0;
  if (!reader.read_u32(policy_count)) {
    return false;
  }
  std::vector<modern::PrivacyPolicyRule> policies;
  policies.reserve(policy_count);
  for (std::uint32_t i = 0; i < policy_count; ++i) {
    modern::PrivacyPolicyRule rule;
    if (!reader.read_string(rule.policy) || !reader.read_string(rule.action) ||
        !reader.read_string(rule.label) ||
        !reader.read_string(rule.aggregate) ||
        !reader.read_u32(rule.min_group) || !reader.read_u32(rule.flags)) {
      return false;
    }
    policies.push_back(modern::normalize_privacy_policy_rule(std::move(rule)));
  }
  std::uint32_t lineage_count = 0;
  if (!reader.read_u32(lineage_count)) {
    return false;
  }
  std::vector<modern::LineageNode> lineage;
  lineage.reserve(lineage_count);
  for (std::uint32_t i = 0; i < lineage_count; ++i) {
    modern::LineageNode node;
    if (!reader.read_string(node.node_id) || !reader.read_string(node.kind) ||
        !parse_string_vector(reader, node.inputs) ||
        !reader.read_string(node.output) ||
        !reader.read_string(node.schema_fingerprint) ||
        !parse_string_vector(reader, node.labels) ||
        !parse_source_location(reader, node.source) ||
        !reader.read_string(node.trace_span) || !reader.read_u32(node.flags)) {
      return false;
    }
    lineage.push_back(modern::normalize_lineage_node(std::move(node)));
  }
  const modern::PrivacyValidationResult validated =
      modern::validate_privacy_metadata(labels, policies, lineage);
  labels_out = validated.labels;
  policies_out = validated.policies;
  lineage_out = validated.lineage;
  return true;
}

bool parse_workflow_metadata(
    Reader &reader, std::vector<modern::WorkflowStep> &steps_out,
    std::vector<modern::WorkflowHistoryEvent> &history_out) {
  std::uint32_t step_count = 0;
  if (!reader.read_u32(step_count)) {
    return false;
  }
  std::vector<modern::WorkflowStep> steps;
  steps.reserve(step_count);
  for (std::uint32_t i = 0; i < step_count; ++i) {
    modern::WorkflowStep step;
    if (!reader.read_string(step.workflow) || !reader.read_string(step.name) ||
        !parse_string_vector(reader, step.effect_row) ||
        !parse_string_vector(reader, step.depends_on) ||
        !reader.read_string(step.retry_policy) ||
        !reader.read_string(step.idempotency_key) ||
        !reader.read_u32(step.timeout_ms) || !reader.read_u32(step.flags)) {
      return false;
    }
    steps.push_back(modern::normalize_workflow_step(std::move(step)));
  }
  std::uint32_t history_count = 0;
  if (!reader.read_u32(history_count)) {
    return false;
  }
  std::vector<modern::WorkflowHistoryEvent> history;
  history.reserve(history_count);
  for (std::uint32_t i = 0; i < history_count; ++i) {
    modern::WorkflowHistoryEvent event;
    if (!reader.read_string(event.workflow_id) ||
        !reader.read_u32(event.workflow_version) ||
        !reader.read_string(event.step) || !reader.read_string(event.event) ||
        !reader.read_string(event.input_digest) ||
        !reader.read_string(event.output_digest) ||
        !reader.read_string(event.schema_version) ||
        !parse_string_vector(reader, event.effect_grants) ||
        !reader.read_string(event.idempotency_key) ||
        !reader.read_string(event.trace_id) || !reader.read_u32(event.flags)) {
      return false;
    }
    history.push_back(
        modern::normalize_workflow_history_event(std::move(event)));
  }
  const modern::WorkflowValidationResult validated =
      modern::validate_workflow_metadata(steps, history);
  steps_out = validated.steps;
  history_out = validated.history;
  return true;
}

bool parse_hashes(Reader &reader, std::vector<HashEntry> &out) {
  std::uint32_t count = 0;
  if (!reader.read_u32(count)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::array<char, 4> raw_tag{};
    std::vector<std::uint8_t> digest;
    std::uint32_t size = 0;
    for (char &ch : raw_tag) {
      std::uint8_t byte = 0;
      if (!reader.read_u8(byte)) {
        return false;
      }
      ch = static_cast<char>(byte);
    }
    SectionKind kind = SectionKind::Hash;
    if (!decode_section_kind(raw_tag, kind)) {
      reader.fail("BC1105", "HASH entry references unknown section kind",
                  reader.pos - 4U);
      return false;
    }
    if (!reader.read_u32(size) || !reader.read_bytes(size, digest)) {
      return false;
    }
    out.push_back({kind, std::move(digest)});
  }
  return true;
}

template <typename T>
bool contains_index(const std::vector<T> &items, std::uint32_t index) {
  return index < items.size();
}

bool code_id_exists(const std::unordered_map<std::uint32_t, std::size_t> &ids,
                    std::uint32_t code_id) {
  return ids.find(code_id) != ids.end();
}

const BcCode *
code_by_id(const BcModule &module,
           const std::unordered_map<std::uint32_t, std::size_t> &ids,
           std::uint32_t code_id) {
  const auto found = ids.find(code_id);
  if (found == ids.end()) {
    return nullptr;
  }
  return &module.code_objects[found->second];
}

void add_verify_error(std::vector<VerifyError> &errors, const std::string &code,
                      const std::string &message, SectionKind section,
                      std::uint64_t offset) {
  errors.push_back({code, message, section_kind_name(section), offset});
}

struct InstructionFlow {
  std::vector<std::uint32_t> reads;
  std::vector<std::uint32_t> writes;
  std::vector<std::uint32_t> successors;
};

bool operand_u32_for_verify(const Instruction &instruction,
                            std::size_t operand_index, std::uint32_t *out) {
  if (operand_index >= instruction.operands.size()) {
    return false;
  }
  const std::int64_t value = instruction.operands[operand_index].value;
  if (value < 0) {
    return false;
  }
  *out = static_cast<std::uint32_t>(value);
  return true;
}

bool operand_count_is(const Instruction &instruction, std::size_t expected,
                      std::vector<VerifyError> &errors) {
  if (instruction.operands.size() == expected) {
    return true;
  }
  add_verify_error(errors, "BC1310", "invalid instruction operand count",
                   SectionKind::Code, 0);
  return false;
}

bool operand_count_between(const Instruction &instruction, std::size_t min,
                           std::size_t max, std::vector<VerifyError> &errors) {
  if (instruction.operands.size() >= min &&
      instruction.operands.size() <= max) {
    return true;
  }
  add_verify_error(errors, "BC1310", "invalid instruction operand count",
                   SectionKind::Code, 0);
  return false;
}

bool read_u32_operand(const Instruction &instruction, std::size_t operand_index,
                      const std::string &message,
                      std::vector<VerifyError> &errors, std::uint32_t *out) {
  if (operand_u32_for_verify(instruction, operand_index, out)) {
    return true;
  }
  add_verify_error(errors, "BC1314", message, SectionKind::Code, 0);
  return false;
}

bool register_operand(const BcCode &code, const Instruction &instruction,
                      std::size_t operand_index, const std::string &message,
                      std::vector<VerifyError> &errors, std::uint32_t *out) {
  if (!read_u32_operand(instruction, operand_index, message, errors, out)) {
    return false;
  }
  if (*out >= code.reg_count) {
    add_verify_error(errors, "BC1311", "register operand is out of range",
                     SectionKind::Code, 0);
    return false;
  }
  return true;
}

void add_register_read(const BcCode &code, const Instruction &instruction,
                       std::size_t operand_index, InstructionFlow &flow,
                       std::vector<VerifyError> &errors) {
  std::uint32_t reg = 0;
  if (register_operand(code, instruction, operand_index,
                       "register operand must be unsigned", errors, &reg)) {
    flow.reads.push_back(reg);
  }
}

void add_register_write(const BcCode &code, const Instruction &instruction,
                        std::size_t operand_index, InstructionFlow &flow,
                        std::vector<VerifyError> &errors) {
  std::uint32_t reg = 0;
  if (register_operand(code, instruction, operand_index,
                       "register operand must be unsigned", errors, &reg)) {
    flow.writes.push_back(reg);
  }
}

void add_register_range_read(const BcCode &code, std::uint32_t first_reg,
                             std::uint32_t count, InstructionFlow &flow,
                             std::vector<VerifyError> &errors) {
  if (count == 0U) {
    return;
  }
  if (first_reg >= code.reg_count || count > code.reg_count - first_reg) {
    add_verify_error(errors, "BC1311", "register range is out of range",
                     SectionKind::Code, 0);
    return;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    flow.reads.push_back(first_reg + index);
  }
}

bool verify_const_ref(const BcModule &module, std::uint32_t const_id,
                      const std::string &message,
                      std::vector<VerifyError> &errors) {
  if (contains_index(module.const_pool, const_id)) {
    return true;
  }
  add_verify_error(errors, "BC1312", message, SectionKind::Code, 0);
  return false;
}

bool verify_symbol_ref(const BcModule &module, std::uint32_t symbol_id,
                       const std::string &message,
                       std::vector<VerifyError> &errors) {
  if (contains_index(module.symbols, symbol_id)) {
    return true;
  }
  add_verify_error(errors, "BC1312", message, SectionKind::Code, 0);
  return false;
}

bool verify_code_ref(const std::unordered_map<std::uint32_t, std::size_t> &ids,
                     std::uint32_t code_id, const std::string &message,
                     std::vector<VerifyError> &errors) {
  if (code_id_exists(ids, code_id)) {
    return true;
  }
  add_verify_error(errors, "BC1312", message, SectionKind::Code, 0);
  return false;
}

void add_fallthrough_successor(std::size_t pc, std::size_t insn_count,
                               InstructionFlow &flow,
                               std::vector<VerifyError> &errors) {
  if (pc + 1U < insn_count) {
    flow.successors.push_back(static_cast<std::uint32_t>(pc + 1U));
    return;
  }
  add_verify_error(errors, "BC1315", "instruction falls through code end",
                   SectionKind::Code, 0);
}

void add_jump_successor(std::uint32_t target, InstructionFlow &flow) {
  flow.successors.push_back(target);
}

bool verify_target(std::uint32_t target, std::size_t insn_count,
                   const std::string &message,
                   std::vector<VerifyError> &errors) {
  if (static_cast<std::size_t>(target) < insn_count) {
    return true;
  }
  add_verify_error(errors, "BC1302", message, SectionKind::Code, 0);
  return false;
}

bool local_layout_role_is(const BcModule &module, const SlotLayoutEntry &entry,
                          const std::string &role) {
  return contains_index(module.strings, entry.role_str_id) &&
         module.strings[entry.role_str_id] == role;
}

std::vector<std::uint8_t> initial_register_state(const BcModule &module,
                                                 const BcCode &code) {
  std::vector<std::uint8_t> state(code.reg_count, 0U);
  for (const SlotLayoutEntry &entry : code.local_layout) {
    if (entry.slot >= code.reg_count) {
      continue;
    }
    if (local_layout_role_is(module, entry, "param") ||
        local_layout_role_is(module, entry, "implicit_block_param") ||
        local_layout_role_is(module, entry, "pattern")) {
      state[entry.slot] = 1U;
    }
  }
  if ((code.kind == CodeKind::Rescue || code.kind == CodeKind::Ensure) &&
      !state.empty()) {
    state[0] = 1U;
  }
  return state;
}

void verify_initializedness(const BcModule &module, const BcCode &code,
                            const std::vector<InstructionFlow> &flows,
                            std::vector<VerifyError> &errors) {
  if (code.instructions.empty()) {
    return;
  }
  std::vector<std::vector<std::uint8_t>> states(
      code.instructions.size(), std::vector<std::uint8_t>(code.reg_count, 0U));
  std::vector<std::uint8_t> seen(code.instructions.size(), 0U);
  std::vector<std::uint32_t> worklist;
  states[0] = initial_register_state(module, code);
  seen[0] = 1U;
  worklist.push_back(0);

  while (!worklist.empty()) {
    const std::uint32_t pc = worklist.back();
    worklist.pop_back();
    if (pc >= flows.size()) {
      continue;
    }
    std::vector<std::uint8_t> out = states[pc];
    for (std::uint32_t reg : flows[pc].reads) {
      if (reg < out.size() && out[reg] == 0U) {
        add_verify_error(errors, "BC1313",
                         "register read before definite initialization",
                         SectionKind::Code, 0);
      }
    }
    for (std::uint32_t reg : flows[pc].writes) {
      if (reg < out.size()) {
        out[reg] = 1U;
      }
    }
    for (std::uint32_t successor : flows[pc].successors) {
      if (successor >= states.size()) {
        continue;
      }
      if (seen[successor] == 0U) {
        states[successor] = out;
        seen[successor] = 1U;
        worklist.push_back(successor);
        continue;
      }
      bool changed = false;
      for (std::size_t reg = 0; reg < out.size(); ++reg) {
        const std::uint8_t merged = states[successor][reg] & out[reg];
        if (merged != states[successor][reg]) {
          states[successor][reg] = merged;
          changed = true;
        }
      }
      if (changed) {
        worklist.push_back(successor);
      }
    }
  }
}

InstructionFlow verify_instruction_flow(
    const BcModule &module,
    const std::unordered_map<std::uint32_t, std::size_t> &code_ids,
    const BcCode &code, std::size_t pc, std::vector<VerifyError> &errors) {
  const Instruction &instruction = code.instructions[pc];
  const std::size_t insn_count = code.instructions.size();
  InstructionFlow flow;
  bool falls_through = true;

  auto read_target = [&](std::size_t operand_index,
                         std::uint32_t *target) -> bool {
    if (!read_u32_operand(instruction, operand_index,
                          "jump target must be unsigned", errors, target)) {
      return false;
    }
    return verify_target(*target, insn_count, "jump target is out of range",
                         errors);
  };

  switch (instruction.opcode) {
  case Opcode::LoadK: {
    if (operand_count_is(instruction, 2, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      std::uint32_t const_id = 0;
      if (read_u32_operand(instruction, 1, "constant ref must be unsigned",
                           errors, &const_id)) {
        verify_const_ref(module, const_id, "constant ref is out of range",
                         errors);
      }
    }
    break;
  }
  case Opcode::LoadNull:
  case Opcode::LoadSelf:
  case Opcode::GetLast: {
    if (operand_count_is(instruction, 1, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
    }
    break;
  }
  case Opcode::LoadBool: {
    if (operand_count_is(instruction, 2, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
    }
    break;
  }
  case Opcode::Move:
  case Opcode::Freeze:
  case Opcode::ObjDestroy:
  case Opcode::ObjDealloc:
  case Opcode::TripleEq:
  case Opcode::InOp: {
    const std::size_t expected = (instruction.opcode == Opcode::TripleEq ||
                                  instruction.opcode == Opcode::InOp)
                                     ? 3U
                                     : 2U;
    if (operand_count_is(instruction, expected, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      for (std::size_t index = 1; index < expected; ++index) {
        add_register_read(code, instruction, index, flow, errors);
      }
    }
    break;
  }
  case Opcode::SetLast:
  case Opcode::Raise: {
    if (operand_count_is(instruction, 1, errors)) {
      add_register_read(code, instruction, 0, flow, errors);
    }
    if (instruction.opcode == Opcode::Raise) {
      falls_through = false;
    }
    break;
  }
  case Opcode::Return: {
    if (operand_count_is(instruction, 1, errors)) {
      add_register_read(code, instruction, 0, flow, errors);
    }
    falls_through = false;
    break;
  }
  case Opcode::MakeList:
  case Opcode::MakeSet:
  case Opcode::MakeTuple: {
    if (operand_count_is(instruction, 3, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      std::uint32_t first_reg = 0;
      std::uint32_t count = 0;
      if (read_u32_operand(instruction, 1, "register operand must be unsigned",
                           errors, &first_reg) &&
          read_u32_operand(instruction, 2, "count operand must be unsigned",
                           errors, &count)) {
        add_register_range_read(code, first_reg, count, flow, errors);
      }
    }
    break;
  }
  case Opcode::MakeMap: {
    std::uint32_t count = 0;
    if (instruction.operands.size() >= 2U &&
        read_u32_operand(instruction, 1, "map count must be unsigned", errors,
                         &count) &&
        operand_count_is(instruction, 2U + static_cast<std::size_t>(count) * 2U,
                         errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      std::size_t operand_index = 2;
      for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t symbol_id = 0;
        if (read_u32_operand(instruction, operand_index++,
                             "symbol ref must be unsigned", errors,
                             &symbol_id)) {
          verify_symbol_ref(module, symbol_id, "map key symbol ref is invalid",
                            errors);
        }
        add_register_read(code, instruction, operand_index++, flow, errors);
      }
    } else if (instruction.operands.size() < 2U) {
      add_verify_error(errors, "BC1310", "invalid instruction operand count",
                       SectionKind::Code, 0);
    }
    break;
  }
  case Opcode::MakeMapDyn: {
    std::uint32_t count = 0;
    if (instruction.operands.size() >= 2U &&
        read_u32_operand(instruction, 1, "map count must be unsigned", errors,
                         &count) &&
        operand_count_is(instruction, 2U + static_cast<std::size_t>(count) * 2U,
                         errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      std::size_t operand_index = 2;
      for (std::uint32_t index = 0; index < count; ++index) {
        add_register_read(code, instruction, operand_index++, flow, errors);
        add_register_read(code, instruction, operand_index++, flow, errors);
      }
    } else if (instruction.operands.size() < 2U) {
      add_verify_error(errors, "BC1310", "invalid instruction operand count",
                       SectionKind::Code, 0);
    }
    break;
  }
  case Opcode::LoadUpval: {
    if (operand_count_is(instruction, 2, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      std::uint32_t slot = 0;
      if (read_u32_operand(instruction, 1, "capture slot must be unsigned",
                           errors, &slot) &&
          slot >= code.capture_layout.size()) {
        add_verify_error(errors, "BC1312", "capture slot is out of range",
                         SectionKind::Code, 0);
      }
    }
    break;
  }
  case Opcode::StoreUpval: {
    if (operand_count_is(instruction, 2, errors)) {
      std::uint32_t slot = 0;
      if (read_u32_operand(instruction, 0, "capture slot must be unsigned",
                           errors, &slot) &&
          slot >= code.capture_layout.size()) {
        add_verify_error(errors, "BC1312", "capture slot is out of range",
                         SectionKind::Code, 0);
      }
      add_register_read(code, instruction, 1, flow, errors);
    }
    break;
  }
  case Opcode::LoadIvar: {
    if (operand_count_is(instruction, 4, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      add_register_read(code, instruction, 1, flow, errors);
      std::uint32_t symbol_id = 0;
      if (read_u32_operand(instruction, 2, "symbol ref must be unsigned",
                           errors, &symbol_id)) {
        verify_symbol_ref(module, symbol_id, "ivar symbol ref is invalid",
                          errors);
      }
    }
    break;
  }
  case Opcode::StoreIvar: {
    if (operand_count_is(instruction, 4, errors)) {
      add_register_read(code, instruction, 0, flow, errors);
      std::uint32_t symbol_id = 0;
      if (read_u32_operand(instruction, 1, "symbol ref must be unsigned",
                           errors, &symbol_id)) {
        verify_symbol_ref(module, symbol_id, "ivar symbol ref is invalid",
                          errors);
      }
      add_register_read(code, instruction, 2, flow, errors);
    }
    break;
  }
  case Opcode::LoadCvar: {
    if (operand_count_is(instruction, 3, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      add_register_read(code, instruction, 1, flow, errors);
      std::uint32_t symbol_id = 0;
      if (read_u32_operand(instruction, 2, "symbol ref must be unsigned",
                           errors, &symbol_id)) {
        verify_symbol_ref(module, symbol_id, "cvar symbol ref is invalid",
                          errors);
      }
    }
    break;
  }
  case Opcode::StoreCvar: {
    if (operand_count_is(instruction, 3, errors)) {
      add_register_read(code, instruction, 0, flow, errors);
      std::uint32_t symbol_id = 0;
      if (read_u32_operand(instruction, 1, "symbol ref must be unsigned",
                           errors, &symbol_id)) {
        verify_symbol_ref(module, symbol_id, "cvar symbol ref is invalid",
                          errors);
      }
      add_register_read(code, instruction, 2, flow, errors);
    }
    break;
  }
  case Opcode::LookupConst: {
    if (operand_count_is(instruction, 2, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      std::uint32_t const_id = 0;
      if (read_u32_operand(instruction, 1, "constant ref must be unsigned",
                           errors, &const_id)) {
        verify_const_ref(module, const_id, "constant ref is out of range",
                         errors);
      }
    }
    break;
  }
  case Opcode::MakeClosure: {
    std::uint32_t capture_count = 0;
    if (instruction.operands.size() >= 3U &&
        read_u32_operand(instruction, 2, "capture count must be unsigned",
                         errors, &capture_count) &&
        operand_count_is(instruction,
                         3U + static_cast<std::size_t>(capture_count) * 2U,
                         errors)) {
      std::uint32_t dst = 0;
      const bool has_dst = register_operand(
          code, instruction, 0, "register operand must be unsigned", errors,
          &dst);
      if (has_dst) {
        flow.writes.push_back(dst);
      }
      std::uint32_t closure_code_id = 0;
      if (read_u32_operand(instruction, 1, "code ref must be unsigned", errors,
                           &closure_code_id)) {
        verify_code_ref(code_ids, closure_code_id, "closure code id is unknown",
                        errors);
      }
      std::size_t operand_index = 3;
      for (std::uint32_t index = 0; index < capture_count; ++index) {
        std::uint32_t kind = 0;
        std::uint32_t slot = 0;
        const bool has_kind =
            read_u32_operand(instruction, operand_index++,
                             "capture kind must be unsigned", errors, &kind);
        const bool has_slot =
            read_u32_operand(instruction, operand_index++,
                             "capture slot must be unsigned", errors, &slot);
        if (!has_kind || !has_slot) {
          continue;
        }
        if (kind == 0U) {
          if (slot >= code.reg_count) {
            add_verify_error(errors, "BC1311",
                             "closure capture register is out of range",
                             SectionKind::Code, 0);
          } else if (!has_dst || slot != dst) {
            flow.reads.push_back(slot);
          } else {
            // A closure may capture itself while being written into its own
            // target register. The VM materializes that capture from the
            // newly allocated closure instead of reading an initialized reg.
          }
        } else if (kind == 1U) {
          if (slot >= code.capture_layout.size()) {
            add_verify_error(errors, "BC1312",
                             "closure capture slot is out of range",
                             SectionKind::Code, 0);
          }
        } else {
          add_verify_error(errors, "BC1314", "invalid closure capture kind",
                           SectionKind::Code, 0);
        }
      }
    } else if (instruction.operands.size() < 3U) {
      add_verify_error(errors, "BC1310", "invalid instruction operand count",
                       SectionKind::Code, 0);
    }
    break;
  }
  case Opcode::CloseUpvalues: {
    if (operand_count_is(instruction, 1, errors)) {
      std::uint32_t from_slot = 0;
      if (read_u32_operand(instruction, 0, "slot operand must be unsigned",
                           errors, &from_slot) &&
          from_slot > code.reg_count) {
        add_verify_error(errors, "BC1311",
                         "close-upvalues slot is out of range",
                         SectionKind::Code, 0);
      }
    }
    break;
  }
  case Opcode::WatchLocal: {
    if (operand_count_is(instruction, 3, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      std::uint32_t slot = 0;
      if (read_u32_operand(instruction, 1, "watch slot must be unsigned",
                           errors, &slot)) {
        if (slot >= code.reg_count) {
          add_verify_error(errors, "BC1311",
                           "watch local register is out of range",
                           SectionKind::Code, 0);
        } else {
          flow.reads.push_back(slot);
        }
      }
    }
    break;
  }
  case Opcode::WatchUpval: {
    if (operand_count_is(instruction, 3, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      std::uint32_t slot = 0;
      if (read_u32_operand(instruction, 1, "watch capture slot must be unsigned",
                           errors, &slot) &&
          slot >= code.capture_layout.size()) {
        add_verify_error(errors, "BC1312", "watch capture slot is out of range",
                         SectionKind::Code, 0);
      }
    }
    break;
  }
  case Opcode::WatchIvar: {
    if (operand_count_is(instruction, 5, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      add_register_read(code, instruction, 1, flow, errors);
      std::uint32_t symbol_id = 0;
      if (read_u32_operand(instruction, 2, "symbol ref must be unsigned",
                           errors, &symbol_id)) {
        verify_symbol_ref(module, symbol_id, "ivar symbol ref is invalid",
                          errors);
      }
    }
    break;
  }
  case Opcode::Send:
  case Opcode::SendDyn:
  case Opcode::Call: {
    const bool is_call = instruction.opcode == Opcode::Call;
    const bool is_dynamic = instruction.opcode == Opcode::SendDyn;
    std::size_t operand_index = 0;
    const std::size_t fixed_prefix = is_call ? 2U : 3U;
    if (instruction.operands.size() < fixed_prefix + 3U) {
      add_verify_error(errors, "BC1310", "invalid instruction operand count",
                       SectionKind::Code, 0);
      break;
    }
    add_register_write(code, instruction, operand_index++, flow, errors);
    add_register_read(code, instruction, operand_index++, flow, errors);
    if (!is_call) {
      if (is_dynamic) {
        add_register_read(code, instruction, operand_index++, flow, errors);
      } else {
        std::uint32_t selector_id = 0;
        if (read_u32_operand(instruction, operand_index++,
                             "selector ref must be unsigned", errors,
                             &selector_id)) {
          verify_symbol_ref(module, selector_id,
                            "selector symbol ref is invalid", errors);
        }
      }
    }
    std::uint32_t pos_count = 0;
    if (!read_u32_operand(instruction, operand_index++,
                          "positional count must be unsigned", errors,
                          &pos_count)) {
      break;
    }
    if (instruction.operands.size() < operand_index + pos_count + 2U) {
      add_verify_error(errors, "BC1310", "invalid instruction operand count",
                       SectionKind::Code, 0);
      break;
    }
    for (std::uint32_t index = 0; index < pos_count; ++index) {
      add_register_read(code, instruction, operand_index++, flow, errors);
    }
    std::uint32_t kw_count = 0;
    if (!read_u32_operand(instruction, operand_index++,
                          "keyword count must be unsigned", errors,
                          &kw_count)) {
      break;
    }
    if (instruction.operands.size() < operand_index + kw_count * 2U + 1U) {
      add_verify_error(errors, "BC1310", "invalid instruction operand count",
                       SectionKind::Code, 0);
      break;
    }
    for (std::uint32_t index = 0; index < kw_count; ++index) {
      std::uint32_t symbol_id = 0;
      if (read_u32_operand(instruction, operand_index++,
                           "keyword symbol ref must be unsigned", errors,
                           &symbol_id)) {
        verify_symbol_ref(module, symbol_id, "keyword symbol ref is invalid",
                          errors);
      }
      add_register_read(code, instruction, operand_index++, flow, errors);
    }
    const std::int64_t block_reg = instruction.operands[operand_index++].value;
    if (block_reg >= 0 &&
        block_reg != static_cast<std::int64_t>(
                         std::numeric_limits<std::uint32_t>::max())) {
      if (static_cast<std::uint64_t>(block_reg) >= code.reg_count) {
        add_verify_error(errors, "BC1311", "block register is out of range",
                         SectionKind::Code, 0);
      } else {
        flow.reads.push_back(static_cast<std::uint32_t>(block_reg));
      }
    }
    if (!operand_count_between(instruction, operand_index, operand_index + 1U,
                               errors)) {
      break;
    }
    if (instruction.operands.size() == operand_index + 1U) {
      std::uint32_t site_id = 0;
      if (read_u32_operand(instruction, operand_index,
                           "call-site id must be unsigned", errors, &site_id) &&
          site_id >= code.call_site_table.size()) {
        add_verify_error(errors, "BC1312", "call-site id is out of range",
                         SectionKind::Code, 0);
      }
    }
    break;
  }
  case Opcode::IAdd:
  case Opcode::ISub:
  case Opcode::ILt:
  case Opcode::IGt:
  case Opcode::IMul:
  case Opcode::IDiv:
  case Opcode::IMod:
  case Opcode::IFloorDiv:
  case Opcode::ILe:
  case Opcode::IGe:
  case Opcode::IEq:
  case Opcode::INe:
  case Opcode::ICmp: {
    if (operand_count_is(instruction, 3, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      add_register_read(code, instruction, 1, flow, errors);
      add_register_read(code, instruction, 2, flow, errors);
    }
    break;
  }
  case Opcode::IAddK:
  case Opcode::ISubK:
  case Opcode::ILtK:
  case Opcode::IGtK:
  case Opcode::IMulK:
  case Opcode::IDivK:
  case Opcode::IModK:
  case Opcode::IFloorDivK:
  case Opcode::ILeK:
  case Opcode::IGeK:
  case Opcode::IEqK:
  case Opcode::INeK:
  case Opcode::ICmpK: {
    if (operand_count_is(instruction, 3, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      add_register_read(code, instruction, 1, flow, errors);
      std::uint32_t const_id = 0;
      if (read_u32_operand(instruction, 2,
                           "integer constant ref must be unsigned", errors,
                           &const_id) &&
          verify_const_ref(module, const_id,
                           "integer constant ref is out of range", errors) &&
          module.const_pool[const_id].kind != ConstantKind::Integer) {
        add_verify_error(errors, "BC1314",
                         "integer opcode constant ref must be integer",
                         SectionKind::Code, 0);
      }
    }
    break;
  }
  case Opcode::TypeCheck: {
    if (operand_count_is(instruction, 2, errors)) {
      add_register_read(code, instruction, 0, flow, errors);
      std::uint32_t const_id = 0;
      if (read_u32_operand(instruction, 1, "type hook ref must be unsigned",
                           errors, &const_id)) {
        verify_const_ref(module, const_id, "type hook ref is out of range",
                         errors);
      }
    }
    break;
  }
  case Opcode::Jump: {
    if (operand_count_is(instruction, 1, errors)) {
      std::uint32_t target = 0;
      if (read_target(0, &target)) {
        add_jump_successor(target, flow);
      }
    }
    falls_through = false;
    break;
  }
  case Opcode::JumpIfTrue:
  case Opcode::JumpIfFalse:
  case Opcode::JumpIfNull: {
    if (operand_count_is(instruction, 2, errors)) {
      add_register_read(code, instruction, 0, flow, errors);
      std::uint32_t target = 0;
      if (read_target(1, &target)) {
        add_jump_successor(target, flow);
      }
    }
    break;
  }
  case Opcode::Safepoint: {
    operand_count_is(instruction, 0, errors);
    break;
  }
  case Opcode::PPrepSeq: {
    if (operand_count_is(instruction, 4, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      add_register_read(code, instruction, 1, flow, errors);
      std::uint32_t target = 0;
      if (read_target(3, &target)) {
        add_jump_successor(target, flow);
      }
    }
    break;
  }
  case Opcode::PPrepMap: {
    if (operand_count_is(instruction, 5, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      add_register_read(code, instruction, 1, flow, errors);
      std::uint32_t keyset_id = 0;
      if (read_u32_operand(instruction, 2, "keyset ref must be unsigned",
                           errors, &keyset_id)) {
        verify_const_ref(module, keyset_id, "keyset ref is out of range",
                         errors);
      }
      std::uint32_t target = 0;
      if (read_target(4, &target)) {
        add_jump_successor(target, flow);
      }
    }
    break;
  }
  case Opcode::PCheckEq:
  case Opcode::PCheckPin:
  case Opcode::PCheckLenEq:
  case Opcode::PCheckLenGte:
  case Opcode::PHasKey:
  case Opcode::PTripleEq: {
    if (operand_count_is(instruction, 3, errors)) {
      add_register_read(code, instruction, 0, flow, errors);
      if (instruction.opcode == Opcode::PCheckPin ||
          instruction.opcode == Opcode::PTripleEq) {
        add_register_read(code, instruction, 1, flow, errors);
      } else if (instruction.opcode == Opcode::PCheckEq) {
        std::uint32_t const_id = 0;
        if (read_u32_operand(instruction, 1, "constant ref must be unsigned",
                             errors, &const_id)) {
          verify_const_ref(module, const_id, "constant ref is out of range",
                           errors);
        }
      } else if (instruction.opcode == Opcode::PHasKey) {
        std::uint32_t symbol_id = 0;
        if (read_u32_operand(instruction, 1, "symbol ref must be unsigned",
                             errors, &symbol_id)) {
          verify_symbol_ref(module, symbol_id, "map key symbol ref is invalid",
                            errors);
        }
      }
      std::uint32_t target = 0;
      if (read_target(2, &target)) {
        add_jump_successor(target, flow);
      }
    }
    break;
  }
  case Opcode::PGetIndex:
  case Opcode::PGetKey: {
    if (operand_count_is(instruction, 3, errors)) {
      add_register_write(code, instruction, 0, flow, errors);
      add_register_read(code, instruction, 1, flow, errors);
      if (instruction.opcode == Opcode::PGetKey) {
        std::uint32_t symbol_id = 0;
        if (read_u32_operand(instruction, 2, "symbol ref must be unsigned",
                             errors, &symbol_id)) {
          verify_symbol_ref(module, symbol_id, "map key symbol ref is invalid",
                            errors);
        }
      }
    }
    break;
  }
  case Opcode::PBind: {
    if (operand_count_is(instruction, 2, errors)) {
      std::uint32_t slot = 0;
      if (read_u32_operand(instruction, 0, "binding slot must be unsigned",
                           errors, &slot) &&
          slot >= code.reg_count) {
        add_verify_error(errors, "BC1311", "binding slot is out of range",
                         SectionKind::Code, 0);
      }
      add_register_read(code, instruction, 1, flow, errors);
    }
    break;
  }
  case Opcode::PCommit: {
    if (operand_count_is(instruction, 2, errors)) {
      std::uint32_t base_slot = 0;
      std::uint32_t count = 0;
      if (read_u32_operand(instruction, 0, "binding slot must be unsigned",
                           errors, &base_slot) &&
          read_u32_operand(instruction, 1, "binding count must be unsigned",
                           errors, &count)) {
        if (count > 0U && (base_slot >= code.reg_count ||
                           count > code.reg_count - base_slot)) {
          add_verify_error(errors, "BC1311",
                           "binding slot range is out of range",
                           SectionKind::Code, 0);
        } else {
          for (std::uint32_t index = 0; index < count; ++index) {
            flow.writes.push_back(base_slot + index);
          }
        }
      }
    }
    break;
  }
  case Opcode::PFail: {
    if (operand_count_is(instruction, 1, errors)) {
      if (instruction.operands[0].value != 0) {
        falls_through = false;
      }
    }
    break;
  }
  }

  if (falls_through) {
    add_fallthrough_successor(pc, insn_count, flow, errors);
  }
  return flow;
}

void verify_module(BcModule &module, std::vector<VerifyError> &errors) {
  const std::uint16_t kSupportedFormatMajor = 1;
  const std::uint16_t kSupportedFormatMinor = 0;
  const std::uint16_t kSupportedLanguageMajor = 1;
  const std::uint16_t kSupportedLanguageMinor = 0;

  if (module.format_version.major != kSupportedFormatMajor ||
      module.format_version.minor > kSupportedFormatMinor) {
    errors.push_back(
        {"BC1003", "unsupported bytecode format version", "header", 4});
  }
  if (module.language_version.major != kSupportedLanguageMajor ||
      module.language_version.minor > kSupportedLanguageMinor) {
    errors.push_back({"BC1004", "unsupported language version", "header", 8});
  }

  std::unordered_map<std::uint32_t, std::size_t> code_ids;
  for (std::size_t i = 0; i < module.code_objects.size(); ++i) {
    const auto [_, inserted] =
        code_ids.emplace(module.code_objects[i].code_id, i);
    if (!inserted) {
      add_verify_error(errors, "BC1201", "duplicate code id", SectionKind::Code,
                       0);
    }
  }

  std::unordered_map<std::uint32_t, std::size_t> pattern_ids;
  for (std::size_t i = 0; i < module.pattern_programs.size(); ++i) {
    const auto [_, inserted] =
        pattern_ids.emplace(module.pattern_programs[i].pattern_id, i);
    if (!inserted) {
      add_verify_error(errors, "BC1207", "duplicate pattern program id",
                       SectionKind::Pats, 0);
    }
  }

  for (const Constant &constant : module.const_pool) {
    switch (constant.kind) {
    case ConstantKind::Null:
    case ConstantKind::Bool:
    case ConstantKind::Integer:
    case ConstantKind::Float:
      break;
    case ConstantKind::SymbolRef:
      if (!contains_index(module.symbols, constant.ref_id)) {
        add_verify_error(errors, "BC1203",
                         "constant symbol ref is out of range",
                         SectionKind::Kons, 0);
      }
      break;
    case ConstantKind::StringRef:
      if (!contains_index(module.strings, constant.ref_id)) {
        add_verify_error(errors, "BC1202",
                         "constant string ref is out of range",
                         SectionKind::Kons, 0);
      }
      break;
    case ConstantKind::CodeRef:
      if (!code_id_exists(code_ids, constant.ref_id)) {
        add_verify_error(errors, "BC1204", "constant code ref is unknown",
                         SectionKind::Kons, 0);
      }
      break;
    case ConstantKind::KeySet:
    case ConstantKind::Path:
      for (std::uint32_t item : constant.items) {
        if (!contains_index(module.symbols, item)) {
          add_verify_error(errors, "BC1203",
                           "constant key/path symbol ref is out of range",
                           SectionKind::Kons, 0);
          break;
        }
      }
      break;
    }
  }

  for (const BcCode &code : module.code_objects) {
    const std::size_t insn_count = code.instructions.size();
    std::map<std::uint32_t, std::uint32_t> safepoints;
    for (const SafepointEntry &entry : code.safepoint_table) {
      safepoints.emplace(entry.pc, entry.flags);
      if (entry.pc >= insn_count) {
        add_verify_error(errors, "BC1306", "safepoint pc is out of range",
                         SectionKind::Code, 0);
      }
    }

    for (const SlotLayoutEntry &entry : code.local_layout) {
      if (entry.slot >= code.reg_count) {
        add_verify_error(errors, "BC1311",
                         "local layout slot is out of register range",
                         SectionKind::Code, 0);
      }
      if (!contains_index(module.strings, entry.name_str_id) ||
          !contains_index(module.strings, entry.role_str_id) ||
          !contains_index(module.strings, entry.binding_kind_str_id)) {
        add_verify_error(errors, "BC1202",
                         "local layout string ref is out of range",
                         SectionKind::Code, 0);
      }
    }
    for (const CaptureLayoutEntry &entry : code.capture_layout) {
      if (!contains_index(module.strings, entry.name_str_id) ||
          !contains_index(module.strings, entry.source_kind_str_id) ||
          !contains_index(module.strings, entry.source_name_str_id)) {
        add_verify_error(errors, "BC1202",
                         "capture layout string ref is out of range",
                         SectionKind::Code, 0);
      }
    }
    for (const CacheSiteEntry &entry : code.call_site_table) {
      if (entry.pc >= insn_count) {
        add_verify_error(errors, "BC1307", "call cache site pc is out of range",
                         SectionKind::Code, 0);
      }
      if (!contains_index(module.symbols, entry.symbol_id)) {
        add_verify_error(errors, "BC1203",
                         "call cache symbol ref is out of range",
                         SectionKind::Code, 0);
      }
    }
    for (const CacheSiteEntry &entry : code.ivar_site_table) {
      if (entry.pc >= insn_count) {
        add_verify_error(errors, "BC1307", "ivar cache site pc is out of range",
                         SectionKind::Code, 0);
      }
      if (!contains_index(module.symbols, entry.symbol_id)) {
        add_verify_error(errors, "BC1203",
                         "ivar cache symbol ref is out of range",
                         SectionKind::Code, 0);
      }
    }
    for (const SourceSpanEntry &entry : code.source_spans) {
      if (entry.pc_from > entry.pc_to || entry.pc_to > insn_count) {
        add_verify_error(errors, "BC1309", "source span range is invalid",
                         SectionKind::Span, 0);
      }
    }
    for (const HandlerEntry &entry : code.handler_table) {
      if (entry.protected_from >= entry.protected_to ||
          entry.protected_to > insn_count || entry.handler_pc >= insn_count) {
        add_verify_error(errors, "BC1304", "handler range is invalid",
                         SectionKind::Code, 0);
      }
      if (!code_id_exists(code_ids, entry.handler_code_id)) {
        add_verify_error(errors, "BC1204", "handler references unknown code id",
                         SectionKind::Code, 0);
      }
    }

    std::vector<InstructionFlow> flows;
    flows.reserve(code.instructions.size());
    for (std::size_t pc = 0; pc < code.instructions.size(); ++pc) {
      flows.push_back(
          verify_instruction_flow(module, code_ids, code, pc, errors));
    }
    for (std::size_t pc = 0; pc < flows.size(); ++pc) {
      for (std::uint32_t target : flows[pc].successors) {
        if (static_cast<std::size_t>(target) < pc &&
            safepoints.find(target) == safepoints.end()) {
          add_verify_error(errors, "BC1303",
                           "back-edge jump target is missing safepoint",
                           SectionKind::Code, 0);
        }
      }
    }
    verify_initializedness(module, code, flows, errors);
  }

  for (const BcMethod &method : module.methods) {
    if (!contains_index(module.symbols, method.selector_sym_id)) {
      add_verify_error(errors, "BC1203",
                       "method selector symbol ref is invalid",
                       SectionKind::Meth, 0);
    }
    if (method.signature_blob_id >= module.const_pool.size() &&
        !module.const_pool.empty()) {
      add_verify_error(errors, "BC1206",
                       "method signature blob ref is out of range",
                       SectionKind::Meth, 0);
    }
    std::uint32_t defaulted_param_count = 0;
    for (const MethodParamEntry &entry : method.params) {
      if (!contains_index(module.symbols, entry.external_name_sym_id) ||
          !contains_index(module.strings, entry.local_name_str_id)) {
        add_verify_error(errors, "BC1202",
                         "method param name ref is out of range",
                         SectionKind::Meth, 0);
      }
      if ((entry.flags & kMethodParamFlagHasDefault) != 0U) {
        ++defaulted_param_count;
      }
    }
    if (!method.params.empty() &&
        defaulted_param_count != method.default_thunk_ids.size()) {
      add_verify_error(errors, "BC1208",
                       "method default thunk count does not match param flags",
                       SectionKind::Meth, 0);
    }
    if (!code_id_exists(code_ids, method.entry_code_id)) {
      add_verify_error(errors, "BC1204", "method entry code id is unknown",
                       SectionKind::Meth, 0);
    }
    if ((method.flags & (kMethodFlagInstance | kMethodFlagClass)) != 0U &&
        method.owner_dispatch_ref >= module.classes.size()) {
      add_verify_error(errors, "BC1205",
                       "method owner dispatch ref is out of range",
                       SectionKind::Meth, 0);
    }
    for (std::uint32_t code_id : method.default_thunk_ids) {
      if (!code_id_exists(code_ids, code_id)) {
        add_verify_error(errors, "BC1204",
                         "default thunk references unknown code id",
                         SectionKind::Meth, 0);
      }
    }
    for (std::uint32_t code_id : method.type_hook_ids) {
      if (!code_id_exists(code_ids, code_id)) {
        add_verify_error(errors, "BC1204",
                         "type hook references unknown code id",
                         SectionKind::Meth, 0);
      }
    }
    for (const ClauseEntry &entry : method.clause_table) {
      if (!pattern_ids.empty() &&
          pattern_ids.find(entry.pattern_program_id) == pattern_ids.end()) {
        add_verify_error(errors, "BC1207",
                         "clause references unknown pattern program id",
                         SectionKind::Meth, 0);
      }
      if (!code_id_exists(code_ids, entry.pattern_code_id) ||
          !code_id_exists(code_ids, entry.guard_code_id) ||
          !code_id_exists(code_ids, entry.body_code_id)) {
        add_verify_error(errors, "BC1204", "clause references unknown code id",
                         SectionKind::Meth, 0);
      }
    }
    for (const AutoAssignEntry &entry : method.auto_assign_desc) {
      if (!contains_index(module.strings, entry.local_name_str_id) ||
          !contains_index(module.strings, entry.target_name_str_id)) {
        add_verify_error(errors, "BC1202",
                         "auto-assign string ref is out of range",
                         SectionKind::Meth, 0);
      }
    }
  }

  for (const BcClass &klass : module.classes) {
    if (!contains_index(module.symbols, klass.class_name_sym_id)) {
      add_verify_error(errors, "BC1203", "class name symbol ref is invalid",
                       SectionKind::Clas, 0);
    }
    if (klass.has_superclass_ref &&
        !constant_is_path_ref(module, klass.superclass_ref)) {
      add_verify_error(errors, "BC1206",
                       "superclass ref must point to path constant",
                       SectionKind::Clas, 0);
    }
    if (klass.ivar_schema_id >= module.const_pool.size() &&
        !module.const_pool.empty()) {
      add_verify_error(errors, "BC1206", "ivar schema ref is out of range",
                       SectionKind::Clas, 0);
    }
    if (klass.method_range_start + klass.method_range_count >
        module.methods.size()) {
      add_verify_error(errors, "BC1208", "class method range is out of range",
                       SectionKind::Clas, 0);
    }
    for (std::uint32_t ref : klass.direct_include_refs) {
      if (!constant_is_path_ref(module, ref)) {
        add_verify_error(errors, "BC1206",
                         "include ref must point to path constant",
                         SectionKind::Clas, 0);
        break;
      }
    }
    for (std::uint32_t ref : klass.direct_extend_refs) {
      if (!constant_is_path_ref(module, ref)) {
        add_verify_error(errors, "BC1206",
                         "extend ref must point to path constant",
                         SectionKind::Clas, 0);
        break;
      }
    }
    if (klass.has_class_init_code_id &&
        !code_id_exists(code_ids, klass.class_init_code_id)) {
      add_verify_error(errors, "BC1204", "class init code id is unknown",
                       SectionKind::Clas, 0);
    }
  }

  for (const DepEntry &entry : module.dependencies) {
    if (!contains_index(module.strings, entry.module_name_str_id)) {
      add_verify_error(errors, "BC1202", "dependency module name is invalid",
                       SectionKind::Deps, 0);
    }
  }

  for (const ExportEntry &entry : module.exports) {
    if (!contains_index(module.symbols, entry.symbol_id)) {
      add_verify_error(errors, "BC1203", "export symbol ref is invalid",
                       SectionKind::Expt, 0);
    }
    if (!contains_index(module.strings, entry.target_kind_str_id)) {
      add_verify_error(errors, "BC1202", "export target kind ref is invalid",
                       SectionKind::Expt, 0);
      continue;
    }
    const std::string &target_kind = module.strings[entry.target_kind_str_id];
    if (target_kind == "method" &&
        entry.target_index >= module.methods.size()) {
      add_verify_error(errors, "BC1208", "export method target is out of range",
                       SectionKind::Expt, 0);
    } else if (target_kind == "class" &&
               entry.target_index >= module.classes.size()) {
      add_verify_error(errors, "BC1205", "export class target is out of range",
                       SectionKind::Expt, 0);
    } else if (target_kind == "code" &&
               !code_id_exists(code_ids, entry.target_index)) {
      add_verify_error(errors, "BC1204", "export code target is unknown",
                       SectionKind::Expt, 0);
    }
    if (entry.has_reexport_module_name &&
        !contains_index(module.strings, entry.reexport_module_name_str_id)) {
      add_verify_error(errors, "BC1202", "re-export module name ref is invalid",
                       SectionKind::Expt, 0);
    }
  }

  if (module.init.has_entry_code_id &&
      !code_id_exists(code_ids, module.init.entry_code_id)) {
    add_verify_error(errors, "BC1204", "init entry code id is unknown",
                     SectionKind::Init, 0);
  }

  for (const LineEntry &entry : module.line_table) {
    const BcCode *code = code_by_id(module, code_ids, entry.code_id);
    if (code == nullptr) {
      add_verify_error(errors, "BC1204", "line entry code id is unknown",
                       SectionKind::Line, 0);
      continue;
    }
    if (entry.pc >= code->instructions.size()) {
      add_verify_error(errors, "BC1309", "line entry pc is out of range",
                       SectionKind::Line, 0);
    }
  }

  for (const LocalDebugEntry &entry : module.local_debug) {
    const BcCode *code = code_by_id(module, code_ids, entry.code_id);
    if (code == nullptr) {
      add_verify_error(errors, "BC1204", "local debug code id is unknown",
                       SectionKind::Locs, 0);
      continue;
    }
    if (!contains_index(module.strings, entry.name_str_id)) {
      add_verify_error(errors, "BC1202", "local debug name ref is invalid",
                       SectionKind::Locs, 0);
    }
    if (entry.start_pc > entry.end_pc ||
        entry.end_pc > code->instructions.size()) {
      add_verify_error(errors, "BC1305", "local debug range is invalid",
                       SectionKind::Locs, 0);
    }
  }

  for (const AttrEntry &entry : module.attrs) {
    if (!contains_index(module.strings, entry.key_str_id) ||
        !contains_index(module.strings, entry.value_str_id)) {
      add_verify_error(errors, "BC1202", "attribute string ref is invalid",
                       SectionKind::Attr, 0);
    }
  }

  auto verify_feature_vector = [&](const std::vector<std::string> &features,
                                   const char *label) {
    std::vector<std::string> seen;
    for (const std::string &feature : features) {
      if (feature.empty()) {
        add_verify_error(errors, "BC1414",
                         std::string(label) + " profile feature is empty",
                         SectionKind::Prof, 0);
      }
      if (std::find(seen.begin(), seen.end(), feature) != seen.end()) {
        add_verify_error(errors, "BC1414",
                         std::string(label) + " profile feature is duplicate",
                         SectionKind::Prof, 0);
      }
      seen.push_back(feature);
    }
  };
  verify_feature_vector(module.required_features, "required");
  verify_feature_vector(module.optional_features, "optional");
  verify_feature_vector(module.forbidden_features, "forbidden");
  for (const std::string &feature : module.required_features) {
    if (std::find(module.forbidden_features.begin(),
                  module.forbidden_features.end(),
                  feature) != module.forbidden_features.end()) {
      add_verify_error(errors, "BC1414",
                       "profile feature is both required and forbidden",
                       SectionKind::Prof, 0);
    }
  }

  for (const capability::CapabilityRequest &entry : module.capabilities) {
    if (!capability::valid_capability_name(entry.name)) {
      add_verify_error(errors, "BC1401", "invalid capability name",
                       SectionKind::Caps, 0);
    }
  }

  const effect::EffectValidationResult effect_validation =
      effect::validate_effect_summaries(module.effects);
  for (const effect::EffectDiagnostic &diagnostic :
       effect_validation.diagnostics) {
    add_verify_error(errors, "BC1402", diagnostic.message, SectionKind::Efct,
                     0);
  }
  for (const effect::EffectSummary &entry : module.effects) {
    if (entry.owner.empty() || entry.kind.empty()) {
      add_verify_error(errors, "BC1403", "effect summary owner/kind is empty",
                       SectionKind::Efct, 0);
    }
    for (const std::string &label : entry.declared_effects) {
      if (!effect::valid_effect_name(label)) {
        add_verify_error(errors, "BC1404", "invalid declared effect label",
                         SectionKind::Efct, 0);
      }
    }
    for (const std::string &label : entry.observed_effects) {
      if (!effect::valid_effect_name(label)) {
        add_verify_error(errors, "BC1404", "invalid observed effect label",
                         SectionKind::Efct, 0);
      }
    }
  }

  const replay::ReplayValidationResult replay_validation =
      replay::validate_metadata(module.replay_metadata,
                                module.observability_sites);
  for (const replay::ReplayDiagnostic &diagnostic :
       replay_validation.diagnostics) {
    add_verify_error(errors, "BC1405", diagnostic.message, SectionKind::Rply,
                     0);
  }

  const data::SchemaValidationResult schema_validation =
      data::validate_schemas(module.schemas, module.schema_migrations);
  for (const data::DataDiagnostic &diagnostic : schema_validation.diagnostics) {
    add_verify_error(errors, "BC1406", diagnostic.message, SectionKind::Scma,
                     0);
  }

  const data::TablePlanValidationResult table_validation =
      data::validate_table_plans(module.table_plans);
  for (const data::DataDiagnostic &diagnostic : table_validation.diagnostics) {
    add_verify_error(errors, "BC1407", diagnostic.message, SectionKind::Tabl,
                     0);
  }

  const wasm_accel::WasmComponentValidationResult wasm_validation =
      wasm_accel::validate_wasm_components(module.wasm_components);
  for (const wasm_accel::WasmAccelDiagnostic &diagnostic :
       wasm_validation.diagnostics) {
    add_verify_error(errors, "BC1408", diagnostic.message, SectionKind::Wasm,
                     0);
  }

  const wasm_accel::AcceleratorValidationResult accelerator_validation =
      wasm_accel::validate_accelerator_kernels(module.accelerator_kernels);
  for (const wasm_accel::WasmAccelDiagnostic &diagnostic :
       accelerator_validation.diagnostics) {
    add_verify_error(errors, "BC1409", diagnostic.message, SectionKind::Accl,
                     0);
  }

  const modern::AgentValidationResult agent_validation =
      modern::validate_agent_metadata(module.agent_symbols,
                                      module.agent_patches,
                                      module.provenance_records);
  for (const modern::ModernDiagnostic &diagnostic :
       agent_validation.diagnostics) {
    add_verify_error(errors, "BC1410", diagnostic.message, SectionKind::Agnt,
                     0);
  }

  const modern::ContractValidationResult contract_validation =
      modern::validate_contract_metadata(module.contracts, module.properties);
  for (const modern::ModernDiagnostic &diagnostic :
       contract_validation.diagnostics) {
    add_verify_error(errors, "BC1411", diagnostic.message, SectionKind::Cntr,
                     0);
  }

  const modern::PrivacyValidationResult privacy_validation =
      modern::validate_privacy_metadata(
          module.privacy_labels, module.privacy_policies, module.lineage_nodes);
  for (const modern::ModernDiagnostic &diagnostic :
       privacy_validation.diagnostics) {
    add_verify_error(errors, "BC1412", diagnostic.message, SectionKind::Priv,
                     0);
  }

  const modern::WorkflowValidationResult workflow_validation =
      modern::validate_workflow_metadata(module.workflow_steps,
                                         module.workflow_history);
  for (const modern::ModernDiagnostic &diagnostic :
       workflow_validation.diagnostics) {
    add_verify_error(errors, "BC1413", diagnostic.message, SectionKind::Wflw,
                     0);
  }

  for (const HashEntry &entry : module.hashes) {
    if (entry.digest.size() != 32U) {
      add_verify_error(errors, "BC1306", "hash digest must be 32 bytes",
                       SectionKind::Hash, 0);
    }
  }
}

std::string string_or_placeholder(const std::vector<std::string> &pool,
                                  std::uint32_t id) {
  if (id >= pool.size()) {
    return "<invalid>";
  }
  return pool[id];
}

bool constant_is_path_ref(const BcModule &module, std::uint32_t ref_id) {
  return ref_id < module.const_pool.size() &&
         module.const_pool[ref_id].kind == ConstantKind::Path;
}

std::string path_constant_text(const BcModule &module, std::uint32_t ref_id) {
  if (!constant_is_path_ref(module, ref_id)) {
    return "<invalid>";
  }
  const Constant &constant = module.const_pool[ref_id];
  std::ostringstream out;
  for (std::size_t i = 0; i < constant.items.size(); ++i) {
    if (i != 0U) {
      out << ".";
    }
    out << string_or_placeholder(module.symbols, constant.items[i]);
  }
  return out.str();
}

void emit_u32_json_array(std::ostringstream &out,
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

void emit_path_ref_list(std::ostringstream &out, const BcModule &module,
                        const std::vector<std::uint32_t> &refs) {
  out << "[";
  for (std::size_t i = 0; i < refs.size(); ++i) {
    if (i != 0U) {
      out << ", ";
    }
    out << "k" << refs[i] << "("
        << json_escape(path_constant_text(module, refs[i])) << ")";
  }
  out << "]";
}

void emit_string_array(std::ostringstream &out, const char *name,
                       const std::vector<std::string> &items) {
  out << "  \"" << name << "\": [";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\"" << json_escape(items[i]) << "\"";
  }
  out << "]";
}

std::string span_to_json(const lexer::Span &span) {
  std::ostringstream out;
  out << "{\"file\":\"" << json_escape(span.file)
      << "\",\"start\":{\"line\":" << span.start.line
      << ",\"col\":" << span.start.col << ",\"offset\":" << span.start.offset
      << "},\"end\":{\"line\":" << span.end.line << ",\"col\":" << span.end.col
      << ",\"offset\":" << span.end.offset << "}}";
  return out.str();
}

std::string constant_to_json(const Constant &constant) {
  std::ostringstream out;
  out << "{\"kind\":\"" << json_escape(constant_kind_name(constant.kind))
      << "\"";
  switch (constant.kind) {
  case ConstantKind::Null:
    break;
  case ConstantKind::Bool:
    out << ",\"bool_value\":" << (constant.bool_value ? "true" : "false");
    break;
  case ConstantKind::Integer:
    out << ",\"int_value\":" << constant.int_value;
    break;
  case ConstantKind::Float:
    out << ",\"float_value\":" << std::setprecision(17) << constant.float_value;
    break;
  case ConstantKind::SymbolRef:
  case ConstantKind::StringRef:
  case ConstantKind::CodeRef:
    out << ",\"ref_id\":" << constant.ref_id;
    break;
  case ConstantKind::KeySet:
  case ConstantKind::Path:
    out << ",\"items\":[";
    for (std::size_t i = 0; i < constant.items.size(); ++i) {
      if (i != 0U) {
        out << ",";
      }
      out << constant.items[i];
    }
    out << "]";
    break;
  }
  out << "}";
  return out.str();
}

std::string operand_to_text(const InstructionOperand &operand) {
  std::ostringstream out;
  if (operand.signed_immediate) {
    out << operand.value;
  } else {
    out << static_cast<std::uint64_t>(operand.value);
  }
  return out.str();
}

} // namespace

std::vector<std::uint8_t> serialize_module(const BcModule &module) {
  constexpr std::size_t kHeaderSize = 56U;
  constexpr std::size_t kSectionEntrySize = 24U;

  const std::vector<SectionPayload> payloads = build_sections(module);
  std::vector<SectionEntry> directory;
  directory.reserve(payloads.size());

  std::uint64_t cursor = static_cast<std::uint64_t>(
      kHeaderSize + payloads.size() * kSectionEntrySize);
  for (const SectionPayload &payload : payloads) {
    cursor = align_up(cursor, payload.align);
    directory.push_back({payload.kind, cursor,
                         static_cast<std::uint32_t>(payload.bytes.size()),
                         payload.align, payload.flags});
    cursor += payload.bytes.size();
  }

  std::vector<std::uint8_t> out;
  out.reserve(static_cast<std::size_t>(cursor));
  out.insert(out.end(), {'A', 'B', 'M', '1'});
  append_u16(out, module.format_version.major);
  append_u16(out, module.format_version.minor);
  append_u16(out, module.language_version.major);
  append_u16(out, module.language_version.minor);
  append_u32(out, module.profile_flags);
  append_u32(out, static_cast<std::uint32_t>(directory.size()));
  append_u32(out, module.file_flags);
  out.insert(out.end(), module.abi_hash.begin(), module.abi_hash.end());

  for (const SectionEntry &entry : directory) {
    const char *tag = section_tag(entry.kind);
    out.insert(out.end(), tag, tag + 4);
    append_u64(out, entry.offset);
    append_u32(out, entry.size);
    append_u32(out, entry.align);
    append_u32(out, entry.flags);
  }

  for (std::size_t i = 0; i < payloads.size(); ++i) {
    while (out.size() < directory[i].offset) {
      out.push_back(0U);
    }
    append_bytes(out, payloads[i].bytes);
  }
  return out;
}

DecodeResult deserialize_module(const std::vector<std::uint8_t> &bytes) {
  constexpr std::size_t kHeaderSize = 56U;
  constexpr std::size_t kSectionEntrySize = 24U;

  DecodeResult result;
  if (bytes.size() < kHeaderSize) {
    result.errors.push_back(
        {"BC1002", "file is smaller than AmberBcHeader", "header", 0});
    return result;
  }

  Reader reader{bytes, 0, bytes.size(), 0, "header", &result.errors};
  std::array<char, 4> magic{};
  for (char &ch : magic) {
    std::uint8_t byte = 0;
    if (!reader.read_u8(byte)) {
      return result;
    }
    ch = static_cast<char>(byte);
  }
  if (std::string(magic.begin(), magic.end()) != "ABM1") {
    result.errors.push_back(
        {"BC1001", "invalid magic; expected ABM1", "header", 0});
    return result;
  }

  std::uint32_t section_count = 0;
  if (!reader.read_u16(result.module.format_version.major) ||
      !reader.read_u16(result.module.format_version.minor) ||
      !reader.read_u16(result.module.language_version.major) ||
      !reader.read_u16(result.module.language_version.minor) ||
      !reader.read_u32(result.module.profile_flags) ||
      !reader.read_u32(section_count) ||
      !reader.read_u32(result.module.file_flags)) {
    return result;
  }
  std::vector<std::uint8_t> abi;
  if (!reader.read_bytes(32, abi)) {
    return result;
  }
  std::copy(abi.begin(), abi.end(), result.module.abi_hash.begin());

  const std::uint64_t directory_bytes =
      static_cast<std::uint64_t>(section_count) * kSectionEntrySize;
  if (bytes.size() < kHeaderSize + directory_bytes) {
    result.errors.push_back(
        {"BC1002", "section directory is truncated", "header", kHeaderSize});
    return result;
  }

  std::map<SectionKind, SectionEntry> by_kind;
  for (std::uint32_t i = 0; i < section_count; ++i) {
    std::array<char, 4> raw_tag{};
    for (char &ch : raw_tag) {
      std::uint8_t byte = 0;
      if (!reader.read_u8(byte)) {
        return result;
      }
      ch = static_cast<char>(byte);
    }
    SectionKind kind = SectionKind::Hash;
    if (!decode_section_kind(raw_tag, kind)) {
      result.errors.push_back({"BC1105", "unknown section kind in directory",
                               "header", reader.pos - 4U});
      std::uint64_t discard_offset = 0;
      std::uint32_t discard_size = 0;
      std::uint32_t discard_align = 0;
      std::uint32_t discard_flags = 0;
      if (!reader.read_u64(discard_offset) || !reader.read_u32(discard_size) ||
          !reader.read_u32(discard_align) || !reader.read_u32(discard_flags)) {
        return result;
      }
      continue;
    }
    SectionEntry entry;
    entry.kind = kind;
    if (!reader.read_u64(entry.offset) || !reader.read_u32(entry.size) ||
        !reader.read_u32(entry.align) || !reader.read_u32(entry.flags)) {
      return result;
    }
    if (entry.align == 0U) {
      result.errors.push_back({"BC1104", "section alignment must be non-zero",
                               section_kind_name(kind), reader.pos});
    } else if ((entry.offset % entry.align) != 0U) {
      result.errors.push_back({"BC1104", "section offset is not aligned",
                               section_kind_name(kind), entry.offset});
    }
    if (entry.offset + entry.size > bytes.size()) {
      result.errors.push_back({"BC1103", "section exceeds file bounds",
                               section_kind_name(kind), entry.offset});
      continue;
    }
    const auto [_, inserted] = by_kind.emplace(kind, entry);
    if (!inserted) {
      result.errors.push_back({"BC1101", "duplicate section in directory",
                               section_kind_name(kind), entry.offset});
      continue;
    }
    result.sections.push_back(entry);
  }

  for (SectionKind kind : required_sections()) {
    if (by_kind.find(kind) == by_kind.end()) {
      result.errors.push_back(
          {"BC1102", "missing required section", section_kind_name(kind), 0});
    }
  }
  if (!result.errors.empty()) {
    return result;
  }

  auto parse_section = [&](SectionKind kind, const char *section_name,
                           auto &&fn) {
    const SectionEntry &entry = by_kind.at(kind);
    Reader section_reader{bytes,
                          static_cast<std::size_t>(entry.offset),
                          static_cast<std::size_t>(entry.offset + entry.size),
                          entry.offset,
                          section_name,
                          &result.errors};
    fn(section_reader);
    if (result.errors.empty() && section_reader.pos != section_reader.end) {
      result.errors.push_back(
          {"BC1005", "section has trailing undecoded bytes", section_name,
           section_reader.base_offset + section_reader.pos});
    }
  };

  parse_section(SectionKind::Strs, "STRS", [&](Reader &r) {
    parse_string_pool(r, result.module.strings);
  });
  parse_section(SectionKind::Syms, "SYMS", [&](Reader &r) {
    parse_string_pool(r, result.module.symbols);
  });
  parse_section(SectionKind::Kons, "KONS", [&](Reader &r) {
    parse_constants(r, result.module.const_pool);
  });
  parse_section(SectionKind::Code, "CODE",
                [&](Reader &r) { parse_code(r, result.module.code_objects); });
  parse_section(SectionKind::Meth, "METH",
                [&](Reader &r) { parse_methods(r, result.module.methods); });
  parse_section(SectionKind::Clas, "CLAS",
                [&](Reader &r) { parse_classes(r, result.module.classes); });
  parse_section(SectionKind::Deps, "DEPS", [&](Reader &r) {
    parse_dependencies(r, result.module.dependencies);
  });
  parse_section(SectionKind::Expt, "EXPT",
                [&](Reader &r) { parse_exports(r, result.module.exports); });
  parse_section(SectionKind::Init, "INIT",
                [&](Reader &r) { parse_init(r, result.module.init); });

  if (by_kind.find(SectionKind::Pats) != by_kind.end()) {
    parse_section(SectionKind::Pats, "PATS", [&](Reader &r) {
      parse_pattern_programs(r, result.module.pattern_programs);
    });
  }
  if (by_kind.find(SectionKind::Span) != by_kind.end()) {
    parse_section(SectionKind::Span, "SPAN", [&](Reader &r) {
      parse_spans(r, result.module.code_objects);
    });
  }
  if (by_kind.find(SectionKind::Line) != by_kind.end()) {
    parse_section(SectionKind::Line, "LINE", [&](Reader &r) {
      parse_line_table(r, result.module.line_table);
    });
  }
  if (by_kind.find(SectionKind::Locs) != by_kind.end()) {
    parse_section(SectionKind::Locs, "LOCS", [&](Reader &r) {
      parse_local_debug(r, result.module.local_debug);
    });
  }
  if (by_kind.find(SectionKind::Attr) != by_kind.end()) {
    parse_section(SectionKind::Attr, "ATTR",
                  [&](Reader &r) { parse_attrs(r, result.module.attrs); });
  }
  if (by_kind.find(SectionKind::Prof) != by_kind.end()) {
    parse_section(SectionKind::Prof, "PROF",
                  [&](Reader &r) { parse_profile_metadata(r, result.module); });
  }
  if (by_kind.find(SectionKind::Caps) != by_kind.end()) {
    parse_section(SectionKind::Caps, "CAPS", [&](Reader &r) {
      parse_capabilities(r, result.module.capabilities);
    });
  }
  if (by_kind.find(SectionKind::Efct) != by_kind.end()) {
    parse_section(SectionKind::Efct, "EFCT",
                  [&](Reader &r) { parse_effects(r, result.module.effects); });
  }
  if (by_kind.find(SectionKind::Obsv) != by_kind.end()) {
    parse_section(SectionKind::Obsv, "OBSV", [&](Reader &r) {
      parse_observability_sites(r, result.module.observability_sites);
    });
  }
  if (by_kind.find(SectionKind::Rply) != by_kind.end()) {
    parse_section(SectionKind::Rply, "RPLY", [&](Reader &r) {
      parse_replay_metadata(r, result.module.replay_metadata);
    });
  }
  if (by_kind.find(SectionKind::Scma) != by_kind.end()) {
    parse_section(SectionKind::Scma, "SCMA", [&](Reader &r) {
      parse_schema_metadata(r, result.module.schemas,
                            result.module.schema_migrations);
    });
  }
  if (by_kind.find(SectionKind::Tabl) != by_kind.end()) {
    parse_section(SectionKind::Tabl, "TABL", [&](Reader &r) {
      parse_table_plans(r, result.module.table_plans);
    });
  }
  if (by_kind.find(SectionKind::Wasm) != by_kind.end()) {
    parse_section(SectionKind::Wasm, "WASM", [&](Reader &r) {
      parse_wasm_components(r, result.module.wasm_components);
    });
  }
  if (by_kind.find(SectionKind::Accl) != by_kind.end()) {
    parse_section(SectionKind::Accl, "ACCL", [&](Reader &r) {
      parse_accelerator_kernels(r, result.module.accelerator_kernels);
    });
  }
  if (by_kind.find(SectionKind::Agnt) != by_kind.end()) {
    parse_section(SectionKind::Agnt, "AGNT", [&](Reader &r) {
      parse_agent_metadata(r, result.module.agent_symbols,
                           result.module.agent_patches,
                           result.module.provenance_records);
    });
  }
  if (by_kind.find(SectionKind::Cntr) != by_kind.end()) {
    parse_section(SectionKind::Cntr, "CNTR", [&](Reader &r) {
      parse_contract_metadata(r, result.module.contracts,
                              result.module.properties);
    });
  }
  if (by_kind.find(SectionKind::Priv) != by_kind.end()) {
    parse_section(SectionKind::Priv, "PRIV", [&](Reader &r) {
      parse_privacy_metadata(r, result.module.privacy_labels,
                             result.module.privacy_policies,
                             result.module.lineage_nodes);
    });
  }
  if (by_kind.find(SectionKind::Wflw) != by_kind.end()) {
    parse_section(SectionKind::Wflw, "WFLW", [&](Reader &r) {
      parse_workflow_metadata(r, result.module.workflow_steps,
                              result.module.workflow_history);
    });
  }
  if (by_kind.find(SectionKind::Hash) != by_kind.end()) {
    parse_section(SectionKind::Hash, "HASH",
                  [&](Reader &r) { parse_hashes(r, result.module.hashes); });
  }

  if (!result.errors.empty()) {
    return result;
  }

  verify_module(result.module, result.errors);
  return result;
}

std::string section_kind_name(SectionKind kind) { return section_tag(kind); }

std::string constant_kind_name(ConstantKind kind) {
  switch (kind) {
  case ConstantKind::Null:
    return "null";
  case ConstantKind::Bool:
    return "bool";
  case ConstantKind::Integer:
    return "integer";
  case ConstantKind::Float:
    return "float";
  case ConstantKind::SymbolRef:
    return "symbol_ref";
  case ConstantKind::StringRef:
    return "string_ref";
  case ConstantKind::CodeRef:
    return "code_ref";
  case ConstantKind::KeySet:
    return "key_set";
  case ConstantKind::Path:
    return "path";
  }
  return "unknown";
}

std::string opcode_name(Opcode opcode) {
  switch (opcode) {
  case Opcode::LoadK:
    return "LOADK";
  case Opcode::LoadNull:
    return "LOADNULL";
  case Opcode::LoadBool:
    return "LOADBOOL";
  case Opcode::Move:
    return "MOVE";
  case Opcode::LoadSelf:
    return "LOADSELF";
  case Opcode::GetLast:
    return "GETLAST";
  case Opcode::SetLast:
    return "SETLAST";
  case Opcode::MakeList:
    return "MAKE_LIST";
  case Opcode::MakeTuple:
    return "MAKE_TUPLE";
  case Opcode::MakeMap:
    return "MAKE_MAP";
  case Opcode::MakeSet:
    return "MAKE_SET";
  case Opcode::MakeMapDyn:
    return "MAKE_MAP_DYN";
  case Opcode::Freeze:
    return "FREEZE";
  case Opcode::LoadUpval:
    return "LOAD_UPVAL";
  case Opcode::StoreUpval:
    return "STORE_UPVAL";
  case Opcode::LoadIvar:
    return "LOAD_IVAR";
  case Opcode::StoreIvar:
    return "STORE_IVAR";
  case Opcode::LoadCvar:
    return "LOAD_CVAR";
  case Opcode::StoreCvar:
    return "STORE_CVAR";
  case Opcode::LookupConst:
    return "LOOKUP_CONST";
  case Opcode::MakeClosure:
    return "MAKE_CLOSURE";
  case Opcode::ObjDestroy:
    return "OBJ_DESTROY";
  case Opcode::ObjDealloc:
    return "OBJ_DEALLOC";
  case Opcode::CloseUpvalues:
    return "CLOSE_UPVALUES";
  case Opcode::WatchLocal:
    return "WATCH_LOCAL";
  case Opcode::WatchUpval:
    return "WATCH_UPVAL";
  case Opcode::WatchIvar:
    return "WATCH_IVAR";
  case Opcode::Send:
    return "SEND";
  case Opcode::SendDyn:
    return "SEND_DYN";
  case Opcode::Call:
    return "CALL";
  case Opcode::InOp:
    return "IN_OP";
  case Opcode::TripleEq:
    return "TRIPLE_EQ";
  case Opcode::TypeCheck:
    return "TYPECHECK";
  case Opcode::IAdd:
    return "IADD";
  case Opcode::ISub:
    return "ISUB";
  case Opcode::ILt:
    return "ILT";
  case Opcode::IGt:
    return "IGT";
  case Opcode::IAddK:
    return "IADDK";
  case Opcode::ISubK:
    return "ISUBK";
  case Opcode::ILtK:
    return "ILTK";
  case Opcode::IGtK:
    return "IGTK";
  case Opcode::IMul:
    return "IMUL";
  case Opcode::IDiv:
    return "IDIV";
  case Opcode::IMod:
    return "IMOD";
  case Opcode::IFloorDiv:
    return "IFLOORDIV";
  case Opcode::ILe:
    return "ILE";
  case Opcode::IGe:
    return "IGE";
  case Opcode::IEq:
    return "IEQ";
  case Opcode::INe:
    return "INE";
  case Opcode::ICmp:
    return "ICMP";
  case Opcode::IMulK:
    return "IMULK";
  case Opcode::IDivK:
    return "IDIVK";
  case Opcode::IModK:
    return "IMODK";
  case Opcode::IFloorDivK:
    return "IFLOORDIVK";
  case Opcode::ILeK:
    return "ILEK";
  case Opcode::IGeK:
    return "IGEK";
  case Opcode::IEqK:
    return "IEQK";
  case Opcode::INeK:
    return "INEK";
  case Opcode::ICmpK:
    return "ICMPK";
  case Opcode::Jump:
    return "JUMP";
  case Opcode::JumpIfTrue:
    return "JUMP_IF_TRUE";
  case Opcode::JumpIfFalse:
    return "JUMP_IF_FALSE";
  case Opcode::JumpIfNull:
    return "JUMP_IF_NULL";
  case Opcode::Return:
    return "RETURN";
  case Opcode::Raise:
    return "RAISE";
  case Opcode::Safepoint:
    return "SAFEPOINT";
  case Opcode::PPrepSeq:
    return "P_PREP_SEQ";
  case Opcode::PPrepMap:
    return "P_PREP_MAP";
  case Opcode::PCheckEq:
    return "P_CHECK_EQ";
  case Opcode::PCheckPin:
    return "P_CHECK_PIN";
  case Opcode::PCheckLenEq:
    return "P_CHECK_LEN_EQ";
  case Opcode::PCheckLenGte:
    return "P_CHECK_LEN_GTE";
  case Opcode::PGetIndex:
    return "P_GET_INDEX";
  case Opcode::PHasKey:
    return "P_HAS_KEY";
  case Opcode::PGetKey:
    return "P_GET_KEY";
  case Opcode::PTripleEq:
    return "P_TRIPLE_EQ";
  case Opcode::PBind:
    return "P_BIND";
  case Opcode::PCommit:
    return "P_COMMIT";
  case Opcode::PFail:
    return "P_FAIL";
  }
  return "UNKNOWN";
}

std::string code_kind_name(CodeKind kind) {
  switch (kind) {
  case CodeKind::Module:
    return "module";
  case CodeKind::Method:
    return "method";
  case CodeKind::Block:
    return "block";
  case CodeKind::Ensure:
    return "ensure";
  case CodeKind::Rescue:
    return "rescue";
  case CodeKind::DefaultThunk:
    return "default_thunk";
  }
  return "unknown";
}

std::string module_to_json(const BcModule &module,
                           const std::vector<SectionEntry> &sections,
                           const std::string &source_hash) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.bc.v1\",\n";
  out << "  \"source_hash\": \"" << json_escape(source_hash) << "\",\n";
  out << "  \"format_version\": {\"major\": " << module.format_version.major
      << ", \"minor\": " << module.format_version.minor << "},\n";
  out << "  \"language_version\": {\"major\": " << module.language_version.major
      << ", \"minor\": " << module.language_version.minor << "},\n";
  out << "  \"profile_flags\": " << module.profile_flags << ",\n";
  out << "  \"file_flags\": " << module.file_flags << ",\n";
  out << "  \"abi_hash\": \"" << bytes_to_hex(module.abi_hash) << "\",\n";
  out << "  \"sections\": [\n";
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const SectionEntry &entry = sections[i];
    out << "    {\"kind\":\"" << json_escape(section_kind_name(entry.kind))
        << "\",\"offset\":" << entry.offset << ",\"size\":" << entry.size
        << ",\"align\":" << entry.align << ",\"flags\":" << entry.flags << "}";
    if (i + 1U != sections.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";
  emit_string_array(out, "strings", module.strings);
  out << ",\n";
  emit_string_array(out, "symbols", module.symbols);
  out << ",\n";
  out << "  \"constants\": [";
  for (std::size_t i = 0; i < module.const_pool.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << constant_to_json(module.const_pool[i]);
  }
  out << "],\n";
  out << "  \"code\": [\n";
  for (std::size_t i = 0; i < module.code_objects.size(); ++i) {
    const BcCode &code = module.code_objects[i];
    out << "    {\"code_id\":" << code.code_id << ",\"kind\":\""
        << json_escape(code_kind_name(code.kind))
        << "\",\"reg_count\":" << code.reg_count << ",\"flags\":" << code.flags
        << ",\"locals\":[";
    for (std::size_t j = 0; j < code.local_layout.size(); ++j) {
      const SlotLayoutEntry &entry = code.local_layout[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"slot\":" << entry.slot
          << ",\"name_str_id\":" << entry.name_str_id
          << ",\"role_str_id\":" << entry.role_str_id
          << ",\"binding_kind_str_id\":" << entry.binding_kind_str_id << "}";
    }
    out << "],\"captures\":[";
    for (std::size_t j = 0; j < code.capture_layout.size(); ++j) {
      const CaptureLayoutEntry &entry = code.capture_layout[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"slot\":" << entry.slot
          << ",\"name_str_id\":" << entry.name_str_id
          << ",\"source_kind_str_id\":" << entry.source_kind_str_id
          << ",\"source_name_str_id\":" << entry.source_name_str_id << "}";
    }
    out << "],\"instructions\":[";
    for (std::size_t j = 0; j < code.instructions.size(); ++j) {
      const Instruction &instruction = code.instructions[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"opcode\":\"" << json_escape(opcode_name(instruction.opcode))
          << "\",\"operands\":[";
      for (std::size_t k = 0; k < instruction.operands.size(); ++k) {
        const InstructionOperand &operand = instruction.operands[k];
        if (k != 0U) {
          out << ",";
        }
        out << "{\"value\":" << operand.value
            << ",\"signed\":" << (operand.signed_immediate ? "true" : "false")
            << "}";
      }
      out << "]}";
    }
    out << "],\"handlers\":[";
    for (std::size_t j = 0; j < code.handler_table.size(); ++j) {
      const HandlerEntry &entry = code.handler_table[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"protected_from\":" << entry.protected_from
          << ",\"protected_to\":" << entry.protected_to
          << ",\"handler_pc\":" << entry.handler_pc
          << ",\"handler_code_id\":" << entry.handler_code_id
          << ",\"flags\":" << entry.flags << "}";
    }
    out << "],\"call_sites\":[";
    for (std::size_t j = 0; j < code.call_site_table.size(); ++j) {
      const CacheSiteEntry &entry = code.call_site_table[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"pc\":" << entry.pc << ",\"slot\":" << entry.slot
          << ",\"symbol_id\":" << entry.symbol_id
          << ",\"flags\":" << entry.flags << "}";
    }
    out << "],\"ivar_sites\":[";
    for (std::size_t j = 0; j < code.ivar_site_table.size(); ++j) {
      const CacheSiteEntry &entry = code.ivar_site_table[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"pc\":" << entry.pc << ",\"slot\":" << entry.slot
          << ",\"symbol_id\":" << entry.symbol_id
          << ",\"flags\":" << entry.flags << "}";
    }
    out << "],\"safepoints\":[";
    for (std::size_t j = 0; j < code.safepoint_table.size(); ++j) {
      const SafepointEntry &entry = code.safepoint_table[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"pc\":" << entry.pc << ",\"flags\":" << entry.flags << "}";
    }
    out << "],\"source_spans\":[";
    for (std::size_t j = 0; j < code.source_spans.size(); ++j) {
      const SourceSpanEntry &entry = code.source_spans[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"pc_from\":" << entry.pc_from << ",\"pc_to\":" << entry.pc_to
          << ",\"span\":" << span_to_json(entry.span) << "}";
    }
    out << "]}";
    if (i + 1U != module.code_objects.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"methods\": [";
  for (std::size_t i = 0; i < module.methods.size(); ++i) {
    const BcMethod &method = module.methods[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"selector_sym_id\":" << method.selector_sym_id
        << ",\"owner_dispatch_ref\":" << method.owner_dispatch_ref
        << ",\"signature_blob_id\":" << method.signature_blob_id
        << ",\"params\":[";
    for (std::size_t j = 0; j < method.params.size(); ++j) {
      const MethodParamEntry &entry = method.params[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"external_name_sym_id\":" << entry.external_name_sym_id
          << ",\"local_name_str_id\":" << entry.local_name_str_id
          << ",\"flags\":" << entry.flags << "}";
    }
    out << "],\"default_thunk_ids\":[";
    for (std::size_t j = 0; j < method.default_thunk_ids.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      out << method.default_thunk_ids[j];
    }
    out << "],\"type_hook_ids\":[";
    for (std::size_t j = 0; j < method.type_hook_ids.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      out << method.type_hook_ids[j];
    }
    out << "],\"clauses\":[";
    for (std::size_t j = 0; j < method.clause_table.size(); ++j) {
      const ClauseEntry &entry = method.clause_table[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"pattern_program_id\":" << entry.pattern_program_id
          << ",\"pattern_code_id\":" << entry.pattern_code_id
          << ",\"guard_code_id\":" << entry.guard_code_id
          << ",\"body_code_id\":" << entry.body_code_id
          << ",\"flags\":" << entry.flags << "}";
    }
    out << "],\"auto_assign\":[";
    for (std::size_t j = 0; j < method.auto_assign_desc.size(); ++j) {
      const AutoAssignEntry &entry = method.auto_assign_desc[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"local_name_str_id\":" << entry.local_name_str_id
          << ",\"target_name_str_id\":" << entry.target_name_str_id
          << ",\"flags\":" << entry.flags << "}";
    }
    out << "],\"entry_code_id\":" << method.entry_code_id
        << ",\"flags\":" << method.flags << "}";
  }
  out << "],\n";
  out << "  \"classes\": [";
  for (std::size_t i = 0; i < module.classes.size(); ++i) {
    const BcClass &klass = module.classes[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"class_name_sym_id\":" << klass.class_name_sym_id
        << ",\"has_superclass_ref\":"
        << (klass.has_superclass_ref ? "true" : "false")
        << ",\"superclass_ref\":" << klass.superclass_ref
        << ",\"ivar_schema_id\":" << klass.ivar_schema_id
        << ",\"method_range_start\":" << klass.method_range_start
        << ",\"method_range_count\":" << klass.method_range_count
        << ",\"direct_include_refs\":";
    emit_u32_json_array(out, klass.direct_include_refs);
    out << ",\"direct_extend_refs\":";
    emit_u32_json_array(out, klass.direct_extend_refs);
    out << ",\"flags\":" << klass.flags << ",\"has_class_init_code_id\":"
        << (klass.has_class_init_code_id ? "true" : "false")
        << ",\"class_init_code_id\":" << klass.class_init_code_id << "}";
  }
  out << "],\n";
  out << "  \"dependencies\": [";
  for (std::size_t i = 0; i < module.dependencies.size(); ++i) {
    const DepEntry &entry = module.dependencies[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"module_name_str_id\":" << entry.module_name_str_id
        << ",\"required_format\":{\"major\":" << entry.required_format.major
        << ",\"minor\":" << entry.required_format.minor
        << "},\"min_language_version\":{\"major\":"
        << entry.min_language_version.major
        << ",\"minor\":" << entry.min_language_version.minor
        << "},\"has_max_language_version\":"
        << (entry.has_max_language_version ? "true" : "false")
        << ",\"max_language_version\":{\"major\":"
        << entry.max_language_version.major
        << ",\"minor\":" << entry.max_language_version.minor
        << "},\"has_abi_requirement\":"
        << (entry.has_abi_requirement ? "true" : "false")
        << ",\"abi_requirement\":\"" << bytes_to_hex(entry.abi_requirement)
        << "\",\"flags\":" << entry.flags << "}";
  }
  out << "],\n";
  out << "  \"exports\": [";
  for (std::size_t i = 0; i < module.exports.size(); ++i) {
    const ExportEntry &entry = module.exports[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"symbol_id\":" << entry.symbol_id
        << ",\"target_kind_str_id\":" << entry.target_kind_str_id
        << ",\"target_index\":" << entry.target_index
        << ",\"visibility_flags\":" << entry.visibility_flags
        << ",\"has_reexport_module_name\":"
        << (entry.has_reexport_module_name ? "true" : "false")
        << ",\"reexport_module_name_str_id\":"
        << entry.reexport_module_name_str_id << "}";
  }
  out << "],\n";
  out << "  \"init\": {\"has_entry_code_id\":"
      << (module.init.has_entry_code_id ? "true" : "false")
      << ",\"entry_code_id\":" << module.init.entry_code_id
      << ",\"flags\":" << module.init.flags << "},\n";
  out << "  \"pattern_programs\": [";
  for (std::size_t i = 0; i < module.pattern_programs.size(); ++i) {
    const PatternProgramEntry &entry = module.pattern_programs[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"pattern_id\":" << entry.pattern_id
        << ",\"binding_count\":" << entry.binding_count
        << ",\"flags\":" << entry.flags << "}";
  }
  out << "],\n";
  out << "  \"line_table\": [";
  for (std::size_t i = 0; i < module.line_table.size(); ++i) {
    const LineEntry &entry = module.line_table[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"code_id\":" << entry.code_id << ",\"pc\":" << entry.pc
        << ",\"line\":" << entry.line << "}";
  }
  out << "],\n";
  out << "  \"local_debug\": [";
  for (std::size_t i = 0; i < module.local_debug.size(); ++i) {
    const LocalDebugEntry &entry = module.local_debug[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"code_id\":" << entry.code_id << ",\"slot\":" << entry.slot
        << ",\"name_str_id\":" << entry.name_str_id
        << ",\"start_pc\":" << entry.start_pc << ",\"end_pc\":" << entry.end_pc
        << "}";
  }
  out << "],\n";
  out << "  \"attrs\": [";
  for (std::size_t i = 0; i < module.attrs.size(); ++i) {
    const AttrEntry &entry = module.attrs[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"key_str_id\":" << entry.key_str_id
        << ",\"value_str_id\":" << entry.value_str_id << "}";
  }
  out << "],\n";
  emit_string_array(out, "required_features", module.required_features);
  out << ",\n";
  emit_string_array(out, "optional_features", module.optional_features);
  out << ",\n";
  emit_string_array(out, "forbidden_features", module.forbidden_features);
  out << ",\n";
  out << "  \"capabilities\": [";
  for (std::size_t i = 0; i < module.capabilities.size(); ++i) {
    const capability::CapabilityRequest &entry = module.capabilities[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"name\":\"" << json_escape(entry.name) << "\",\"target\":\""
        << json_escape(entry.target) << "\",\"reason\":\""
        << json_escape(entry.reason) << "\",\"flags\":" << entry.flags << "}";
  }
  out << "],\n";
  out << "  \"effects\": [";
  for (std::size_t i = 0; i < module.effects.size(); ++i) {
    const effect::EffectSummary &entry = module.effects[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"owner\":\"" << json_escape(entry.owner) << "\",\"kind\":\""
        << json_escape(entry.kind) << "\",\"declared\":\""
        << json_escape(effect::effect_row_to_text(entry.declared_effects))
        << "\",\"observed\":\""
        << json_escape(effect::effect_row_to_text(entry.observed_effects))
        << "\",\"flags\":" << entry.flags << "}";
  }
  out << "],\n";
  out << "  \"observability_sites\": [";
  for (std::size_t i = 0; i < module.observability_sites.size(); ++i) {
    const replay::ObservabilitySite &entry = module.observability_sites[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"site_id\":" << entry.site_id << ",\"event_name\":\""
        << json_escape(entry.event_name) << "\",\"kind\":\""
        << json_escape(entry.kind) << "\",\"owner\":\""
        << json_escape(entry.owner) << "\",\"source\":{\"file\":\""
        << json_escape(entry.source.file) << "\",\"line\":" << entry.source.line
        << ",\"column\":" << entry.source.column
        << "},\"flags\":" << entry.flags << "}";
  }
  out << "],\n";
  out << "  \"replay_metadata\": {\"required_events\":[";
  const replay::ReplayMetadata replay_metadata =
      replay::normalize_metadata(module.replay_metadata);
  for (std::size_t i = 0; i < replay_metadata.required_event_names.size();
       ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\"" << json_escape(replay_metadata.required_event_names[i]) << "\"";
  }
  out << "],\"deterministic_sources\":[";
  for (std::size_t i = 0; i < replay_metadata.deterministic_sources.size();
       ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\"" << json_escape(replay_metadata.deterministic_sources[i])
        << "\"";
  }
  out << "],\"flags\":" << replay_metadata.flags << "},\n";
  const data::SchemaValidationResult schema_metadata =
      data::validate_schemas(module.schemas, module.schema_migrations);
  out << "  \"schemas\": [";
  for (std::size_t i = 0; i < schema_metadata.schemas.size(); ++i) {
    const data::SchemaDefinition &schema = schema_metadata.schemas[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"name\":\"" << json_escape(schema.name)
        << "\",\"version\":" << schema.version << ",\"fields\":[";
    for (std::size_t j = 0; j < schema.fields.size(); ++j) {
      const data::SchemaField &field = schema.fields[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"name\":\"" << json_escape(field.name) << "\",\"type\":\""
          << json_escape(field.type)
          << "\",\"required\":" << (field.required ? "true" : "false")
          << ",\"nullable\":" << (field.nullable ? "true" : "false")
          << ",\"default\":\"" << json_escape(field.default_value)
          << "\",\"flags\":" << field.flags << "}";
    }
    out << "],\"flags\":" << schema.flags << "}";
  }
  out << "],\n";
  out << "  \"schema_migrations\": [";
  for (std::size_t i = 0; i < schema_metadata.migrations.size(); ++i) {
    const data::SchemaMigration &migration = schema_metadata.migrations[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"schema\":\"" << json_escape(migration.schema_name)
        << "\",\"from\":" << migration.from_version
        << ",\"to\":" << migration.to_version << ",\"kind\":\""
        << json_escape(migration.kind) << "\",\"flags\":" << migration.flags
        << "}";
  }
  out << "],\n";
  const data::TablePlanValidationResult table_metadata =
      data::validate_table_plans(module.table_plans);
  out << "  \"table_plans\": [";
  for (std::size_t i = 0; i < table_metadata.plans.size(); ++i) {
    const data::TablePlan &plan = table_metadata.plans[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"plan_id\":\"" << json_escape(plan.plan_id) << "\",\"op\":\""
        << json_escape(plan.op) << "\",\"inputs\":[";
    for (std::size_t j = 0; j < plan.input_refs.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      out << "\"" << json_escape(plan.input_refs[j]) << "\"";
    }
    out << "],\"arguments\":[";
    for (std::size_t j = 0; j < plan.arguments.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      out << "\"" << json_escape(plan.arguments[j]) << "\"";
    }
    out << "],\"column_dependencies\":[";
    for (std::size_t j = 0; j < plan.column_dependencies.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      out << "{\"table\":\""
          << json_escape(plan.column_dependencies[j].table_ref)
          << "\",\"column\":\""
          << json_escape(plan.column_dependencies[j].column_ref) << "\"}";
    }
    out << "],\"effect_row\":\""
        << json_escape(effect::effect_row_to_text(plan.effect_row))
        << "\",\"fingerprint\":\"" << data::table_plan_fingerprint(plan)
        << "\",\"flags\":" << plan.flags << "}";
  }
  out << "],\n";
  const wasm_accel::WasmComponentValidationResult wasm_metadata =
      wasm_accel::validate_wasm_components(module.wasm_components);
  out << "  \"wasm_components\": [";
  for (std::size_t i = 0; i < wasm_metadata.components.size(); ++i) {
    const wasm_accel::WasmComponent &component = wasm_metadata.components[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"name\":\"" << json_escape(component.name) << "\",\"world\":\""
        << json_escape(component.world) << "\",\"flags\":" << component.flags
        << ",\"imports\":[";
    for (std::size_t j = 0; j < component.imports.size(); ++j) {
      const wasm_accel::WasmInterfaceEntry &entry = component.imports[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"name\":\"" << json_escape(entry.name) << "\",\"kind\":\""
          << json_escape(entry.kind) << "\",\"type\":\""
          << json_escape(entry.type_signature) << "\",\"schema\":\""
          << json_escape(entry.schema_name) << "\",\"capability\":\""
          << json_escape(capability::request_to_text(entry.capability))
          << "\",\"effects\":\""
          << json_escape(effect::effect_row_to_text(entry.effect_row))
          << "\",\"flags\":" << entry.flags << "}";
    }
    out << "],\"exports\":[";
    for (std::size_t j = 0; j < component.exports.size(); ++j) {
      const wasm_accel::WasmInterfaceEntry &entry = component.exports[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"name\":\"" << json_escape(entry.name) << "\",\"kind\":\""
          << json_escape(entry.kind) << "\",\"type\":\""
          << json_escape(entry.type_signature) << "\",\"schema\":\""
          << json_escape(entry.schema_name) << "\",\"effects\":\""
          << json_escape(effect::effect_row_to_text(entry.effect_row))
          << "\",\"flags\":" << entry.flags << "}";
    }
    out << "]}";
  }
  out << "],\n";
  const wasm_accel::AcceleratorValidationResult accelerator_metadata =
      wasm_accel::validate_accelerator_kernels(module.accelerator_kernels);
  out << "  \"accelerator_kernels\": [";
  for (std::size_t i = 0; i < accelerator_metadata.kernels.size(); ++i) {
    const wasm_accel::AcceleratorKernel &kernel =
        accelerator_metadata.kernels[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"id\":\"" << json_escape(kernel.kernel_id) << "\",\"entry\":\""
        << json_escape(kernel.entry) << "\",\"target\":\""
        << json_escape(kernel.target) << "\",\"effects\":\""
        << json_escape(effect::effect_row_to_text(kernel.effect_row))
        << "\",\"flags\":" << kernel.flags << ",\"params\":[";
    for (std::size_t j = 0; j < kernel.params.size(); ++j) {
      const wasm_accel::AcceleratorValue &value = kernel.params[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"name\":\"" << json_escape(value.name) << "\",\"type\":\""
          << json_escape(value.type) << "\",\"space\":\""
          << json_escape(value.address_space) << "\",\"flags\":" << value.flags
          << "}";
    }
    out << "],\"captures\":[";
    for (std::size_t j = 0; j < kernel.captures.size(); ++j) {
      const wasm_accel::AcceleratorValue &value = kernel.captures[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"name\":\"" << json_escape(value.name) << "\",\"type\":\""
          << json_escape(value.type) << "\",\"space\":\""
          << json_escape(value.address_space) << "\",\"flags\":" << value.flags
          << "}";
    }
    out << "],\"forbidden_features\":[";
    for (std::size_t j = 0; j < kernel.forbidden_features.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      out << "\"" << json_escape(kernel.forbidden_features[j]) << "\"";
    }
    out << "]}";
  }
  out << "],\n";
  const modern::AgentValidationResult agent_metadata =
      modern::validate_agent_metadata(module.agent_symbols,
                                      module.agent_patches,
                                      module.provenance_records);
  out << "  \"agent_symbols\": [";
  for (std::size_t i = 0; i < agent_metadata.symbols.size(); ++i) {
    const modern::AgentSymbol &symbol = agent_metadata.symbols[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"id\":\"" << json_escape(symbol.symbol_id) << "\",\"name\":\""
        << json_escape(symbol.name) << "\",\"kind\":\""
        << json_escape(symbol.kind) << "\",\"module\":\""
        << json_escape(symbol.module) << "\",\"visibility\":\""
        << json_escape(symbol.visibility) << "\"}";
  }
  out << "],\n";
  out << "  \"agent_patches\": [";
  for (std::size_t i = 0; i < agent_metadata.patches.size(); ++i) {
    const modern::AgentPatch &patch = agent_metadata.patches[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"id\":\"" << json_escape(patch.patch_id) << "\",\"intent\":\""
        << json_escape(patch.intent)
        << "\",\"operations\":" << patch.operations.size()
        << ",\"flags\":" << patch.flags << "}";
  }
  out << "],\n";
  out << "  \"provenance_records\": [";
  for (std::size_t i = 0; i < agent_metadata.provenance.size(); ++i) {
    const modern::ProvenanceRecord &record = agent_metadata.provenance[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"patch_id\":\"" << json_escape(record.patch_id)
        << "\",\"tool\":\"" << json_escape(record.tool)
        << "\",\"checks\":" << record.checks_run.size() << "}";
  }
  out << "],\n";
  const modern::ContractValidationResult contract_metadata =
      modern::validate_contract_metadata(module.contracts, module.properties);
  out << "  \"contracts\": [";
  for (std::size_t i = 0; i < contract_metadata.contracts.size(); ++i) {
    const modern::ContractSpec &contract = contract_metadata.contracts[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"owner\":\"" << json_escape(contract.owner) << "\",\"kind\":\""
        << json_escape(contract.kind) << "\",\"expr\":\""
        << json_escape(contract.expression) << "\",\"effects\":\""
        << json_escape(effect::effect_row_to_text(contract.effect_row))
        << "\"}";
  }
  out << "],\n";
  out << "  \"properties\": [";
  for (std::size_t i = 0; i < contract_metadata.properties.size(); ++i) {
    const modern::PropertySpec &property = contract_metadata.properties[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"name\":\"" << json_escape(property.name)
        << "\",\"seed\":" << property.seed << ",\"generator\":\""
        << json_escape(property.generator) << "\"}";
  }
  out << "],\n";
  const modern::PrivacyValidationResult privacy_metadata =
      modern::validate_privacy_metadata(
          module.privacy_labels, module.privacy_policies, module.lineage_nodes);
  out << "  \"privacy_labels\": [";
  for (std::size_t i = 0; i < privacy_metadata.labels.size(); ++i) {
    const modern::PrivacyLabel &label = privacy_metadata.labels[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"name\":\"" << json_escape(label.name) << "\",\"kind\":\""
        << json_escape(label.kind) << "\",\"flags\":" << label.flags << "}";
  }
  out << "],\n";
  out << "  \"privacy_policies\": [";
  for (std::size_t i = 0; i < privacy_metadata.policies.size(); ++i) {
    const modern::PrivacyPolicyRule &rule = privacy_metadata.policies[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"policy\":\"" << json_escape(rule.policy) << "\",\"action\":\""
        << json_escape(rule.action) << "\",\"label\":\""
        << json_escape(rule.label) << "\"}";
  }
  out << "],\n";
  out << "  \"lineage_nodes\": [";
  for (std::size_t i = 0; i < privacy_metadata.lineage.size(); ++i) {
    const modern::LineageNode &node = privacy_metadata.lineage[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"id\":\"" << json_escape(node.node_id) << "\",\"kind\":\""
        << json_escape(node.kind) << "\",\"output\":\""
        << json_escape(node.output) << "\"}";
  }
  out << "],\n";
  const modern::WorkflowValidationResult workflow_metadata =
      modern::validate_workflow_metadata(module.workflow_steps,
                                         module.workflow_history);
  out << "  \"workflow_steps\": [";
  for (std::size_t i = 0; i < workflow_metadata.steps.size(); ++i) {
    const modern::WorkflowStep &step = workflow_metadata.steps[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"workflow\":\"" << json_escape(step.workflow) << "\",\"name\":\""
        << json_escape(step.name) << "\",\"effects\":\""
        << json_escape(effect::effect_row_to_text(step.effect_row)) << "\"}";
  }
  out << "],\n";
  out << "  \"workflow_history\": [";
  for (std::size_t i = 0; i < workflow_metadata.history.size(); ++i) {
    const modern::WorkflowHistoryEvent &event = workflow_metadata.history[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"workflow_id\":\"" << json_escape(event.workflow_id)
        << "\",\"step\":\"" << json_escape(event.step) << "\",\"event\":\""
        << json_escape(event.event) << "\"}";
  }
  out << "],\n";
  out << "  \"hashes\": [";
  for (std::size_t i = 0; i < module.hashes.size(); ++i) {
    const HashEntry &entry = module.hashes[i];
    if (i != 0U) {
      out << ",";
    }
    out << "{\"section\":\"" << json_escape(section_kind_name(entry.section))
        << "\",\"digest\":\"" << bytes_to_hex(entry.digest) << "\"}";
  }
  out << "]\n";
  out << "}\n";
  return out.str();
}

std::string module_to_disasm(const BcModule &module,
                             const std::vector<SectionEntry> &sections,
                             const std::string &source_hash) {
  std::ostringstream out;
  out << "; amber.bc.v1 sha256=" << source_hash << "\n";
  out << ".header format=" << module.format_version.major << "."
      << module.format_version.minor
      << " language=" << module.language_version.major << "."
      << module.language_version.minor
      << " profile_flags=" << module.profile_flags
      << " file_flags=" << module.file_flags
      << " abi=" << bytes_to_hex(module.abi_hash) << "\n";
  out << ".sections\n";
  for (const SectionEntry &entry : sections) {
    out << "  " << section_kind_name(entry.kind) << " offset=" << entry.offset
        << " size=" << entry.size << " align=" << entry.align
        << " flags=" << entry.flags << "\n";
  }
  out << ".strs\n";
  for (std::size_t i = 0; i < module.strings.size(); ++i) {
    out << "  s" << i << " \"" << json_escape(module.strings[i]) << "\"\n";
  }
  out << ".syms\n";
  for (std::size_t i = 0; i < module.symbols.size(); ++i) {
    out << "  y" << i << " :" << module.symbols[i] << "\n";
  }
  out << ".kons\n";
  for (std::size_t i = 0; i < module.const_pool.size(); ++i) {
    const Constant &constant = module.const_pool[i];
    out << "  k" << i << " " << constant_kind_name(constant.kind);
    switch (constant.kind) {
    case ConstantKind::Null:
      break;
    case ConstantKind::Bool:
      out << " " << (constant.bool_value ? "true" : "false");
      break;
    case ConstantKind::Integer:
      out << " " << constant.int_value;
      break;
    case ConstantKind::Float:
      out << " " << std::setprecision(17) << constant.float_value;
      break;
    case ConstantKind::SymbolRef:
    case ConstantKind::StringRef:
    case ConstantKind::CodeRef:
      out << " " << constant.ref_id;
      break;
    case ConstantKind::KeySet:
    case ConstantKind::Path:
      out << " [";
      for (std::size_t item = 0; item < constant.items.size(); ++item) {
        if (item != 0U) {
          out << ", ";
        }
        out << constant.items[item];
      }
      out << "]";
      break;
    }
    out << "\n";
  }
  out << ".code\n";
  for (const BcCode &code : module.code_objects) {
    out << "code c" << code.code_id << " kind=" << code_kind_name(code.kind)
        << " regs=" << code.reg_count << " flags=" << code.flags << "\n";
    for (const SlotLayoutEntry &entry : code.local_layout) {
      out << "  local l" << entry.slot << " name=s" << entry.name_str_id << "("
          << json_escape(
                 string_or_placeholder(module.strings, entry.name_str_id))
          << ") role=s" << entry.role_str_id << "("
          << json_escape(
                 string_or_placeholder(module.strings, entry.role_str_id))
          << ") binding=s" << entry.binding_kind_str_id << "("
          << json_escape(string_or_placeholder(module.strings,
                                               entry.binding_kind_str_id))
          << ")\n";
    }
    for (const CaptureLayoutEntry &entry : code.capture_layout) {
      out << "  capture u" << entry.slot << " name=s" << entry.name_str_id
          << "("
          << json_escape(
                 string_or_placeholder(module.strings, entry.name_str_id))
          << ") source_kind=s" << entry.source_kind_str_id << "("
          << json_escape(string_or_placeholder(module.strings,
                                               entry.source_kind_str_id))
          << ") source_name=s" << entry.source_name_str_id << "("
          << json_escape(string_or_placeholder(module.strings,
                                               entry.source_name_str_id))
          << ")\n";
    }
    for (std::size_t pc = 0; pc < code.instructions.size(); ++pc) {
      out << "  " << std::setw(4) << std::setfill('0') << pc << " "
          << opcode_name(code.instructions[pc].opcode);
      for (const InstructionOperand &operand : code.instructions[pc].operands) {
        out << " " << operand_to_text(operand);
      }
      out << std::setfill(' ') << "\n";
    }
    for (const HandlerEntry &entry : code.handler_table) {
      out << "  handler from=" << entry.protected_from
          << " to=" << entry.protected_to << " pc=" << entry.handler_pc
          << " code=c" << entry.handler_code_id << " flags=" << entry.flags
          << "\n";
    }
    for (const CacheSiteEntry &entry : code.call_site_table) {
      out << "  callsite pc=" << entry.pc << " slot=" << entry.slot << " sym=y"
          << entry.symbol_id << " flags=" << entry.flags << "\n";
    }
    for (const CacheSiteEntry &entry : code.ivar_site_table) {
      out << "  ivarsite pc=" << entry.pc << " slot=" << entry.slot << " sym=y"
          << entry.symbol_id << " flags=" << entry.flags << "\n";
    }
    for (const SafepointEntry &entry : code.safepoint_table) {
      out << "  safepoint pc=" << entry.pc << " flags=" << entry.flags << "\n";
    }
    for (const SourceSpanEntry &entry : code.source_spans) {
      out << "  span " << entry.pc_from << ".." << entry.pc_to << " "
          << entry.span.file << ":" << entry.span.start.line << ":"
          << entry.span.start.col << "-" << entry.span.end.line << ":"
          << entry.span.end.col << "\n";
    }
  }
  out << ".methods\n";
  for (std::size_t i = 0; i < module.methods.size(); ++i) {
    const BcMethod &method = module.methods[i];
    out << "  m" << i << " selector=y" << method.selector_sym_id << "("
        << json_escape(
               string_or_placeholder(module.symbols, method.selector_sym_id))
        << ") owner=" << method.owner_dispatch_ref << " signature=k"
        << method.signature_blob_id << " entry=c" << method.entry_code_id
        << " flags=" << method.flags << "\n";
    for (const MethodParamEntry &entry : method.params) {
      out << "    param external=y" << entry.external_name_sym_id << "("
          << json_escape(string_or_placeholder(module.symbols,
                                               entry.external_name_sym_id))
          << ") local=s" << entry.local_name_str_id << "("
          << json_escape(
                 string_or_placeholder(module.strings, entry.local_name_str_id))
          << ") flags=" << entry.flags << "\n";
    }
    for (std::uint32_t code_id : method.default_thunk_ids) {
      out << "    default_thunk c" << code_id << "\n";
    }
    for (std::uint32_t code_id : method.type_hook_ids) {
      out << "    type_hook c" << code_id << "\n";
    }
    for (const ClauseEntry &entry : method.clause_table) {
      out << "    clause pat=" << entry.pattern_program_id << " match=c"
          << entry.pattern_code_id << " guard=c" << entry.guard_code_id
          << " body=c" << entry.body_code_id << " flags=" << entry.flags
          << "\n";
    }
    for (const AutoAssignEntry &entry : method.auto_assign_desc) {
      out << "    auto_assign local=s" << entry.local_name_str_id << "("
          << json_escape(
                 string_or_placeholder(module.strings, entry.local_name_str_id))
          << ") target=s" << entry.target_name_str_id << "("
          << json_escape(string_or_placeholder(module.strings,
                                               entry.target_name_str_id))
          << ") flags=" << entry.flags << "\n";
    }
  }
  out << ".classes\n";
  for (std::size_t i = 0; i < module.classes.size(); ++i) {
    const BcClass &klass = module.classes[i];
    out << "  c" << i << " name=y" << klass.class_name_sym_id << "("
        << json_escape(
               string_or_placeholder(module.symbols, klass.class_name_sym_id))
        << ") super=";
    if (klass.has_superclass_ref) {
      out << "k" << klass.superclass_ref << "("
          << json_escape(path_constant_text(module, klass.superclass_ref))
          << ")";
    } else {
      out << "-";
    }
    out << " ivar_schema=k" << klass.ivar_schema_id
        << " methods=" << klass.method_range_start << "+"
        << klass.method_range_count << " flags=" << klass.flags;
    if ((klass.flags & kClassFlagMixin) != 0U) {
      out << " kind=mixin";
    }
    if (!klass.direct_include_refs.empty()) {
      out << " includes=";
      emit_path_ref_list(out, module, klass.direct_include_refs);
    }
    if (!klass.direct_extend_refs.empty()) {
      out << " extends=";
      emit_path_ref_list(out, module, klass.direct_extend_refs);
    }
    if (klass.has_class_init_code_id) {
      out << " class_init=c" << klass.class_init_code_id;
    }
    out << "\n";
  }
  out << ".deps\n";
  for (const DepEntry &entry : module.dependencies) {
    out << "  dep s" << entry.module_name_str_id << "("
        << json_escape(
               string_or_placeholder(module.strings, entry.module_name_str_id))
        << ") format=" << entry.required_format.major << "."
        << entry.required_format.minor
        << " language>=" << entry.min_language_version.major << "."
        << entry.min_language_version.minor;
    if (entry.has_max_language_version) {
      out << " language<=" << entry.max_language_version.major << "."
          << entry.max_language_version.minor;
    }
    if (entry.has_abi_requirement) {
      out << " abi=" << bytes_to_hex(entry.abi_requirement);
    }
    out << " flags=" << entry.flags << "\n";
  }
  out << ".exports\n";
  for (const ExportEntry &entry : module.exports) {
    out << "  export y" << entry.symbol_id << "("
        << json_escape(string_or_placeholder(module.symbols, entry.symbol_id))
        << ") kind=s" << entry.target_kind_str_id << "("
        << json_escape(
               string_or_placeholder(module.strings, entry.target_kind_str_id))
        << ") target=" << entry.target_index
        << " visibility=" << entry.visibility_flags;
    if (entry.has_reexport_module_name) {
      out << " reexport=s" << entry.reexport_module_name_str_id << "("
          << json_escape(string_or_placeholder(
                 module.strings, entry.reexport_module_name_str_id))
          << ")";
    }
    out << "\n";
  }
  out << ".init\n";
  if (module.init.has_entry_code_id) {
    out << "  entry c" << module.init.entry_code_id
        << " flags=" << module.init.flags << "\n";
  } else {
    out << "  <none>\n";
  }
  if (!module.pattern_programs.empty()) {
    out << ".pats\n";
    for (const PatternProgramEntry &entry : module.pattern_programs) {
      out << "  pat " << entry.pattern_id << " bindings=" << entry.binding_count
          << " flags=" << entry.flags << "\n";
    }
  }
  if (!module.line_table.empty()) {
    out << ".line\n";
    for (const LineEntry &entry : module.line_table) {
      out << "  c" << entry.code_id << " pc=" << entry.pc
          << " line=" << entry.line << "\n";
    }
  }
  if (!module.local_debug.empty()) {
    out << ".locs\n";
    for (const LocalDebugEntry &entry : module.local_debug) {
      out << "  c" << entry.code_id << " l" << entry.slot << " name=s"
          << entry.name_str_id << "("
          << json_escape(
                 string_or_placeholder(module.strings, entry.name_str_id))
          << ") " << entry.start_pc << ".." << entry.end_pc << "\n";
    }
  }
  if (!module.attrs.empty()) {
    out << ".attr\n";
    for (const AttrEntry &entry : module.attrs) {
      out << "  s" << entry.key_str_id << "("
          << json_escape(
                 string_or_placeholder(module.strings, entry.key_str_id))
          << ") = s" << entry.value_str_id << "("
          << json_escape(
                 string_or_placeholder(module.strings, entry.value_str_id))
          << ")\n";
    }
  }
  if (!module.required_features.empty() || !module.optional_features.empty() ||
      !module.forbidden_features.empty()) {
    out << ".prof\n";
    for (const std::string &feature : module.required_features) {
      out << "  required=\"" << json_escape(feature) << "\"\n";
    }
    for (const std::string &feature : module.optional_features) {
      out << "  optional=\"" << json_escape(feature) << "\"\n";
    }
    for (const std::string &feature : module.forbidden_features) {
      out << "  forbidden=\"" << json_escape(feature) << "\"\n";
    }
  }
  if (!module.capabilities.empty()) {
    out << ".caps\n";
    for (const capability::CapabilityRequest &entry : module.capabilities) {
      out << "  " << entry.name;
      if (!entry.target.empty()) {
        out << " target=\"" << json_escape(entry.target) << "\"";
      }
      if (!entry.reason.empty()) {
        out << " reason=\"" << json_escape(entry.reason) << "\"";
      }
      out << " flags=" << entry.flags << "\n";
    }
  }
  if (!module.effects.empty()) {
    out << ".efct\n";
    for (const effect::EffectSummary &entry : module.effects) {
      out << "  " << entry.owner << " kind=\"" << json_escape(entry.kind)
          << "\" declared=\""
          << json_escape(effect::effect_row_to_text(entry.declared_effects))
          << "\" observed=\""
          << json_escape(effect::effect_row_to_text(entry.observed_effects))
          << "\" flags=" << entry.flags << "\n";
    }
  }
  if (!module.observability_sites.empty()) {
    out << ".obsv\n";
    for (const replay::ObservabilitySite &entry : module.observability_sites) {
      out << "  site=" << entry.site_id << " event=\""
          << json_escape(entry.event_name) << "\" kind=\""
          << json_escape(entry.kind) << "\" owner=\""
          << json_escape(entry.owner) << "\" source=\""
          << json_escape(entry.source.file) << ":" << entry.source.line << ":"
          << entry.source.column << "\" flags=" << entry.flags << "\n";
    }
  }
  const replay::ReplayMetadata replay_metadata =
      replay::normalize_metadata(module.replay_metadata);
  if (!replay_metadata.required_event_names.empty() ||
      !replay_metadata.deterministic_sources.empty() ||
      replay_metadata.flags != 0U) {
    out << ".rply flags=" << replay_metadata.flags << "\n";
    for (const std::string &event_name : replay_metadata.required_event_names) {
      out << "  required_event=\"" << json_escape(event_name) << "\"\n";
    }
    for (const std::string &source : replay_metadata.deterministic_sources) {
      out << "  deterministic_source=\"" << json_escape(source) << "\"\n";
    }
  }
  const data::SchemaValidationResult schema_metadata =
      data::validate_schemas(module.schemas, module.schema_migrations);
  if (!schema_metadata.schemas.empty() || !schema_metadata.migrations.empty()) {
    out << ".scma\n";
    for (const data::SchemaDefinition &schema : schema_metadata.schemas) {
      out << "  schema " << schema.name << " version=" << schema.version
          << " flags=" << schema.flags << "\n";
      for (const data::SchemaField &field : schema.fields) {
        out << "    field " << field.name << " type=\"" << field.type
            << "\" required=" << (field.required ? 1 : 0)
            << " nullable=" << (field.nullable ? 1 : 0);
        if (!field.default_value.empty()) {
          out << " default=\"" << json_escape(field.default_value) << "\"";
        }
        out << " flags=" << field.flags << "\n";
      }
    }
    for (const data::SchemaMigration &migration : schema_metadata.migrations) {
      out << "  migration " << migration.schema_name << " "
          << migration.from_version << "->" << migration.to_version
          << " kind=\"" << json_escape(migration.kind)
          << "\" flags=" << migration.flags << "\n";
    }
  }
  const data::TablePlanValidationResult table_metadata =
      data::validate_table_plans(module.table_plans);
  if (!table_metadata.plans.empty()) {
    out << ".tabl\n";
    for (const data::TablePlan &plan : table_metadata.plans) {
      out << "  plan " << plan.plan_id << " op=\"" << json_escape(plan.op)
          << "\" fingerprint=\"" << data::table_plan_fingerprint(plan)
          << "\" flags=" << plan.flags << "\n";
      for (const std::string &input : plan.input_refs) {
        out << "    input=\"" << json_escape(input) << "\"\n";
      }
      for (const std::string &argument : plan.arguments) {
        out << "    arg=\"" << json_escape(argument) << "\"\n";
      }
      for (const data::ColumnDependency &dependency :
           plan.column_dependencies) {
        out << "    dep=\"" << json_escape(dependency.table_ref) << "."
            << json_escape(dependency.column_ref) << "\"\n";
      }
      out << "    effect_row=\""
          << json_escape(effect::effect_row_to_text(plan.effect_row)) << "\"\n";
    }
  }
  const wasm_accel::WasmComponentValidationResult wasm_metadata =
      wasm_accel::validate_wasm_components(module.wasm_components);
  if (!wasm_metadata.components.empty()) {
    out << ".wasm\n";
    for (const wasm_accel::WasmComponent &component :
         wasm_metadata.components) {
      out << "  component " << component.name << " world=\""
          << json_escape(component.world) << "\" flags=" << component.flags
          << "\n";
      for (const wasm_accel::WasmInterfaceEntry &entry : component.imports) {
        out << "    import " << entry.name << " kind=\""
            << json_escape(entry.kind) << "\" type=\""
            << json_escape(entry.type_signature) << "\"";
        if (!entry.schema_name.empty()) {
          out << " schema=\"" << json_escape(entry.schema_name) << "\"";
        }
        if (!entry.capability.name.empty()) {
          out << " capability=\""
              << json_escape(capability::request_to_text(entry.capability))
              << "\"";
        }
        out << " effects=\""
            << json_escape(effect::effect_row_to_text(entry.effect_row))
            << "\" flags=" << entry.flags << "\n";
      }
      for (const wasm_accel::WasmInterfaceEntry &entry : component.exports) {
        out << "    export " << entry.name << " kind=\""
            << json_escape(entry.kind) << "\" type=\""
            << json_escape(entry.type_signature) << "\"";
        if (!entry.schema_name.empty()) {
          out << " schema=\"" << json_escape(entry.schema_name) << "\"";
        }
        out << " effects=\""
            << json_escape(effect::effect_row_to_text(entry.effect_row))
            << "\" flags=" << entry.flags << "\n";
      }
    }
  }
  const wasm_accel::AcceleratorValidationResult accelerator_metadata =
      wasm_accel::validate_accelerator_kernels(module.accelerator_kernels);
  if (!accelerator_metadata.kernels.empty()) {
    out << ".accl\n";
    for (const wasm_accel::AcceleratorKernel &kernel :
         accelerator_metadata.kernels) {
      out << "  kernel " << kernel.kernel_id << " entry=\""
          << json_escape(kernel.entry) << "\" target=\""
          << json_escape(kernel.target) << "\" effects=\""
          << json_escape(effect::effect_row_to_text(kernel.effect_row))
          << "\" flags=" << kernel.flags << "\n";
      for (const wasm_accel::AcceleratorValue &value : kernel.params) {
        out << "    param " << value.name << " type=\""
            << json_escape(value.type) << "\" space=\""
            << json_escape(value.address_space) << "\" flags=" << value.flags
            << "\n";
      }
      for (const wasm_accel::AcceleratorValue &value : kernel.captures) {
        out << "    capture " << value.name << " type=\""
            << json_escape(value.type) << "\" space=\""
            << json_escape(value.address_space) << "\" flags=" << value.flags
            << "\n";
      }
      for (const std::string &feature : kernel.forbidden_features) {
        out << "    forbidden=\"" << json_escape(feature) << "\"\n";
      }
    }
  }
  const modern::AgentValidationResult agent_metadata =
      modern::validate_agent_metadata(module.agent_symbols,
                                      module.agent_patches,
                                      module.provenance_records);
  if (!agent_metadata.symbols.empty() || !agent_metadata.patches.empty() ||
      !agent_metadata.provenance.empty()) {
    out << ".agnt\n";
    for (const modern::AgentSymbol &symbol : agent_metadata.symbols) {
      out << "  symbol " << symbol.symbol_id << " name=\""
          << json_escape(symbol.name) << "\" kind=\""
          << json_escape(symbol.kind) << "\" module=\""
          << json_escape(symbol.module) << "\" visibility=\""
          << json_escape(symbol.visibility) << "\"\n";
    }
    for (const modern::AgentPatch &patch : agent_metadata.patches) {
      out << "  patch " << patch.patch_id << " intent=\""
          << json_escape(patch.intent)
          << "\" operations=" << patch.operations.size()
          << " flags=" << patch.flags << "\n";
    }
    for (const modern::ProvenanceRecord &record : agent_metadata.provenance) {
      out << "  provenance " << record.patch_id << " tool=\""
          << json_escape(record.tool)
          << "\" checks=" << record.checks_run.size()
          << " flags=" << record.flags << "\n";
    }
  }
  const modern::ContractValidationResult contract_metadata =
      modern::validate_contract_metadata(module.contracts, module.properties);
  if (!contract_metadata.contracts.empty() ||
      !contract_metadata.properties.empty()) {
    out << ".cntr\n";
    for (const modern::ContractSpec &contract : contract_metadata.contracts) {
      out << "  contract " << contract.owner << " kind=\""
          << json_escape(contract.kind) << "\" expr=\""
          << json_escape(contract.expression) << "\" effects=\""
          << json_escape(effect::effect_row_to_text(contract.effect_row))
          << "\" flags=" << contract.flags << "\n";
    }
    for (const modern::PropertySpec &property : contract_metadata.properties) {
      out << "  property \"" << json_escape(property.name)
          << "\" seed=" << property.seed << " generator=\""
          << json_escape(property.generator) << "\" flags=" << property.flags
          << "\n";
    }
  }
  const modern::PrivacyValidationResult privacy_metadata =
      modern::validate_privacy_metadata(
          module.privacy_labels, module.privacy_policies, module.lineage_nodes);
  if (!privacy_metadata.labels.empty() || !privacy_metadata.policies.empty() ||
      !privacy_metadata.lineage.empty()) {
    out << ".priv\n";
    for (const modern::PrivacyLabel &label : privacy_metadata.labels) {
      out << "  label " << label.name << " kind=\"" << json_escape(label.kind)
          << "\" flags=" << label.flags << "\n";
    }
    for (const modern::PrivacyPolicyRule &rule : privacy_metadata.policies) {
      out << "  policy " << rule.policy << " action=\""
          << json_escape(rule.action) << "\" label=\""
          << json_escape(rule.label) << "\" min_group=" << rule.min_group
          << " flags=" << rule.flags << "\n";
    }
    for (const modern::LineageNode &node : privacy_metadata.lineage) {
      out << "  lineage " << node.node_id << " kind=\""
          << json_escape(node.kind) << "\" output=\""
          << json_escape(node.output) << "\" flags=" << node.flags << "\n";
    }
  }
  const modern::WorkflowValidationResult workflow_metadata =
      modern::validate_workflow_metadata(module.workflow_steps,
                                         module.workflow_history);
  if (!workflow_metadata.steps.empty() || !workflow_metadata.history.empty()) {
    out << ".wflw\n";
    for (const modern::WorkflowStep &step : workflow_metadata.steps) {
      out << "  step " << step.workflow << "." << step.name << " effects=\""
          << json_escape(effect::effect_row_to_text(step.effect_row))
          << "\" idempotency_key=\"" << json_escape(step.idempotency_key)
          << "\" flags=" << step.flags << "\n";
    }
    for (const modern::WorkflowHistoryEvent &event :
         workflow_metadata.history) {
      out << "  history " << event.workflow_id << " step=\""
          << json_escape(event.step) << "\" event=\""
          << json_escape(event.event) << "\" key=\""
          << json_escape(event.idempotency_key) << "\" flags=" << event.flags
          << "\n";
    }
  }
  if (!module.hashes.empty()) {
    out << ".hash\n";
    for (const HashEntry &entry : module.hashes) {
      out << "  " << section_kind_name(entry.section)
          << " sha256=" << bytes_to_hex(entry.digest) << "\n";
    }
  }
  return out.str();
}

std::string verify_errors_to_json(const std::vector<VerifyError> &errors) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.bc.verify.v1\",\n";
  out << "  \"errors\": [\n";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    const VerifyError &error = errors[i];
    out << "    {\"code\":\"" << json_escape(error.code) << "\",\"message\":\""
        << json_escape(error.message) << "\",\"section\":\""
        << json_escape(error.section) << "\",\"offset\":" << error.offset
        << "}";
    if (i + 1U != errors.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::bytecode
