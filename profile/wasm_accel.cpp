#include "profile/wasm_accel.h"

#include "profile/data.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace amber::wasm_accel {

namespace {

std::string trim(const std::string &value) {
  std::size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return value;
}

std::string canonical_type_text(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : trim(value)) {
    if (std::isspace(static_cast<unsigned char>(c)) == 0) {
      out.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return out;
}

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (const char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
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
      out << c;
      break;
    }
  }
  return out.str();
}

std::string line_unescape(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  bool escaping = false;
  for (const char c : value) {
    if (escaping) {
      if (c == 'n') {
        out.push_back('\n');
      } else if (c == 'r') {
        out.push_back('\r');
      } else {
        out.push_back(c);
      }
      escaping = false;
      continue;
    }
    if (c == '\\') {
      escaping = true;
      continue;
    }
    out.push_back(c);
  }
  if (escaping) {
    out.push_back('\\');
  }
  return out;
}

bool parse_u32(const std::string &value, std::uint32_t *out) {
  try {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(trim(value), &consumed, 10);
    if (consumed != trim(value).size() || parsed > 0xffffffffUL) {
      return false;
    }
    if (out != nullptr) {
      *out = static_cast<std::uint32_t>(parsed);
    }
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool parse_bool(const std::string &value, bool *out) {
  const std::string text = lower_ascii(trim(value));
  if (text == "1" || text == "true" || text == "yes") {
    if (out != nullptr) {
      *out = true;
    }
    return true;
  }
  if (text == "0" || text == "false" || text == "no") {
    if (out != nullptr) {
      *out = false;
    }
    return true;
  }
  return false;
}

std::map<std::string, std::string> parse_lines(const std::string &source) {
  std::map<std::string, std::string> values;
  std::istringstream in(source);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (first) {
      values["schema"] = trim(line);
      first = false;
      continue;
    }
    const std::string text = trim(line);
    if (text.empty() || text[0] == '#') {
      continue;
    }
    const std::size_t equals = text.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    values[trim(text.substr(0, equals))] =
        line_unescape(trim(text.substr(equals + 1U)));
  }
  return values;
}

std::string value_or_empty(const std::map<std::string, std::string> &values,
                           const std::string &key) {
  const auto found = values.find(key);
  return found == values.end() ? std::string{} : found->second;
}

WasmAccelDiagnostic diagnostic(std::string error_name, std::string message,
                               std::string subject = {},
                               std::string field = {}) {
  WasmAccelDiagnostic out;
  out.error_name = std::move(error_name);
  out.message = std::move(message);
  out.subject = std::move(subject);
  out.field = std::move(field);
  return out;
}

bool read_count(const std::map<std::string, std::string> &values,
                const std::string &key, std::uint32_t *count,
                std::vector<WasmAccelDiagnostic> *diagnostics,
                bool required = true) {
  const std::string raw = value_or_empty(values, key);
  if (raw.empty()) {
    if (required && diagnostics != nullptr) {
      diagnostics->push_back(
          diagnostic("WasmAcceleratorParseError", "missing count: " + key));
    }
    if (count != nullptr) {
      *count = 0;
    }
    return !required;
  }
  if (!parse_u32(raw, count)) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(
          diagnostic("WasmAcceleratorParseError", "invalid count: " + key));
    }
    return false;
  }
  return true;
}

std::vector<std::string> split_csv(const std::string &value) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::string item = trim(value.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!item.empty()) {
      out.push_back(item);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1U;
  }
  return out;
}

std::vector<std::string> sorted_unique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

bool valid_name_char(char c) {
  const unsigned char ch = static_cast<unsigned char>(c);
  return std::isalnum(ch) != 0 || c == '_' || c == '-' || c == '.';
}

bool valid_identifierish(const std::string &name) {
  if (name.empty() || name.front() == '.' || name.back() == '.') {
    return false;
  }
  for (const char c : name) {
    if (!valid_name_char(c)) {
      return false;
    }
  }
  return true;
}

const std::set<std::string> &wasm_kind_set() {
  static const std::set<std::string> names = {"func", "resource", "value"};
  return names;
}

const std::set<std::string> &accelerator_target_set() {
  static const std::set<std::string> names = {
      "cpu", "cuda", "gpu", "metal", "opencl", "simd", "vulkan", "webgpu"};
  return names;
}

