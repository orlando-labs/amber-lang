#pragma once

#include "runtime/vm.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace amber::runtime {

extern thread_local std::uint64_t tls_runtime_worker_id;
extern thread_local std::uint64_t tls_runtime_strand_id;
extern thread_local std::uint64_t tls_runtime_task_id;
extern thread_local const std::atomic<bool> *tls_runtime_task_cancel_flag;
extern thread_local std::uint32_t tls_runtime_task_sync_depth;
extern thread_local std::shared_ptr<RuntimeTextWriter> tls_runtime_stdout;
extern thread_local std::shared_ptr<RuntimeTextWriter> tls_runtime_stderr;
extern thread_local std::string tls_runtime_task_annotation;
extern thread_local std::uint64_t tls_runtime_native_thread_id;
extern thread_local RuntimeTextSourceLocation tls_runtime_text_source_location;

extern std::atomic<std::uint64_t> g_runtime_output_order;
extern std::atomic<std::uint64_t> g_runtime_native_thread_ids;

class RuntimeTextSourceLocationScope {
public:
  explicit RuntimeTextSourceLocationScope(RuntimeTextSourceLocation location);
  RuntimeTextSourceLocationScope(const RuntimeTextSourceLocationScope &) =
      delete;
  RuntimeTextSourceLocationScope &
  operator=(const RuntimeTextSourceLocationScope &) = delete;
  ~RuntimeTextSourceLocationScope();

private:
  RuntimeTextSourceLocation previous_;
};

class RuntimeTaskScope {
public:
  RuntimeTaskScope(std::uint64_t task_id, const std::atomic<bool> *cancel_flag);
  RuntimeTaskScope(const RuntimeTaskScope &) = delete;
  RuntimeTaskScope &operator=(const RuntimeTaskScope &) = delete;
  ~RuntimeTaskScope();

private:
  std::uint64_t previous_task_id_ = 0;
  const std::atomic<bool> *previous_cancel_flag_ = nullptr;
};

class RuntimeTaskSyncScope {
public:
  RuntimeTaskSyncScope();
  RuntimeTaskSyncScope(const RuntimeTaskSyncScope &) = delete;
  RuntimeTaskSyncScope &operator=(const RuntimeTaskSyncScope &) = delete;
  ~RuntimeTaskSyncScope();
};

std::uint64_t current_runtime_owner_strand_id();

} // namespace amber::runtime
