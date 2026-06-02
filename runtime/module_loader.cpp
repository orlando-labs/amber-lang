#include "runtime/module_loader.h"

#include "build/build.h"
#include "runtime/vm.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

namespace amber::runtime {

namespace {

struct RuntimeExportCell {
  std::string module_name;
  std::string public_name;
  std::string target_kind;
  std::uint32_t target_index = 0;
  bool has_reexport = false;
  std::string reexport_module_name;
  std::string reexport_export_name;
  RuntimeExportCellState state = RuntimeExportCellState::Uninitialized;
  std::string error_name;
  std::string message;
};

struct RuntimeImportAlias {
  std::string module_name;
  std::string local_name;
  std::string dependency_name;
  std::string export_name;
  bool read_only = true;
};

struct RuntimeModuleRecord {
  std::string name;
  bytecode::BcModule module;
  RuntimeModuleState state = RuntimeModuleState::Unloaded;
  std::uint64_t init_runs = 0;
  std::string error_name;
  std::string message;
  std::vector<RuntimeExportCell> exports;
  std::map<std::string, std::size_t> export_index;
  std::vector<RuntimeImportAlias> imports;
  std::map<std::string, std::size_t> import_index;
};

std::string dependency_name(const bytecode::BcModule &module,
                            const bytecode::DepEntry &dependency) {
  if (dependency.module_name_str_id >= module.strings.size()) {
    return "";
  }
  return module.strings[dependency.module_name_str_id];
}

std::string symbol_name(const bytecode::BcModule &module,
                        std::uint32_t symbol_id) {
  if (symbol_id >= module.symbols.size()) {
    return "";
  }
  return module.symbols[symbol_id];
}

std::string string_name(const bytecode::BcModule &module,
                        std::uint32_t string_id) {
  if (string_id >= module.strings.size()) {
    return "";
  }
  return module.strings[string_id];
}

bool version_less(const bytecode::Version &left,
                  const bytecode::Version &right) {
  if (left.major != right.major) {
    return left.major < right.major;
  }
  return left.minor < right.minor;
}

bool version_greater(const bytecode::Version &left,
                     const bytecode::Version &right) {
  if (left.major != right.major) {
    return left.major > right.major;
  }
  return left.minor > right.minor;
}

bool abi_hash_is_zero(const std::array<std::uint8_t, 32> &value) {
  return std::all_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte == 0U; });
}

RuntimeDebugLocation location_from_fault(const std::string &module_name,
                                         const Fault &fault) {
  RuntimeDebugLocation out;
  out.module_name = module_name;
  out.code_id = fault.code_id;
  out.pc = fault.pc;
  if (!fault.trace.empty()) {
    out.code_id = fault.trace.front().code_id;
    out.pc = fault.trace.front().pc;
    out.file = fault.trace.front().file;
    out.line = fault.trace.front().line;
    out.column = fault.trace.front().column;
  }
  return out;
}

RuntimeLoaderDiagnostic make_diagnostic(std::string error_name,
                                        std::string message,
                                        std::string module_name = {},
                                        std::string dependency_name = {},
                                        std::string export_name = {},
                                        RuntimeDebugLocation location = {}) {
  RuntimeLoaderDiagnostic diagnostic;
  diagnostic.error_name = std::move(error_name);
  diagnostic.message = std::move(message);
  diagnostic.module_name = std::move(module_name);
  diagnostic.dependency_name = std::move(dependency_name);
  diagnostic.export_name = std::move(export_name);
  diagnostic.location = std::move(location);
  return diagnostic;
}

RuntimeExportCellSnapshot
snapshot_for_export_cell(const RuntimeExportCell &cell,
                         const std::string &resolved_module_name = {},
                         const std::string &resolved_export_name = {},
                         std::optional<RuntimeExportCellState> state = {}) {
  RuntimeExportCellSnapshot snapshot;
  snapshot.module_name = cell.module_name;
  snapshot.public_name = cell.public_name;
  snapshot.target_kind = cell.target_kind;
  snapshot.target_index = cell.target_index;
  snapshot.has_reexport = cell.has_reexport;
  snapshot.reexport_module_name = cell.reexport_module_name;
  snapshot.reexport_export_name = cell.reexport_export_name;
  snapshot.resolved_module_name =
      resolved_module_name.empty() ? cell.module_name : resolved_module_name;
  snapshot.resolved_export_name =
      resolved_export_name.empty() ? cell.public_name : resolved_export_name;
  snapshot.state = state.value_or(cell.state);
  snapshot.error_name = cell.error_name;
  snapshot.message = cell.message;
  return snapshot;
}