const std::set<std::string> &forbidden_feature_set() {
  static const std::set<std::string> names = {
      "allocation",    "dynamic_dispatch",
      "env",           "exceptions",
      "ffi",           "fs",
      "hidden_io",     "net",
      "object_access", "random",
      "reflection",    "time",
      "watch",         "world_mutation"};
  return names;
}

const std::set<std::string> &primitive_value_type_set() {
  static const std::set<std::string> names = {
      "bool",   "boolean", "bytes", "f16", "f32", "f64",     "float",
      "i16",    "i32",     "i64",   "i8",  "int", "integer", "str",
      "string", "u16",     "u32",   "u64", "u8"};
  return names;
}

bool valid_generic_device_type(const std::string &type,
                               const std::string &prefix) {
  if (type.rfind(prefix + "[", 0) != 0 || type.back() != ']') {
    return false;
  }
  const std::string inner =
      type.substr(prefix.size() + 1U, type.size() - prefix.size() - 2U);
  return primitive_value_type_set().find(inner) !=
         primitive_value_type_set().end();
}

std::vector<WasmInterfaceEntry>
normalize_entries(std::vector<WasmInterfaceEntry> entries) {
  for (WasmInterfaceEntry &entry : entries) {
    entry = normalize_wasm_interface_entry(std::move(entry));
  }
  std::sort(
      entries.begin(), entries.end(),
      [](const WasmInterfaceEntry &left, const WasmInterfaceEntry &right) {
        if (left.name != right.name) {
          return left.name < right.name;
        }
        return left.kind < right.kind;
      });
  return entries;
}

std::vector<AcceleratorValue>
normalize_values(std::vector<AcceleratorValue> values) {
  for (AcceleratorValue &value : values) {
    value = normalize_accelerator_value(std::move(value));
  }
  std::sort(values.begin(), values.end(),
            [](const AcceleratorValue &left, const AcceleratorValue &right) {
              return left.name < right.name;
            });
  return values;
}

std::string prefixed_key(const std::string &prefix, const std::string &name) {
  return prefix + "." + name;
}

void parse_effects_value(const std::map<std::string, std::string> &values,
                         const std::string &key,
                         std::vector<std::string> *effects,
                         std::vector<WasmAccelDiagnostic> *diagnostics,
                         const std::string &subject) {
  const std::string raw = value_or_empty(values, key);
  if (raw.empty()) {
    if (effects != nullptr) {
      *effects = {};
    }
    return;
  }
  std::vector<effect::EffectDiagnostic> effect_diagnostics;
  std::vector<std::string> parsed;
  if (!effect::parse_effect_row(raw, &parsed, &effect_diagnostics, subject)) {
    if (diagnostics != nullptr) {
      for (const effect::EffectDiagnostic &item : effect_diagnostics) {
        diagnostics->push_back(diagnostic("WasmAcceleratorParseError",
                                          item.message, subject, key));
      }
    }
  }
  if (effects != nullptr) {
    *effects = std::move(parsed);
  }
}

capability::CapabilityRequest
parse_capability_value(const std::string &raw,
                       std::vector<WasmAccelDiagnostic> *diagnostics,
                       const std::string &subject, const std::string &field) {
  const std::string text = trim(raw);
  if (text.empty()) {
    return {};
  }
  const std::size_t colon = text.find(':');
  const std::string name =
      colon == std::string::npos ? text : trim(text.substr(0, colon));
  const std::string target = colon == std::string::npos
                                 ? std::string{}
                                 : trim(text.substr(colon + 1U));
  capability::CapabilityRequest request =
      capability::make_capability(name, target);
  if (!capability::valid_capability_name(request.name) &&
      diagnostics != nullptr) {
    diagnostics->push_back(diagnostic("WasmAcceleratorParseError",
                                      "invalid capability name: " + name,
                                      subject, field));
  }
  return request;
}

