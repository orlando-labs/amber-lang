#pragma once

#include "runtime/vm.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
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
extern thread_local const RuntimeIoWaitObserver *tls_runtime_io_wait_observer;
extern thread_local std::uint32_t tls_runtime_io_wait_depth;

extern std::atomic<std::uint64_t> g_runtime_output_order;
extern std::atomic<std::uint64_t> g_runtime_native_thread_ids;

// Resolves the source location attributed to text output events. A fixed
// value scope ("this exact location") and a provider scope ("ask the running
// VM at output time") share the same binding: whichever scope was entered
// most recently wins, so log replay can pin a captured location while a VM
// is live on the same thread.
using RuntimeTextSourceLocationProviderFn =
    RuntimeTextSourceLocation (*)(const void *ctx);

extern thread_local RuntimeTextSourceLocationProviderFn
    tls_runtime_text_source_location_provider;
extern thread_local const void *tls_runtime_text_source_location_provider_ctx;

RuntimeTextSourceLocation resolve_runtime_text_source_location();

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
  RuntimeTextSourceLocationProviderFn previous_provider_ = nullptr;
  const void *previous_provider_ctx_ = nullptr;
};

class RuntimeTextSourceLocationProviderScope {
public:
  RuntimeTextSourceLocationProviderScope(
      RuntimeTextSourceLocationProviderFn provider, const void *ctx);
  RuntimeTextSourceLocationProviderScope(
      const RuntimeTextSourceLocationProviderScope &) = delete;
  RuntimeTextSourceLocationProviderScope &
  operator=(const RuntimeTextSourceLocationProviderScope &) = delete;
  ~RuntimeTextSourceLocationProviderScope();

private:
  RuntimeTextSourceLocationProviderFn previous_provider_ = nullptr;
  const void *previous_provider_ctx_ = nullptr;
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

class RuntimeIoWaitObserverScope {
public:
  explicit RuntimeIoWaitObserverScope(const RuntimeIoWaitObserver *observer);
  RuntimeIoWaitObserverScope(const RuntimeIoWaitObserverScope &) = delete;
  RuntimeIoWaitObserverScope &
  operator=(const RuntimeIoWaitObserverScope &) = delete;
  ~RuntimeIoWaitObserverScope();

private:
  const RuntimeIoWaitObserver *previous_observer_ = nullptr;
};

class RuntimeIoWaitScope {
public:
  RuntimeIoWaitScope(
      std::string operation, std::string resource,
      RuntimeIoWaitInterest interest, std::uint64_t resource_id = 0,
      std::optional<std::chrono::milliseconds> timeout = std::nullopt);
  RuntimeIoWaitScope(const RuntimeIoWaitScope &) = delete;
  RuntimeIoWaitScope &operator=(const RuntimeIoWaitScope &) = delete;
  ~RuntimeIoWaitScope();

private:
  RuntimeIoWaitRecord record_;
  const RuntimeIoWaitObserver *observer_ = nullptr;
  bool active_ = false;
};

std::uint64_t current_runtime_owner_strand_id();
std::uint32_t current_runtime_io_wait_depth();

} // namespace amber::runtime
