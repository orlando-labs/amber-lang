#pragma once

#include "bytecode/format.h"

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

struct RuntimeModuleSnapshot {
  std::string name;
  RuntimeModuleState state = RuntimeModuleState::Unloaded;
  std::vector<std::string> dependencies;
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
};

const char *runtime_module_state_name(RuntimeModuleState state);

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
  RuntimeModuleLoadResult link();
  RuntimeModuleLoadResult initialize_module(const std::string &name);
  RuntimeModuleLoadResult initialize_all();

  std::optional<RuntimeModuleSnapshot>
  module_snapshot(const std::string &name) const;
  std::vector<RuntimeModuleSnapshot> snapshots() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace amber::runtime