RuntimeImportAliasSnapshot
snapshot_for_import_alias(const RuntimeImportAlias &alias,
                          const RuntimeExportCellSnapshot &export_cell) {
  RuntimeImportAliasSnapshot snapshot;
  snapshot.module_name = alias.module_name;
  snapshot.local_name = alias.local_name;
  snapshot.dependency_name = alias.dependency_name;
  snapshot.export_name = alias.export_name;
  snapshot.read_only = alias.read_only;
  snapshot.export_cell = export_cell;
  return snapshot;
}

RuntimeExportCellState export_state_for_module(RuntimeModuleState state) {
  switch (state) {
  case RuntimeModuleState::Unloaded:
  case RuntimeModuleState::Mapped:
  case RuntimeModuleState::Verified:
  case RuntimeModuleState::Linked:
    return RuntimeExportCellState::Uninitialized;
  case RuntimeModuleState::Initializing:
    return RuntimeExportCellState::Initializing;
  case RuntimeModuleState::Ready:
    return RuntimeExportCellState::Ready;
  case RuntimeModuleState::Failed:
    return RuntimeExportCellState::Failed;
  }
  return RuntimeExportCellState::Failed;
}

RuntimeModuleSnapshot snapshot_for(
    const RuntimeModuleRecord &record,
    const std::map<std::string, RuntimeModuleRecord> *all_modules = nullptr);

RuntimeExportCellSnapshot unresolved_export_snapshot() {
  RuntimeExportCell cell;
  return snapshot_for_export_cell(cell);
}

RuntimeExportCellSnapshot resolve_export_snapshot(
    const std::map<std::string, RuntimeModuleRecord> &modules,
    const std::string &module_name, const std::string &export_name,
    std::vector<std::string> *stack, RuntimeLoaderDiagnostic *diagnostic) {
  const std::string stack_key = module_name + ":" + export_name;
  if (std::find(stack->begin(), stack->end(), stack_key) != stack->end()) {
    std::ostringstream message;
    message << "cyclic re-export chain at '" << stack_key << "'";
    if (diagnostic != nullptr) {
      *diagnostic = make_diagnostic("ImportError", message.str(), module_name,
                                    {}, export_name);
    }
    RuntimeExportCell failed;
    failed.module_name = module_name;
    failed.public_name = export_name;
    failed.state = RuntimeExportCellState::Failed;
    failed.error_name = "ImportError";
    failed.message = message.str();
    return snapshot_for_export_cell(failed);
  }

  const auto module_it = modules.find(module_name);
  if (module_it == modules.end()) {
    std::ostringstream message;
    message << "module '" << module_name << "' is not loaded";
    if (diagnostic != nullptr) {
      *diagnostic = make_diagnostic("ImportError", message.str(), module_name,
                                    {}, export_name);
    }
    RuntimeExportCell failed;
    failed.module_name = module_name;
    failed.public_name = export_name;
    failed.state = RuntimeExportCellState::Failed;
    failed.error_name = "ImportError";
    failed.message = message.str();
    return snapshot_for_export_cell(failed);
  }

  const RuntimeModuleRecord &record = module_it->second;
  const auto export_it = record.export_index.find(export_name);
  if (export_it == record.export_index.end()) {
    std::ostringstream message;
    message << "module '" << module_name << "' has no export '" << export_name
            << "'";
    if (diagnostic != nullptr) {
      *diagnostic = make_diagnostic("ImportError", message.str(), module_name,
                                    module_name, export_name);
    }
    RuntimeExportCell failed;
    failed.module_name = module_name;
    failed.public_name = export_name;
    failed.state = RuntimeExportCellState::Failed;
    failed.error_name = "ImportError";
    failed.message = message.str();
    return snapshot_for_export_cell(failed);
  }

  const RuntimeExportCell &cell = record.exports[export_it->second];
  if (!cell.has_reexport) {
    return snapshot_for_export_cell(cell);
  }

  stack->push_back(stack_key);
  RuntimeExportCellSnapshot resolved =
      resolve_export_snapshot(modules, cell.reexport_module_name,
                              cell.reexport_export_name, stack, diagnostic);
  stack->pop_back();
  RuntimeExportCellSnapshot snapshot =
      snapshot_for_export_cell(cell, resolved.resolved_module_name,
                               resolved.resolved_export_name, resolved.state);
  snapshot.error_name = resolved.error_name;
  snapshot.message = resolved.message;
  return snapshot;
}

RuntimeExportCellSnapshot resolve_export_snapshot(
    const std::map<std::string, RuntimeModuleRecord> &modules,
    const std::string &module_name, const std::string &export_name,
    RuntimeLoaderDiagnostic *diagnostic = nullptr) {
  std::vector<std::string> stack;
  return resolve_export_snapshot(modules, module_name, export_name, &stack,
                                 diagnostic);
}

