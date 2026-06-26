#pragma once

#include "bytecode/format.h"
#include "runtime/world.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace amber::runtime {

enum class RuntimeModuleState {
  Unloaded,
  Mapped,
  Verified,
  Linked,
  Initializing,
  Ready,
  Failed
};

enum class RuntimeExportCellState {
  Uninitialized,
  Initializing,
  Ready,
  Failed
};

struct RuntimeDebugLocation {
  std::string module_name;
  std::uint32_t code_id = 0;
  std::uint32_t pc = 0;
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

struct RuntimeLoaderDiagnostic {
  std::string error_name;
  std::string message;
  std::string module_name;
  std::string dependency_name;
  std::string export_name;
  RuntimeDebugLocation location;
};

struct RuntimeExportCellSnapshot {
  std::string module_name;
  std::string public_name;
  std::string target_kind;
  std::uint32_t target_index = 0;
  bool has_reexport = false;
  std::string reexport_module_name;
  std::string reexport_export_name;
  std::string resolved_module_name;
  std::string resolved_export_name;
  RuntimeExportCellState state = RuntimeExportCellState::Uninitialized;
  bool read_only = true;
  std::string error_name;
  std::string message;
};

struct RuntimeImportAliasSnapshot {
  std::string module_name;
  std::string local_name;
  std::string dependency_name;
  std::string export_name;
  bool read_only = true;
  RuntimeExportCellSnapshot export_cell;
};

struct RuntimeModuleSnapshot {
  std::string name;
  RuntimeModuleState state = RuntimeModuleState::Unloaded;
  std::vector<std::string> dependencies;
  std::vector<RuntimeExportCellSnapshot> exports;
  std::vector<RuntimeImportAliasSnapshot> imports;
  bool has_init = false;
  std::uint32_t init_code_id = 0;
  std::uint64_t init_runs = 0;
  std::string error_name;
  std::string message;
};

struct RuntimeModuleLoadResult {
  bool ok = true;
  std::string error_name;
  std::string message;
  std::vector<std::string> init_order;
  std::vector<RuntimeModuleSnapshot> modules;
  std::vector<RuntimeLoaderDiagnostic> diagnostics;
  bool has_execution_result = false;
  Value value = Value::null();
  std::vector<ExecutionLocal> locals;
  std::vector<RuntimeWatchEvent> watch_events;
  std::uint64_t watch_epoch = 0;
  std::vector<std::string> runtime_strings;
  std::vector<std::string> runtime_symbols;
};

const char *runtime_module_state_name(RuntimeModuleState state);
const char *runtime_export_cell_state_name(RuntimeExportCellState state);

class RuntimeModuleLoader {
public:
  RuntimeModuleLoader();
  ~RuntimeModuleLoader();

  RuntimeModuleLoader(const RuntimeModuleLoader &) = delete;
  RuntimeModuleLoader &operator=(const RuntimeModuleLoader &) = delete;
  RuntimeModuleLoader(RuntimeModuleLoader &&) noexcept;
  RuntimeModuleLoader &operator=(RuntimeModuleLoader &&) noexcept;

  RuntimeModuleLoadResult
  add_serialized_module(const std::string &name,
                        const std::vector<std::uint8_t> &bytes);
  RuntimeModuleLoadResult add_import_alias(const std::string &module_name,
                                           const std::string &local_name,
                                           const std::string &dependency_name,
                                           const std::string &export_name);
  RuntimeModuleLoadResult link();
  RuntimeModuleLoadResult initialize_module(const std::string &name);
  RuntimeModuleLoadResult initialize_all();
  RuntimeModuleLoadResult read_import_alias(const std::string &module_name,
                                            const std::string &local_name);

  std::optional<RuntimeModuleSnapshot>
  module_snapshot(const std::string &name) const;
  std::vector<RuntimeModuleSnapshot> snapshots() const;
  std::optional<RuntimeExportCellSnapshot>
  export_snapshot(const std::string &module_name,
                  const std::string &export_name) const;
  std::optional<RuntimeImportAliasSnapshot>
  import_alias_snapshot(const std::string &module_name,
                        const std::string &local_name) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace amber::runtime