WasmInterfaceEntry
parse_interface_entry(const std::map<std::string, std::string> &values,
                      const std::string &prefix,
                      std::vector<WasmAccelDiagnostic> *diagnostics) {
  WasmInterfaceEntry entry;
  entry.name = value_or_empty(values, prefixed_key(prefix, "name"));
  entry.kind = value_or_empty(values, prefixed_key(prefix, "kind"));
  entry.type_signature = value_or_empty(values, prefixed_key(prefix, "type"));
  entry.schema_name = value_or_empty(values, prefixed_key(prefix, "schema"));
  parse_effects_value(values, prefixed_key(prefix, "effects"),
                      &entry.effect_row, diagnostics, entry.name);
  entry.capability = parse_capability_value(
      value_or_empty(values, prefixed_key(prefix, "capability")), diagnostics,
      entry.name, prefixed_key(prefix, "capability"));
  const std::string flags =
      value_or_empty(values, prefixed_key(prefix, "flags"));
  if (!flags.empty() && !parse_u32(flags, &entry.flags) &&
      diagnostics != nullptr) {
    diagnostics->push_back(diagnostic("WasmAcceleratorParseError",
                                      "invalid interface flags", entry.name,
                                      prefixed_key(prefix, "flags")));
  }
  return normalize_wasm_interface_entry(std::move(entry));
}

AcceleratorValue
parse_accelerator_value(const std::map<std::string, std::string> &values,
                        const std::string &prefix,
                        std::vector<WasmAccelDiagnostic> *diagnostics) {
  AcceleratorValue value;
  value.name = value_or_empty(values, prefixed_key(prefix, "name"));
  value.type = value_or_empty(values, prefixed_key(prefix, "type"));
  value.address_space = value_or_empty(values, prefixed_key(prefix, "space"));
  const std::string flags =
      value_or_empty(values, prefixed_key(prefix, "flags"));
  if (!flags.empty() && !parse_u32(flags, &value.flags) &&
      diagnostics != nullptr) {
    diagnostics->push_back(diagnostic(
        "WasmAcceleratorParseError", "invalid accelerator value flags",
        value.name, prefixed_key(prefix, "flags")));
  }
  return normalize_accelerator_value(std::move(value));
}

void parse_forbidden_feature_bool(
    const std::map<std::string, std::string> &values, const std::string &prefix,
    const std::string &name, std::vector<std::string> *features,
    std::vector<WasmAccelDiagnostic> *diagnostics, const std::string &subject) {
  const std::string raw = value_or_empty(values, prefixed_key(prefix, name));
  if (raw.empty()) {
    return;
  }
  bool enabled = false;
  if (!parse_bool(raw, &enabled)) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(diagnostic("WasmAcceleratorParseError",
                                        "invalid boolean for " + name, subject,
                                        prefixed_key(prefix, name)));
    }
    return;
  }
  if (enabled && features != nullptr) {
    features->push_back(name);
  }
}

} // namespace

bool valid_component_name(const std::string &name) {
  return valid_identifierish(name);
}

bool valid_world_name(const std::string &name) {
  return valid_identifierish(name);
}

bool valid_wasm_interface_kind(const std::string &kind) {
  return wasm_kind_set().find(lower_ascii(trim(kind))) != wasm_kind_set().end();
}

bool valid_component_boundary_type(const std::string &type_signature) {
  const std::string type = canonical_type_text(type_signature);
  if (type.empty()) {
    return false;
  }
  if (type.find('*') != std::string::npos ||
      type.find("ptr") != std::string::npos ||
      type.find("ffi") != std::string::npos) {
    return false;
  }
  return true;
}

bool valid_accelerator_target(const std::string &target) {
  return accelerator_target_set().find(lower_ascii(trim(target))) !=
         accelerator_target_set().end();
}

bool valid_accelerator_value_type(const std::string &type) {
  const std::string text = canonical_type_text(type);
  if (primitive_value_type_set().find(text) !=
      primitive_value_type_set().end()) {
    return true;
  }
  return valid_generic_device_type(text, "tensor") ||
         valid_generic_device_type(text, "devicebuffer") ||
         valid_generic_device_type(text, "buffer") ||
         valid_generic_device_type(text, "slice");
}

bool valid_accelerator_forbidden_feature(const std::string &feature) {
  return forbidden_feature_set().find(lower_ascii(trim(feature))) !=
         forbidden_feature_set().end();
}

WasmInterfaceEntry normalize_wasm_interface_entry(WasmInterfaceEntry entry) {
  entry.name = trim(entry.name);
  entry.kind = lower_ascii(trim(entry.kind));
  entry.type_signature = trim(entry.type_signature);
  entry.schema_name = trim(entry.schema_name);
  entry.effect_row = effect::normalize_effects(std::move(entry.effect_row));
  if (!entry.schema_name.empty()) {
    entry.flags |= kWasmInterfaceFlagSchemaBoundary;
  }
  if (!entry.capability.name.empty()) {
    entry.capability = capability::make_capability(
        entry.capability.name, entry.capability.target, entry.capability.reason,
        entry.capability.flags);
    entry.flags |= kWasmInterfaceFlagHostImport;
  }
  return entry;
}