RuntimeModuleSnapshot
snapshot_for(const RuntimeModuleRecord &record,
             const std::map<std::string, RuntimeModuleRecord> *all_modules) {
  RuntimeModuleSnapshot snapshot;
  snapshot.name = record.name;
  snapshot.state = record.state;
  snapshot.has_init = record.module.init.has_entry_code_id;
  snapshot.init_code_id = record.module.init.entry_code_id;
  snapshot.init_runs = record.init_runs;
  snapshot.error_name = record.error_name;
  snapshot.message = record.message;
  for (const bytecode::DepEntry &dependency : record.module.dependencies) {
    snapshot.dependencies.push_back(dependency_name(record.module, dependency));
  }
  for (const RuntimeExportCell &cell : record.exports) {
    if (all_modules == nullptr) {
      snapshot.exports.push_back(snapshot_for_export_cell(cell));
    } else {
      snapshot.exports.push_back(resolve_export_snapshot(
          *all_modules, cell.module_name, cell.public_name));
    }
  }
  for (const RuntimeImportAlias &alias : record.imports) {
    RuntimeExportCellSnapshot export_cell;
    if (all_modules == nullptr) {
      export_cell = unresolved_export_snapshot();
    } else {
      export_cell = resolve_export_snapshot(*all_modules, alias.dependency_name,
                                            alias.export_name);
    }
    snapshot.imports.push_back(snapshot_for_import_alias(alias, export_cell));
  }
  return snapshot;
}

std::string cycle_message(const std::vector<std::string> &stack,
                          const std::string &name) {
  std::ostringstream out;
  out << "cyclic module initialization";
  const auto found = std::find(stack.begin(), stack.end(), name);
  if (found == stack.end()) {
    out << ": " << name;
    return out.str();
  }
  out << ": ";
  for (auto it = found; it != stack.end(); ++it) {
    if (it != found) {
      out << " -> ";
    }
    out << *it;
  }
  out << " -> " << name;
  return out.str();
}

} // namespace

struct RuntimeModuleLoader::Impl {
  std::map<std::string, RuntimeModuleRecord> modules;
  std::vector<std::string> init_order;

  RuntimeModuleLoadResult
  result(bool ok = true, std::string error_name = {}, std::string message = {},
         std::vector<RuntimeLoaderDiagnostic> diagnostics = {},
         const ExecutionResult *execution_result = nullptr) const {
    RuntimeModuleLoadResult out;
    out.ok = ok;
    out.error_name = std::move(error_name);
    out.message = std::move(message);
    out.init_order = init_order;
    for (const auto &[name, record] : modules) {
      (void)name;
      out.modules.push_back(snapshot_for(record, &modules));
    }
    out.diagnostics = std::move(diagnostics);
    if (execution_result != nullptr) {
      out.has_execution_result = true;
      out.value = execution_result->value;
      out.locals = execution_result->locals;
      out.watch_events = execution_result->watch_events;
      out.watch_epoch = execution_result->watch_epoch;
    }
    return out;
  }

  RuntimeModuleLoadResult
  error_result(RuntimeLoaderDiagnostic diagnostic) const {
    std::vector<RuntimeLoaderDiagnostic> diagnostics;
    diagnostics.push_back(diagnostic);
    return result(false, diagnostic.error_name, diagnostic.message,
                  std::move(diagnostics));
  }

  void fail_module(RuntimeModuleRecord &record, std::string error_name,
                   std::string message) {
    record.state = RuntimeModuleState::Failed;
    record.error_name = std::move(error_name);
    record.message = std::move(message);
    for (RuntimeExportCell &cell : record.exports) {
      cell.state = RuntimeExportCellState::Failed;
      cell.error_name = record.error_name;
      cell.message = record.message;
    }
  }

  void sync_export_states(RuntimeModuleRecord &record) {
    const RuntimeExportCellState state = export_state_for_module(record.state);
    for (RuntimeExportCell &cell : record.exports) {
      cell.state = state;
      if (state == RuntimeExportCellState::Failed) {
        cell.error_name = record.error_name;
        cell.message = record.message;
      } else if (state == RuntimeExportCellState::Ready ||
                 state == RuntimeExportCellState::Initializing ||
                 state == RuntimeExportCellState::Uninitialized) {
        cell.error_name.clear();
        cell.message.clear();
      }
    }
  }

