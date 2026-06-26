#include "runtime/errors.h"

#include <string_view>

namespace amber::runtime {
namespace {

struct RuntimeErrorSpec {
  const char *name;
  const char *parent;
  const char *default_message;
  std::int64_t default_exit_code;
  std::uint32_t field_mask;
};

// Registry-ordered native error metadata. The single source of truth is the
// generated X-macro list, shared with the binder so expression lookup and the
// VM agree on dotted names, ancestry, and structured fields.
constexpr RuntimeErrorSpec kRuntimeErrorSpecs[] = {
#define AMBER_RUNTIME_ERROR(name, parent, default_message, default_exit_code,  \
                            field_mask)                                        \
  {name, parent, default_message, default_exit_code, field_mask},
#include "spec/registries/runtime_errors.def"
#undef AMBER_RUNTIME_ERROR
};

constexpr std::uint16_t kRuntimeErrorCount = static_cast<std::uint16_t>(
    sizeof(kRuntimeErrorSpecs) / sizeof(kRuntimeErrorSpecs[0]));

std::optional<std::uint16_t> runtime_error_parent_id(std::uint16_t error_id) {
  if (error_id >= kRuntimeErrorCount ||
      kRuntimeErrorSpecs[error_id].parent[0] == '\0') {
    return std::nullopt;
  }
  for (std::uint16_t i = 0; i < kRuntimeErrorCount; ++i) {
    if (std::string_view(kRuntimeErrorSpecs[i].name) ==
        kRuntimeErrorSpecs[error_id].parent) {
      return i;
    }
  }
  return std::nullopt;
}

} // namespace

const char *runtime_error_name(std::uint16_t error_id) {
  if (error_id >= kRuntimeErrorCount) {
    return "Error";
  }
  return kRuntimeErrorSpecs[error_id].name;
}

std::optional<std::uint16_t> runtime_error_id(const std::string &name) {
  for (std::uint16_t i = 0; i < kRuntimeErrorCount; ++i) {
    if (name == kRuntimeErrorSpecs[i].name) {
      return i;
    }
  }
  return std::nullopt;
}

bool runtime_error_is_a(std::uint16_t error_id,
                        std::uint16_t ancestor_error_id) {
  for (std::uint16_t depth = 0;
       error_id < kRuntimeErrorCount && depth < kRuntimeErrorCount; ++depth) {
    if (error_id == ancestor_error_id) {
      return true;
    }
    const std::optional<std::uint16_t> parent =
        runtime_error_parent_id(error_id);
    if (!parent.has_value()) {
      return false;
    }
    error_id = *parent;
  }
  return false;
}

std::uint32_t runtime_error_effective_field_mask(std::uint16_t error_id) {
  std::uint32_t mask = 0;
  for (std::uint16_t depth = 0;
       error_id < kRuntimeErrorCount && depth < kRuntimeErrorCount; ++depth) {
    mask |= kRuntimeErrorSpecs[error_id].field_mask;
    const std::optional<std::uint16_t> parent =
        runtime_error_parent_id(error_id);
    if (!parent.has_value()) {
      break;
    }
    error_id = *parent;
  }
  return mask;
}

std::optional<std::int64_t>
runtime_error_default_exit_code(std::uint16_t error_id) {
  for (std::uint16_t depth = 0;
       error_id < kRuntimeErrorCount && depth < kRuntimeErrorCount; ++depth) {
    if (kRuntimeErrorSpecs[error_id].default_exit_code >= 0) {
      return kRuntimeErrorSpecs[error_id].default_exit_code;
    }
    const std::optional<std::uint16_t> parent =
        runtime_error_parent_id(error_id);
    if (!parent.has_value()) {
      break;
    }
    error_id = *parent;
  }
  return std::nullopt;
}

const char *runtime_error_default_message(std::uint16_t error_id) {
  for (std::uint16_t depth = 0;
       error_id < kRuntimeErrorCount && depth < kRuntimeErrorCount; ++depth) {
    if (kRuntimeErrorSpecs[error_id].default_message[0] != '\0') {
      return kRuntimeErrorSpecs[error_id].default_message;
    }
    const std::optional<std::uint16_t> parent =
        runtime_error_parent_id(error_id);
    if (!parent.has_value()) {
      break;
    }
    error_id = *parent;
  }
  return "";
}

std::uint32_t runtime_error_field_bit(const std::string &name) {
  if (name == "option") {
    return kRuntimeErrorFieldOption;
  }
  if (name == "value") {
    return kRuntimeErrorFieldValue;
  }
  if (name == "exit_code") {
    return kRuntimeErrorFieldExitCode;
  }
  if (name == "usage") {
    return kRuntimeErrorFieldUsage;
  }
  if (name == "help") {
    return kRuntimeErrorFieldHelp;
  }
  return 0;
}

} // namespace amber::runtime