WasmComponent normalize_wasm_component(WasmComponent component) {
  component.name = trim(component.name);
  component.world = trim(component.world);
  component.imports = normalize_entries(std::move(component.imports));
  component.exports = normalize_entries(std::move(component.exports));
  return component;
}

AcceleratorValue normalize_accelerator_value(AcceleratorValue value) {
  value.name = trim(value.name);
  value.type = trim(value.type);
  value.address_space = lower_ascii(trim(value.address_space));
  return value;
}

AcceleratorKernel normalize_accelerator_kernel(AcceleratorKernel kernel) {
  kernel.kernel_id = trim(kernel.kernel_id);
  kernel.entry = trim(kernel.entry);
  kernel.target = lower_ascii(trim(kernel.target));
  kernel.params = normalize_values(std::move(kernel.params));
  kernel.captures = normalize_values(std::move(kernel.captures));
  kernel.effect_row = effect::normalize_effects(std::move(kernel.effect_row));
  for (std::string &feature : kernel.forbidden_features) {
    feature = lower_ascii(trim(feature));
  }
  kernel.forbidden_features =
      sorted_unique(std::move(kernel.forbidden_features));
  return kernel;
}

WasmComponentValidationResult
validate_wasm_components(const std::vector<WasmComponent> &components) {
  WasmComponentValidationResult result;
  std::set<std::string> seen_components;
  for (WasmComponent component : components) {
    component = normalize_wasm_component(std::move(component));
    result.components.push_back(component);
    if (!valid_component_name(component.name)) {
      result.diagnostics.push_back(diagnostic("WasmProfileError",
                                              "invalid component name",
                                              component.name, "name"));
    }
    if (!valid_world_name(component.world)) {
      result.diagnostics.push_back(diagnostic("WasmProfileError",
                                              "invalid component world",
                                              component.name, "world"));
    }
    if (!seen_components.insert(component.name).second) {
      result.diagnostics.push_back(diagnostic("WasmProfileError",
                                              "duplicate component name",
                                              component.name, "name"));
    }
    if ((component.flags & kWasmComponentFlagFrozenWorld) == 0U) {
      result.diagnostics.push_back(
          diagnostic("WasmProfileError", "Wasm component must be frozen-world",
                     component.name, "flags"));
    }
    if ((component.flags & kWasmComponentFlagRawFfiDenied) == 0U) {
      result.diagnostics.push_back(diagnostic(
          "WasmProfileError", "raw FFI is forbidden in Wasm component profile",
          component.name, "raw_ffi"));
    }
    if ((component.flags & kWasmComponentFlagWorldMutationDenied) == 0U) {
      result.diagnostics.push_back(
          diagnostic("WasmProfileError",
                     "reflective world mutation is forbidden after component "
                     "instantiation",
                     component.name, "world_mutation"));
    }
    if (component.exports.empty()) {
      result.diagnostics.push_back(diagnostic(
          "WasmProfileError", "component must declare at least one export",
          component.name, "exports"));
    }

    auto validate_entry = [&](const WasmInterfaceEntry &entry,
                              const std::string &side) {
      if (!valid_identifierish(entry.name)) {
        result.diagnostics.push_back(diagnostic("WasmProfileError",
                                                "invalid " + side + " name",
                                                component.name, entry.name));
      }
      if (!valid_wasm_interface_kind(entry.kind)) {
        result.diagnostics.push_back(diagnostic("WasmProfileError",
                                                "invalid " + side + " kind",
                                                component.name, entry.name));
      }
      if (!valid_component_boundary_type(entry.type_signature)) {
        result.diagnostics.push_back(
            diagnostic("WasmProfileError", "invalid " + side + " boundary type",
                       component.name, entry.name));
      }
      if (!entry.schema_name.empty() &&
          !data::valid_schema_name(entry.schema_name)) {
        result.diagnostics.push_back(
            diagnostic("WasmProfileError", "invalid schema boundary name",
                       component.name, entry.schema_name));
      }
      if (!entry.capability.name.empty() &&
          !capability::valid_capability_name(entry.capability.name)) {
        result.diagnostics.push_back(
            diagnostic("WasmProfileError", "invalid host capability binding",
                       component.name, entry.capability.name));
      }
      const effect::EffectValidationResult effects =
          effect::validate_effect_summaries({effect::make_effect_summary(
              entry.name, side, entry.effect_row, entry.effect_row, true)});
      for (const effect::EffectDiagnostic &diag : effects.diagnostics) {
        result.diagnostics.push_back(diagnostic(
            "WasmProfileError", diag.message, component.name, entry.name));
      }
    };

    for (const WasmInterfaceEntry &entry : component.imports) {
      validate_entry(entry, "import");
    }
    for (const WasmInterfaceEntry &entry : component.exports) {
      validate_entry(entry, "export");
    }
  }
  std::sort(result.components.begin(), result.components.end(),
            [](const WasmComponent &left, const WasmComponent &right) {
              return left.name < right.name;
            });
  result.ok = result.diagnostics.empty();
  return result;
}