  void materialize_exports(RuntimeModuleRecord &record) {
    record.exports.clear();
    record.export_index.clear();
    for (const bytecode::ExportEntry &entry : record.module.exports) {
      RuntimeExportCell cell;
      cell.module_name = record.name;
      cell.public_name = symbol_name(record.module, entry.symbol_id);
      cell.target_kind = string_name(record.module, entry.target_kind_str_id);
      cell.target_index = entry.target_index;
      cell.has_reexport =
          entry.has_reexport_module_name || cell.target_kind == "reexport";
      if (entry.has_reexport_module_name) {
        cell.reexport_module_name =
            string_name(record.module, entry.reexport_module_name_str_id);
      }
      if (cell.target_kind == "reexport" &&
          entry.target_index < record.module.strings.size()) {
        cell.reexport_export_name = record.module.strings[entry.target_index];
      } else {
        cell.reexport_export_name = cell.public_name;
      }
      const std::size_t index = record.exports.size();
      record.export_index[cell.public_name] = index;
      record.exports.push_back(std::move(cell));
    }
    sync_export_states(record);
  }

  void materialize_all_exports() {
    for (auto &[name, record] : modules) {
      (void)name;
      materialize_exports(record);
    }
  }

  std::optional<RuntimeLoaderDiagnostic>
  check_profile_compatibility(const RuntimeModuleRecord &record) const {
    for (const std::string &feature : record.module.required_features) {
      if (!build::runtime_supports_feature(feature)) {
        std::ostringstream message;
        message << "module '" << record.name
                << "' requires unsupported profile feature '" << feature << "'";
        return make_diagnostic("UnsupportedProfileError", message.str(),
                               record.name);
      }
    }
    for (const std::string &feature : record.module.forbidden_features) {
      if (build::runtime_supports_feature(feature)) {
        std::ostringstream message;
        message << "module '" << record.name
                << "' forbids host profile feature '" << feature << "'";
        return make_diagnostic("UnsupportedProfileError", message.str(),
                               record.name);
      }
    }
    return std::nullopt;
  }

  std::optional<RuntimeLoaderDiagnostic>
  check_dependency_compatibility(const RuntimeModuleRecord &record,
                                 const bytecode::DepEntry &dependency,
                                 const RuntimeModuleRecord &dep_record) const {
    const std::string dep_name = dependency_name(record.module, dependency);
    if (version_less(dep_record.module.format_version,
                     dependency.required_format)) {
      std::ostringstream message;
      message << "module '" << record.name << "' requires module '" << dep_name
              << "' bytecode format >= " << dependency.required_format.major
              << "." << dependency.required_format.minor << ", got "
              << dep_record.module.format_version.major << "."
              << dep_record.module.format_version.minor;
      return make_diagnostic("ImportError", message.str(), record.name,
                             dep_name);
    }
    if (version_less(dep_record.module.language_version,
                     dependency.min_language_version)) {
      std::ostringstream message;
      message << "module '" << record.name << "' requires module '" << dep_name
              << "' language >= " << dependency.min_language_version.major
              << "." << dependency.min_language_version.minor << ", got "
              << dep_record.module.language_version.major << "."
              << dep_record.module.language_version.minor;
      return make_diagnostic("ImportError", message.str(), record.name,
                             dep_name);
    }
    if (dependency.has_max_language_version &&
        version_greater(dep_record.module.language_version,
                        dependency.max_language_version)) {
      std::ostringstream message;
      message << "module '" << record.name << "' requires module '" << dep_name
              << "' language <= " << dependency.max_language_version.major
              << "." << dependency.max_language_version.minor << ", got "
              << dep_record.module.language_version.major << "."
              << dep_record.module.language_version.minor;
      return make_diagnostic("ImportError", message.str(), record.name,
                             dep_name);
    }
    if (dependency.has_abi_requirement &&
        !abi_hash_is_zero(dependency.abi_requirement) &&
        dependency.abi_requirement != dep_record.module.abi_hash) {
      std::ostringstream message;
      message << "module '" << record.name << "' requires module '" << dep_name
              << "' ABI hash that does not match the loaded module";
      return make_diagnostic("ImportError", message.str(), record.name,
                             dep_name);
    }
    return std::nullopt;
  }

  std::optional<RuntimeLoaderDiagnostic>
  check_export_cell(const RuntimeModuleRecord &record,
                    const RuntimeExportCell &cell) const {
    const bool supported_kind =
        cell.target_kind == "method" || cell.target_kind == "class" ||
        cell.target_kind == "code" || cell.target_kind == "module" ||
        cell.target_kind == "reexport";
    if (!supported_kind) {
      std::ostringstream message;
      message << "module '" << record.name << "' export '" << cell.public_name
              << "' has unsupported target kind '" << cell.target_kind << "'";
      return make_diagnostic("ImportError", message.str(), record.name, {},
                             cell.public_name);
    }
    if (cell.has_reexport) {
      if (cell.reexport_module_name.empty()) {
        std::ostringstream message;
        message << "module '" << record.name << "' re-export '"
                << cell.public_name << "' is missing a source module";
        return make_diagnostic("ImportError", message.str(), record.name, {},
                               cell.public_name);
      }
      RuntimeLoaderDiagnostic diagnostic;
      const RuntimeExportCellSnapshot resolved =
          resolve_export_snapshot(modules, cell.reexport_module_name,
                                  cell.reexport_export_name, &diagnostic);
      if (resolved.state == RuntimeExportCellState::Failed &&
          !diagnostic.error_name.empty()) {
        return diagnostic;
      }
    }
    return std::nullopt;
  }

