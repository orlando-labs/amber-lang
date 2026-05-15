#include "runtime/module_loader.h"

#include "runtime/vm.h"

#include <algorithm>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

namespace amber::runtime {

namespace {

struct RuntimeModuleRecord {
  std::string name;
  bytecode::BcModule module;
  RuntimeModuleState state = RuntimeModuleState::Unloaded;
  std::uint64_t init_runs = 0;
  std::string error_name;
  std::string message;
};

std::string dependency_name(const bytecode::BcModule &module,
                            const bytecode::DepEntry &dependency) {
  if (dependency.module_name_str_id >= module.strings.size()) {
    return "";
  }
  return module.strings[dependency.module_name_str_id];
}

RuntimeModuleSnapshot snapshot_for(const RuntimeModuleRecord &record) {
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

  RuntimeModuleLoadResult result(bool ok = true, std::string error_name = {},
                                 std::string message = {}) const {
    RuntimeModuleLoadResult out;
    out.ok = ok;
    out.error_name = std::move(error_name);
    out.message = std::move(message);
    out.init_order = init_order;
    for (const auto &[name, record] : modules) {
      (void)name;
      out.modules.push_back(snapshot_for(record));
    }
    return out;
  }

  void fail_module(RuntimeModuleRecord &record, std::string error_name,
                   std::string message) {
    record.state = RuntimeModuleState::Failed;
    record.error_name = std::move(error_name);
    record.message = std::move(message);
  }

  RuntimeModuleLoadResult link() {
    for (auto &[name, record] : modules) {
      (void)name;
      if (record.state == RuntimeModuleState::Failed) {
        return result(false, record.error_name, record.message);
      }
    }

    for (auto &[name, record] : modules) {
      for (const bytecode::DepEntry &dependency : record.module.dependencies) {
        const std::string dep_name = dependency_name(record.module, dependency);
        if (dep_name.empty() || modules.find(dep_name) == modules.end()) {
          std::ostringstream message;
          message << "module '" << record.name
                  << "' depends on missing module '" << dep_name << "'";
          return result(false, "ImportError", message.str());
        }
      }
    }

    for (auto &[name, record] : modules) {
      (void)name;
      if (record.state == RuntimeModuleState::Verified ||
          record.state == RuntimeModuleState::Mapped) {
        record.state = RuntimeModuleState::Linked;
      }
    }
    return result();
  }

  bool initialize_dfs(const std::string &name, std::vector<std::string> *stack,
                      std::string *error_name, std::string *message) {
    auto found = modules.find(name);
    if (found == modules.end()) {
      *error_name = "ImportError";
      *message = "module '" + name + "' is not loaded";
      return false;
    }

    RuntimeModuleRecord &record = found->second;
    if (record.state == RuntimeModuleState::Ready) {
      return true;
    }
    if (record.state == RuntimeModuleState::Failed) {
      *error_name = record.error_name;
      *message = record.message;
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
      return false;
    }

    if (record.state != RuntimeModuleState::Linked &&
        record.state != RuntimeModuleState::Verified &&
        record.state != RuntimeModuleState::Mapped) {
      *error_name = "ModuleInitError";
      *message = "module '" + name + "' is not linked";
      fail_module(record, *error_name, *message);
      return false;
    }

    record.state = RuntimeModuleState::Initializing;
    stack->push_back(name);

    for (const bytecode::DepEntry &dependency : record.module.dependencies) {
      const std::string dep_name = dependency_name(record.module, dependency);
      if (!initialize_dfs(dep_name, stack, error_name, message)) {
        if (record.state != RuntimeModuleState::Failed) {
          std::ostringstream dep_message;
          dep_message << "dependency '" << dep_name
                      << "' failed during module init: " << *message;
          fail_module(record, "ModuleInitError", dep_message.str());
          *error_name = record.error_name;
          *message = record.message;
        }
        stack->pop_back();
        return false;
      }
    }

    if (record.module.init.has_entry_code_id) {
      const ExecutionResult exec =
          execute_code(record.module, record.module.init.entry_code_id);
      if (!exec.ok()) {
        std::ostringstream init_message;
        init_message << "module '" << name << "' init failed";
        if (exec.fault.has_value()) {
          init_message << ": " << exec.fault->error_name << ": "
                       << exec.fault->message;
        }
        fail_module(record, "ModuleInitError", init_message.str());
        *error_name = record.error_name;
        *message = record.message;
        stack->pop_back();
        return false;
      }
      ++record.init_runs;
    }

    record.state = RuntimeModuleState::Ready;
    init_order.push_back(name);
    stack->pop_back();
    return true;
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
        false, "ModuleLoadError", "module loader is moved-from", {}, {}};
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
  impl_->modules.emplace(name, std::move(record));
  return impl_->result();
}

RuntimeModuleLoadResult RuntimeModuleLoader::link() {
  if (impl_ == nullptr) {
    return RuntimeModuleLoadResult{
        false, "ModuleLoadError", "module loader is moved-from", {}, {}};
  }
  return impl_->link();
}

RuntimeModuleLoadResult
RuntimeModuleLoader::initialize_module(const std::string &name) {
  if (impl_ == nullptr) {
    return RuntimeModuleLoadResult{
        false, "ModuleLoadError", "module loader is moved-from", {}, {}};
  }
  RuntimeModuleLoadResult linked = impl_->link();
  if (!linked.ok) {
    return linked;
  }
  std::vector<std::string> stack;
  std::string error_name;
  std::string message;
  if (!impl_->initialize_dfs(name, &stack, &error_name, &message)) {
    return impl_->result(false, error_name, message);
  }
  return impl_->result();
}

RuntimeModuleLoadResult RuntimeModuleLoader::initialize_all() {
  if (impl_ == nullptr) {
    return RuntimeModuleLoadResult{
        false, "ModuleLoadError", "module loader is moved-from", {}, {}};
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
    if (!impl_->initialize_dfs(name, &stack, &error_name, &message)) {
      return impl_->result(false, error_name, message);
    }
  }
  return impl_->result();
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
  return snapshot_for(found->second);
}

std::vector<RuntimeModuleSnapshot> RuntimeModuleLoader::snapshots() const {
  if (impl_ == nullptr) {
    return {};
  }
  std::vector<RuntimeModuleSnapshot> out;
  for (const auto &[name, record] : impl_->modules) {
    (void)name;
    out.push_back(snapshot_for(record));
  }
  return out;
}

} // namespace amber::runtime
