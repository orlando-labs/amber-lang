#include "bytecode/format.h"

#include "frontend/lexer/token.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
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
    for (std::size_t pc = 0; pc < code.instructions.size(); ++pc) {
      const Instruction &instruction = code.instructions[pc];
      const bool is_unconditional_jump = instruction.opcode == Opcode::Jump;
      const bool is_conditional_jump =
          instruction.opcode == Opcode::JumpIfTrue ||
          instruction.opcode == Opcode::JumpIfFalse ||
          instruction.opcode == Opcode::JumpIfNull;
      const std::size_t target_operand_index =
          is_unconditional_jump ? 0U : (is_conditional_jump ? 1U : 0U);

      if ((is_unconditional_jump || is_conditional_jump) &&
          instruction.operands.size() <= target_operand_index) {
        add_verify_error(errors, "BC1302",
                         "jump instruction is missing target operand",
                         SectionKind::Code, 0);
        continue;
      }
      if (is_unconditional_jump || is_conditional_jump) {
        const InstructionOperand &target_operand =
            instruction.operands[target_operand_index];
        if (target_operand.value < 0 ||
            static_cast<std::size_t>(target_operand.value) >= insn_count) {
          add_verify_error(errors, "BC1302", "jump target is out of range",
                           SectionKind::Code, 0);
          continue;
        }
        const std::size_t target =
            static_cast<std::size_t>(target_operand.value);
        if (target < pc && safepoints.find(static_cast<std::uint32_t>(
                               target)) == safepoints.end()) {
          add_verify_error(errors, "BC1303",
                           "back-edge jump target is missing safepoint",
                           SectionKind::Code, 0);
        }
      }
    }
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
    if (method.flags != 0U &&
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