  std::optional<RuntimeLoaderDiagnostic>
  check_import_alias(const RuntimeImportAlias &alias) const {
    const auto module_it = modules.find(alias.module_name);
    if (module_it == modules.end()) {
      std::ostringstream message;
      message << "import alias '" << alias.local_name
              << "' belongs to missing module '" << alias.module_name << "'";
      return make_diagnostic("ImportError", message.str(), alias.module_name,
                             alias.dependency_name, alias.export_name);
    }
    if (modules.find(alias.dependency_name) == modules.end()) {
      std::ostringstream message;
      message << "module '" << alias.module_name << "' imports missing module '"
              << alias.dependency_name << "'";
      return make_diagnostic("ImportError", message.str(), alias.module_name,
                             alias.dependency_name, alias.export_name);
    }
    RuntimeLoaderDiagnostic diagnostic;
    const RuntimeExportCellSnapshot resolved = resolve_export_snapshot(
        modules, alias.dependency_name, alias.export_name, &diagnostic);
    if (resolved.state == RuntimeExportCellState::Failed &&
        !diagnostic.error_name.empty()) {
      return make_diagnostic(diagnostic.error_name, diagnostic.message,
                             alias.module_name, alias.dependency_name,
                             alias.export_name, diagnostic.location);
    }
    return std::nullopt;
  }

  RuntimeModuleLoadResult link() {
    for (auto &[name, record] : modules) {
      (void)name;
      if (record.state == RuntimeModuleState::Failed) {
        return result(false, record.error_name, record.message);
      }
    }

    materialize_all_exports();

    for (auto &[name, record] : modules) {
      if (std::optional<RuntimeLoaderDiagnostic> diagnostic =
              check_profile_compatibility(record)) {
        return error_result(*diagnostic);
      }
      for (const bytecode::DepEntry &dependency : record.module.dependencies) {
        const std::string dep_name = dependency_name(record.module, dependency);
        const auto dep_it = modules.find(dep_name);
        if (dep_name.empty() || dep_it == modules.end()) {
          std::ostringstream message;
          message << "module '" << record.name
                  << "' depends on missing module '" << dep_name << "'";
          return error_result(make_diagnostic("ImportError", message.str(),
                                              record.name, dep_name));
        }
        if (std::optional<RuntimeLoaderDiagnostic> diagnostic =
                check_dependency_compatibility(record, dependency,
                                               dep_it->second)) {
          return error_result(*diagnostic);
        }
      }
    }

    for (const auto &[name, record] : modules) {
      (void)name;
      for (const RuntimeExportCell &cell : record.exports) {
        if (std::optional<RuntimeLoaderDiagnostic> diagnostic =
                check_export_cell(record, cell)) {
          return error_result(*diagnostic);
        }
      }
      for (const RuntimeImportAlias &alias : record.imports) {
        if (std::optional<RuntimeLoaderDiagnostic> diagnostic =
                check_import_alias(alias)) {
          return error_result(*diagnostic);
        }
      }
    }

    for (auto &[name, record] : modules) {
      (void)name;
      if (record.state == RuntimeModuleState::Verified ||
          record.state == RuntimeModuleState::Mapped) {
        record.state = RuntimeModuleState::Linked;
        sync_export_states(record);
      }
    }
    return result();
  }