AcceleratorValidationResult
validate_accelerator_kernels(const std::vector<AcceleratorKernel> &kernels) {
  AcceleratorValidationResult result;
  std::set<std::string> seen_kernels;
  for (AcceleratorKernel kernel : kernels) {
    kernel = normalize_accelerator_kernel(std::move(kernel));
    result.kernels.push_back(kernel);
    if (!valid_identifierish(kernel.kernel_id)) {
      result.diagnostics.push_back(diagnostic(
          "AcceleratorError", "invalid kernel id", kernel.kernel_id, "id"));
    }
    if (!valid_identifierish(kernel.entry)) {
      result.diagnostics.push_back(diagnostic("AcceleratorError",
                                              "invalid kernel entry",
                                              kernel.kernel_id, "entry"));
    }
    if (!seen_kernels.insert(kernel.kernel_id).second) {
      result.diagnostics.push_back(diagnostic(
          "AcceleratorError", "duplicate kernel id", kernel.kernel_id, "id"));
    }
    if (!valid_accelerator_target(kernel.target)) {
      result.diagnostics.push_back(diagnostic("AcceleratorError",
                                              "invalid accelerator target",
                                              kernel.kernel_id, "target"));
    }
    const bool gpu_target =
        kernel.target == "gpu" || kernel.target == "cuda" ||
        kernel.target == "metal" || kernel.target == "opencl" ||
        kernel.target == "vulkan" || kernel.target == "webgpu";
    if (gpu_target && !effect::effects_subset_of({"gpu"}, kernel.effect_row)) {
      result.diagnostics.push_back(diagnostic(
          "AcceleratorError", "GPU accelerator kernels must carry !{gpu}",
          kernel.kernel_id, "effects"));
    }
    const effect::EffectValidationResult effects =
        effect::validate_effect_summaries({effect::make_effect_summary(
            kernel.kernel_id, "accelerator_kernel", kernel.effect_row,
            kernel.effect_row, true)});
    for (const effect::EffectDiagnostic &diag : effects.diagnostics) {
      result.diagnostics.push_back(diagnostic("AcceleratorError", diag.message,
                                              kernel.kernel_id, "effects"));
    }
    for (const AcceleratorValue &value : kernel.params) {
      if (!valid_identifierish(value.name)) {
        result.diagnostics.push_back(diagnostic("AcceleratorError",
                                                "invalid kernel parameter name",
                                                kernel.kernel_id, value.name));
      }
      if (!valid_accelerator_value_type(value.type)) {
        result.diagnostics.push_back(diagnostic("AcceleratorError",
                                                "invalid kernel parameter type",
                                                kernel.kernel_id, value.type));
      }
    }
    for (const AcceleratorValue &value : kernel.captures) {
      if (!valid_identifierish(value.name)) {
        result.diagnostics.push_back(diagnostic("AcceleratorError",
                                                "invalid kernel capture name",
                                                kernel.kernel_id, value.name));
      }
      if (!valid_accelerator_value_type(value.type)) {
        result.diagnostics.push_back(diagnostic("AcceleratorError",
                                                "invalid kernel capture type",
                                                kernel.kernel_id, value.type));
      }
    }
    for (const std::string &feature : kernel.forbidden_features) {
      if (!valid_accelerator_forbidden_feature(feature)) {
        result.diagnostics.push_back(
            diagnostic("AcceleratorError", "unknown forbidden kernel feature",
                       kernel.kernel_id, feature));
      } else {
        result.diagnostics.push_back(diagnostic(
            "AcceleratorError",
            "forbidden operation inside accelerator kernel: " + feature,
            kernel.kernel_id, feature));
      }
    }
  }
  std::sort(result.kernels.begin(), result.kernels.end(),
            [](const AcceleratorKernel &left, const AcceleratorKernel &right) {
              return left.kernel_id < right.kernel_id;
            });
  result.ok = result.diagnostics.empty();
  return result;
}

