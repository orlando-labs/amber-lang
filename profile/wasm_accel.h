#pragma once

#include "profile/capabilities.h"
#include "profile/effects.h"

#include <cstdint>
#include <string>
#include <vector>

namespace amber::wasm_accel {

inline constexpr std::uint32_t kWasmComponentFlagFrozenWorld = 0x1U;
inline constexpr std::uint32_t kWasmComponentFlagRawFfiDenied = 0x2U;
inline constexpr std::uint32_t kWasmComponentFlagWorldMutationDenied = 0x4U;
inline constexpr std::uint32_t kWasmInterfaceFlagSchemaBoundary = 0x1U;
inline constexpr std::uint32_t kWasmInterfaceFlagHostImport = 0x2U;

inline constexpr std::uint32_t kAcceleratorKernelFlagPureHelpersOnly = 0x1U;

struct WasmInterfaceEntry {
  std::string name;
  std::string kind;
  std::string type_signature;
  std::string schema_name;
  capability::CapabilityRequest capability;
  std::vector<std::string> effect_row;
  std::uint32_t flags = 0;
};

struct WasmComponent {
  std::string name;
  std::string world;
  std::vector<WasmInterfaceEntry> imports;
  std::vector<WasmInterfaceEntry> exports;
  std::uint32_t flags = 0;
};

struct AcceleratorValue {
  std::string name;
  std::string type;
  std::string address_space;
  std::uint32_t flags = 0;
};

struct AcceleratorKernel {
  std::string kernel_id;
  std::string entry;
  std::string target;
  std::vector<AcceleratorValue> params;
  std::vector<AcceleratorValue> captures;
  std::vector<std::string> effect_row;
  std::vector<std::string> forbidden_features;
  std::uint32_t flags = 0;
};

struct WasmAccelDiagnostic {
  std::string error_name;
  std::string message;
  std::string subject;
  std::string field;
};

struct WasmComponentValidationResult {
  bool ok = false;
  std::vector<WasmComponent> components;
  std::vector<WasmAccelDiagnostic> diagnostics;
};

struct AcceleratorValidationResult {
  bool ok = false;
  std::vector<AcceleratorKernel> kernels;
  std::vector<WasmAccelDiagnostic> diagnostics;
};

struct WasmAccelDocument {
  std::vector<WasmComponent> components;
  std::vector<AcceleratorKernel> kernels;
};

struct WasmAccelDocumentParseResult {
  WasmAccelDocument document;
  std::vector<WasmAccelDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

bool valid_component_name(const std::string &name);
bool valid_world_name(const std::string &name);
bool valid_wasm_interface_kind(const std::string &kind);
bool valid_component_boundary_type(const std::string &type_signature);
bool valid_accelerator_target(const std::string &target);
bool valid_accelerator_value_type(const std::string &type);
bool valid_accelerator_forbidden_feature(const std::string &feature);

WasmInterfaceEntry normalize_wasm_interface_entry(WasmInterfaceEntry entry);
WasmComponent normalize_wasm_component(WasmComponent component);
AcceleratorValue normalize_accelerator_value(AcceleratorValue value);
AcceleratorKernel normalize_accelerator_kernel(AcceleratorKernel kernel);

WasmComponentValidationResult
validate_wasm_components(const std::vector<WasmComponent> &components);
AcceleratorValidationResult
validate_accelerator_kernels(const std::vector<AcceleratorKernel> &kernels);

WasmAccelDocumentParseResult
parse_wasm_accel_document(const std::string &source);

std::string
wasm_component_validation_to_json(const WasmComponentValidationResult &result);
std::string
accelerator_validation_to_json(const AcceleratorValidationResult &result);

} // namespace amber::wasm_accel