  bool initialize_dfs(const std::string &name, std::vector<std::string> *stack,
                      std::string *error_name, std::string *message,
                      RuntimeLoaderDiagnostic *diagnostic,
                      std::optional<ExecutionResult> *execution_result =
                          nullptr) {
    auto found = modules.find(name);
    if (found == modules.end()) {
      *error_name = "ImportError";
      *message = "module '" + name + "' is not loaded";
      if (diagnostic != nullptr) {
        *diagnostic = make_diagnostic(*error_name, *message, name);
      }
      return false;
    }

    RuntimeModuleRecord &record = found->second;
    if (record.state == RuntimeModuleState::Ready) {
      return true;
    }
    if (record.state == RuntimeModuleState::Failed) {
      *error_name = record.error_name;
      *message = record.message;
      if (diagnostic != nullptr) {
        *diagnostic = make_diagnostic(*error_name, *message, name);
      }
      return false;
    }
    if (std::find(stack->begin(), stack->end(), name) != stack->end()) {
      const std::string cycle = cycle_message(*stack, name);
      const auto cycle_start = std::find(stack->begin(), stack->end(), name);
      for (auto it = cycle_start; it != stack->end(); ++it) {
        auto cycle_record = modules.find(*it);
        if (cycle_record != modules.end()) {
          fail_module(cycle_record->second, "ModuleInitError", cycle);
        }
      }
      fail_module(record, "ModuleInitError", cycle);
      *error_name = "ModuleInitError";
      *message = cycle;
      if (diagnostic != nullptr) {
        *diagnostic = make_diagnostic(*error_name, *message, name);
      }
      return false;
    }

    if (record.state != RuntimeModuleState::Linked &&
        record.state != RuntimeModuleState::Verified &&
        record.state != RuntimeModuleState::Mapped) {
      *error_name = "ModuleInitError";
      *message = "module '" + name + "' is not linked";
      fail_module(record, *error_name, *message);
      if (diagnostic != nullptr) {
        *diagnostic = make_diagnostic(*error_name, *message, name);
      }
      return false;
    }

    record.state = RuntimeModuleState::Initializing;
    sync_export_states(record);
    stack->push_back(name);

    for (const bytecode::DepEntry &dependency : record.module.dependencies) {
      const std::string dep_name = dependency_name(record.module, dependency);
      if (!initialize_dfs(dep_name, stack, error_name, message, diagnostic)) {
        if (record.state != RuntimeModuleState::Failed) {
          std::ostringstream dep_message;
          dep_message << "dependency '" << dep_name
                      << "' failed during module init: " << *message;
          fail_module(record, "ModuleInitError", dep_message.str());
          *error_name = record.error_name;
          *message = record.message;
          if (diagnostic != nullptr && diagnostic->error_name.empty()) {
            *diagnostic =
                make_diagnostic(*error_name, *message, name, dep_name);
          }
        }
        stack->pop_back();
        return false;
      }
    }

    if (record.module.init.has_entry_code_id) {
      ExecutionResult exec =
          execute_code(record.module, record.module.init.entry_code_id);
      if (!exec.ok()) {
        std::ostringstream init_message;
        init_message << "module '" << name << "' init failed";
        if (exec.fault.has_value()) {
          init_message << ": " << exec.fault->error_name << ": "
                       << exec.fault->message;
          if (!exec.fault->trace_text.empty()) {
            init_message << "\n" << exec.fault->trace_text;
          }
        }
        fail_module(record, "ModuleInitError", init_message.str());
        *error_name = record.error_name;
        *message = record.message;
        if (diagnostic != nullptr) {
          RuntimeDebugLocation location;
          if (exec.fault.has_value()) {
            location = location_from_fault(name, *exec.fault);
          }
          *diagnostic =
              make_diagnostic(*error_name, *message, name, {}, {}, location);
        }
        stack->pop_back();
        return false;
      }
      if (execution_result != nullptr) {
        *execution_result = exec;
      }
      ++record.init_runs;
    }

    record.state = RuntimeModuleState::Ready;
    sync_export_states(record);
    init_order.push_back(name);
    stack->pop_back();
    return true;
  }

  RuntimeModuleLoadResult read_import_alias(const std::string &module_name,
                                            const std::string &local_name) {
    RuntimeModuleLoadResult linked = link();
    if (!linked.ok) {
      return linked;
    }
    const auto module_it = modules.find(module_name);
    if (module_it == modules.end()) {
      return error_result(make_diagnostic(
          "ImportError", "module '" + module_name + "' is not loaded",
          module_name));
    }
    RuntimeModuleRecord &record = module_it->second;
    const auto alias_it = record.import_index.find(local_name);
    if (alias_it == record.import_index.end()) {
      return error_result(make_diagnostic("ImportError",
                                          "module '" + module_name +
                                              "' has no import alias '" +
                                              local_name + "'",
                                          module_name, {}, local_name));
    }
    const RuntimeImportAlias &alias = record.imports[alias_it->second];
    RuntimeLoaderDiagnostic diagnostic;
    const RuntimeExportCellSnapshot export_cell = resolve_export_snapshot(
        modules, alias.dependency_name, alias.export_name, &diagnostic);
    if (export_cell.state != RuntimeExportCellState::Ready) {
      std::ostringstream message;
      message << "import alias '" << local_name << "' in module '"
              << module_name << "' reads export '" << alias.export_name
              << "' from module '" << alias.dependency_name
              << "' before it is ready";
      return error_result(make_diagnostic("ModuleInitError", message.str(),
                                          module_name, alias.dependency_name,
                                          alias.export_name));
    }
    return result();
  }
};