WasmAccelDocumentParseResult
parse_wasm_accel_document(const std::string &source) {
  WasmAccelDocumentParseResult result;
  const std::map<std::string, std::string> values = parse_lines(source);

  std::uint32_t component_count = 0;
  read_count(values, "wasm.count", &component_count, &result.diagnostics,
             false);
  for (std::uint32_t i = 0; i < component_count; ++i) {
    const std::string prefix = "wasm." + std::to_string(i);
    WasmComponent component;
    component.name = value_or_empty(values, prefixed_key(prefix, "name"));
    component.world = value_or_empty(values, prefixed_key(prefix, "world"));

    bool frozen = true;
    bool raw_ffi = false;
    bool world_mutation = false;
    const std::string frozen_value =
        value_or_empty(values, prefixed_key(prefix, "frozen"));
    const std::string raw_ffi_value =
        value_or_empty(values, prefixed_key(prefix, "raw_ffi"));
    const std::string world_mutation_value =
        value_or_empty(values, prefixed_key(prefix, "world_mutation"));
    if (!frozen_value.empty() && !parse_bool(frozen_value, &frozen)) {
      result.diagnostics.push_back(
          diagnostic("WasmAcceleratorParseError", "invalid frozen boolean",
                     component.name, prefixed_key(prefix, "frozen")));
    }
    if (!raw_ffi_value.empty() && !parse_bool(raw_ffi_value, &raw_ffi)) {
      result.diagnostics.push_back(
          diagnostic("WasmAcceleratorParseError", "invalid raw_ffi boolean",
                     component.name, prefixed_key(prefix, "raw_ffi")));
    }
    if (!world_mutation_value.empty() &&
        !parse_bool(world_mutation_value, &world_mutation)) {
      result.diagnostics.push_back(diagnostic(
          "WasmAcceleratorParseError", "invalid world_mutation boolean",
          component.name, prefixed_key(prefix, "world_mutation")));
    }
    if (frozen) {
      component.flags |= kWasmComponentFlagFrozenWorld;
    }
    if (!raw_ffi) {
      component.flags |= kWasmComponentFlagRawFfiDenied;
    }
    if (!world_mutation) {
      component.flags |= kWasmComponentFlagWorldMutationDenied;
    }

    const std::string flags =
        value_or_empty(values, prefixed_key(prefix, "flags"));
    if (!flags.empty() && !parse_u32(flags, &component.flags)) {
      result.diagnostics.push_back(
          diagnostic("WasmAcceleratorParseError", "invalid component flags",
                     component.name, prefixed_key(prefix, "flags")));
    }

    std::uint32_t import_count = 0;
    read_count(values, prefixed_key(prefix, "import.count"), &import_count,
               &result.diagnostics, false);
    for (std::uint32_t j = 0; j < import_count; ++j) {
      component.imports.push_back(parse_interface_entry(
          values, prefixed_key(prefix, "import." + std::to_string(j)),
          &result.diagnostics));
    }

    std::uint32_t export_count = 0;
    read_count(values, prefixed_key(prefix, "export.count"), &export_count,
               &result.diagnostics, false);
    for (std::uint32_t j = 0; j < export_count; ++j) {
      component.exports.push_back(parse_interface_entry(
          values, prefixed_key(prefix, "export." + std::to_string(j)),
          &result.diagnostics));
    }
    result.document.components.push_back(
        normalize_wasm_component(std::move(component)));
  }

  std::uint32_t kernel_count = 0;
  read_count(values, "kernel.count", &kernel_count, &result.diagnostics, false);
  for (std::uint32_t i = 0; i < kernel_count; ++i) {
    const std::string prefix = "kernel." + std::to_string(i);
    AcceleratorKernel kernel;
    kernel.kernel_id = value_or_empty(values, prefixed_key(prefix, "id"));
    kernel.entry = value_or_empty(values, prefixed_key(prefix, "entry"));
    kernel.target = value_or_empty(values, prefixed_key(prefix, "target"));
    parse_effects_value(values, prefixed_key(prefix, "effects"),
                        &kernel.effect_row, &result.diagnostics,
                        kernel.kernel_id);
    kernel.forbidden_features =
        split_csv(value_or_empty(values, prefixed_key(prefix, "forbidden")));
    for (const std::string &feature :
         {"allocation", "dynamic_dispatch", "reflection", "ffi", "hidden_io",
          "exceptions", "object_access", "watch", "random", "time", "fs", "net",
          "env", "world_mutation"}) {
      parse_forbidden_feature_bool(values, prefix, feature,
                                   &kernel.forbidden_features,
                                   &result.diagnostics, kernel.kernel_id);
    }
    const std::string flags =
        value_or_empty(values, prefixed_key(prefix, "flags"));
    if (!flags.empty() && !parse_u32(flags, &kernel.flags)) {
      result.diagnostics.push_back(
          diagnostic("WasmAcceleratorParseError", "invalid kernel flags",
                     kernel.kernel_id, prefixed_key(prefix, "flags")));
    }

    std::uint32_t param_count = 0;
    read_count(values, prefixed_key(prefix, "param.count"), &param_count,
               &result.diagnostics, false);
    for (std::uint32_t j = 0; j < param_count; ++j) {
      kernel.params.push_back(parse_accelerator_value(
          values, prefixed_key(prefix, "param." + std::to_string(j)),
          &result.diagnostics));
    }
    std::uint32_t capture_count = 0;
    read_count(values, prefixed_key(prefix, "capture.count"), &capture_count,
               &result.diagnostics, false);
    for (std::uint32_t j = 0; j < capture_count; ++j) {
      kernel.captures.push_back(parse_accelerator_value(
          values, prefixed_key(prefix, "capture." + std::to_string(j)),
          &result.diagnostics));
    }
    result.document.kernels.push_back(
        normalize_accelerator_kernel(std::move(kernel)));
  }
  return result;
}