const char *runtime_module_state_name(RuntimeModuleState state) {
  switch (state) {
  case RuntimeModuleState::Unloaded:
    return "unloaded";
  case RuntimeModuleState::Mapped:
    return "mapped";
  case RuntimeModuleState::Verified:
    return "verified";
  case RuntimeModuleState::Linked:
    return "linked";
  case RuntimeModuleState::Initializing:
    return "initializing";
  case RuntimeModuleState::Ready:
    return "ready";
  case RuntimeModuleState::Failed:
    return "failed";
  }
  return "unknown";
}

const char *runtime_export_cell_state_name(RuntimeExportCellState state) {
  switch (state) {
  case RuntimeExportCellState::Uninitialized:
    return "uninitialized";
  case RuntimeExportCellState::Initializing:
    return "initializing";
  case RuntimeExportCellState::Ready:
    return "ready";
  case RuntimeExportCellState::Failed:
    return "failed";
  }
  return "unknown";
}

RuntimeModuleLoader::RuntimeModuleLoader() : impl_(std::make_unique<Impl>()) {}

RuntimeModuleLoader::~RuntimeModuleLoader() = default;

RuntimeModuleLoader::RuntimeModuleLoader(RuntimeModuleLoader &&other) noexcept =
    default;

RuntimeModuleLoader &
RuntimeModuleLoader::operator=(RuntimeModuleLoader &&other) noexcept = default;

RuntimeModuleLoadResult RuntimeModuleLoader::add_serialized_module(
    const std::string &name, const std::vector<std::uint8_t> &bytes) {
  if (impl_ == nullptr) {
    return RuntimeModuleLoadResult{
        false, "ModuleLoadError", "module loader is moved-from", {}, {}, {},
        false, Value::null(), {}, {}, 0};
  }
  if (name.empty()) {
    return impl_->result(false, "ModuleLoadError", "module name is empty");
  }
  if (impl_->modules.find(name) != impl_->modules.end()) {
    return impl_->result(false, "ModuleLoadError",
                         "duplicate module '" + name + "'");
  }

  RuntimeModuleRecord record;
  record.name = name;
  record.state = RuntimeModuleState::Mapped;
  bytecode::DecodeResult decoded = bytecode::deserialize_module(bytes);
  if (!decoded.ok()) {
    record.state = RuntimeModuleState::Failed;
    record.error_name = "BytecodeVerificationError";
    record.message = bytecode::verify_errors_to_json(decoded.errors);
    impl_->modules.emplace(name, std::move(record));
    return impl_->result(false, "BytecodeVerificationError",
                         "module '" + name + "' failed bytecode verification");
  }

  record.module = std::move(decoded.module);
  record.state = RuntimeModuleState::Verified;
  impl_->materialize_exports(record);
  impl_->modules.emplace(name, std::move(record));
  return impl_->result();
}

RuntimeModuleLoadResult RuntimeModuleLoader::add_import_alias(
    const std::string &module_name, const std::string &local_name,
    const std::string &dependency_name, const std::string &export_name) {
  if (impl_ == nullptr) {
    return RuntimeModuleLoadResult{
        false, "ModuleLoadError", "module loader is moved-from", {}, {}, {},
        false, Value::null(), {}, {}, 0};
  }
  if (module_name.empty() || local_name.empty() || dependency_name.empty() ||
      export_name.empty()) {
    return impl_->result(false, "ModuleLoadError",
                         "import alias fields must be non-empty");
  }
  auto found = impl_->modules.find(module_name);
  if (found == impl_->modules.end()) {
    return impl_->error_result(
        make_diagnostic("ImportError",
                        "import alias '" + local_name +
                            "' belongs to missing module '" + module_name + "'",
                        module_name, dependency_name, export_name));
  }
  RuntimeModuleRecord &record = found->second;
  if (record.import_index.find(local_name) != record.import_index.end()) {
    return impl_->result(false, "ModuleLoadError",
                         "duplicate import alias '" + local_name +
                             "' in module '" + module_name + "'");
  }
  RuntimeImportAlias alias;
  alias.module_name = module_name;
  alias.local_name = local_name;
  alias.dependency_name = dependency_name;
  alias.export_name = export_name;
  const std::size_t index = record.imports.size();
  record.import_index[local_name] = index;
  record.imports.push_back(std::move(alias));
  return impl_->result();
}

RuntimeModuleLoadResult RuntimeModuleLoader::link() {
  if (impl_ == nullptr) {
    return RuntimeModuleLoadResult{
        false, "ModuleLoadError", "module loader is moved-from", {}, {}, {},
        false, Value::null(), {}, {}, 0};
  }
  return impl_->link();
}

RuntimeModuleLoadResult
RuntimeModuleLoader::initialize_module(const std::string &name) {
  if (impl_ == nullptr) {
    return RuntimeModuleLoadResult{
        false, "ModuleLoadError", "module loader is moved-from", {}, {}, {},
        false, Value::null(), {}, {}, 0};
  }
  RuntimeModuleLoadResult linked = impl_->link();
  if (!linked.ok) {
    return linked;
  }
  std::vector<std::string> stack;
  std::string error_name;
  std::string message;
  RuntimeLoaderDiagnostic diagnostic;
  std::optional<ExecutionResult> execution_result;
  if (!impl_->initialize_dfs(name, &stack, &error_name, &message,
                             &diagnostic, &execution_result)) {
    if (!diagnostic.error_name.empty()) {
      return impl_->error_result(std::move(diagnostic));
    }
    return impl_->result(false, error_name, message);
  }
  return impl_->result(true, {}, {}, {},
                       execution_result.has_value() ? &*execution_result
                                                    : nullptr);
}

RuntimeModuleLoadResult RuntimeModuleLoader::initialize_all() {
  if (impl_ == nullptr) {
    return RuntimeModuleLoadResult{
        false, "ModuleLoadError", "module loader is moved-from", {}, {}, {},
        false, Value::null(), {}, {}, 0};
  }
  RuntimeModuleLoadResult linked = impl_->link();
  if (!linked.ok) {
    return linked;
  }
  for (const auto &[name, record] : impl_->modules) {
    (void)record;
    std::vector<std::string> stack;
    std::string error_name;
    std::string message;
    RuntimeLoaderDiagnostic diagnostic;
    if (!impl_->initialize_dfs(name, &stack, &error_name, &message,
                               &diagnostic)) {
      if (!diagnostic.error_name.empty()) {
        return impl_->error_result(std::move(diagnostic));
      }
      return impl_->result(false, error_name, message);
    }
  }
  return impl_->result();
}

RuntimeModuleLoadResult
RuntimeModuleLoader::read_import_alias(const std::string &module_name,
                                       const std::string &local_name) {
  if (impl_ == nullptr) {
    return RuntimeModuleLoadResult{
        false, "ModuleLoadError", "module loader is moved-from", {}, {}, {},
        false, Value::null(), {}, {}, 0};
  }
  return impl_->read_import_alias(module_name, local_name);
}

std::optional<RuntimeModuleSnapshot>
RuntimeModuleLoader::module_snapshot(const std::string &name) const {
  if (impl_ == nullptr) {
    return std::nullopt;
  }
  const auto found = impl_->modules.find(name);
  if (found == impl_->modules.end()) {
    return std::nullopt;
  }
  return snapshot_for(found->second, &impl_->modules);
}

std::vector<RuntimeModuleSnapshot> RuntimeModuleLoader::snapshots() const {
  if (impl_ == nullptr) {
    return {};
  }
  std::vector<RuntimeModuleSnapshot> out;
  for (const auto &[name, record] : impl_->modules) {
    (void)name;
    out.push_back(snapshot_for(record, &impl_->modules));
  }
  return out;
}

std::optional<RuntimeExportCellSnapshot>
RuntimeModuleLoader::export_snapshot(const std::string &module_name,
                                     const std::string &export_name) const {
  if (impl_ == nullptr) {
    return std::nullopt;
  }
  const auto found = impl_->modules.find(module_name);
  if (found == impl_->modules.end()) {
    return std::nullopt;
  }
  if (found->second.export_index.empty() &&
      !found->second.module.exports.empty()) {
    return std::nullopt;
  }
  const auto export_it = found->second.export_index.find(export_name);
  if (export_it == found->second.export_index.end()) {
    return std::nullopt;
  }
  return resolve_export_snapshot(impl_->modules, module_name, export_name);
}

std::optional<RuntimeImportAliasSnapshot>
RuntimeModuleLoader::import_alias_snapshot(
    const std::string &module_name, const std::string &local_name) const {
  if (impl_ == nullptr) {
    return std::nullopt;
  }
  const auto found = impl_->modules.find(module_name);
  if (found == impl_->modules.end()) {
    return std::nullopt;
  }
  const auto alias_it = found->second.import_index.find(local_name);
  if (alias_it == found->second.import_index.end()) {
    return std::nullopt;
  }
  const RuntimeImportAlias &alias = found->second.imports[alias_it->second];
  const RuntimeExportCellSnapshot export_cell = resolve_export_snapshot(
      impl_->modules, alias.dependency_name, alias.export_name);
  return snapshot_for_import_alias(alias, export_cell);
}

} // namespace amber::runtime