std::string
wasm_component_validation_to_json(const WasmComponentValidationResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.wasm_component.v1\",\n";
  out << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n";
  out << "  \"components\": [";
  for (std::size_t i = 0; i < result.components.size(); ++i) {
    const WasmComponent &component = result.components[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(component.name)
        << "\",\"world\":\"" << json_escape(component.world)
        << "\",\"flags\":" << component.flags << ",\"imports\":[";
    for (std::size_t j = 0; j < component.imports.size(); ++j) {
      const WasmInterfaceEntry &entry = component.imports[j];
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
      const WasmInterfaceEntry &entry = component.exports[j];
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
  out << "\n  ],\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    const WasmAccelDiagnostic &diag = result.diagnostics[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error\":\"" << json_escape(diag.error_name)
        << "\",\"message\":\"" << json_escape(diag.message)
        << "\",\"subject\":\"" << json_escape(diag.subject) << "\",\"field\":\""
        << json_escape(diag.field) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

std::string
accelerator_validation_to_json(const AcceleratorValidationResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.accelerator.v1\",\n";
  out << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n";
  out << "  \"kernels\": [";
  for (std::size_t i = 0; i < result.kernels.size(); ++i) {
    const AcceleratorKernel &kernel = result.kernels[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"id\":\"" << json_escape(kernel.kernel_id)
        << "\",\"entry\":\"" << json_escape(kernel.entry) << "\",\"target\":\""
        << json_escape(kernel.target) << "\",\"effects\":\""
        << json_escape(effect::effect_row_to_text(kernel.effect_row))
        << "\",\"flags\":" << kernel.flags << ",\"params\":[";
    for (std::size_t j = 0; j < kernel.params.size(); ++j) {
      const AcceleratorValue &value = kernel.params[j];
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
      const AcceleratorValue &value = kernel.captures[j];
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
  out << "\n  ],\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    const WasmAccelDiagnostic &diag = result.diagnostics[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error\":\"" << json_escape(diag.error_name)
        << "\",\"message\":\"" << json_escape(diag.message)
        << "\",\"subject\":\"" << json_escape(diag.subject) << "\",\"field\":\""
        << json_escape(diag.field) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::wasm_accel
