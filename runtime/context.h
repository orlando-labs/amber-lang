#pragma once

#include "runtime/concurrency.h"
#include "runtime/text.h"

#include <atomic>
#include <chrono>
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

// Layer B cooperative IO yield. When the VM drives a parkable task body it sets
// tls_runtime_io_park_enabled; io.cpp's wait_fd then, instead of blocking on
// the reactor, records the fd it would have waited on into
// tls_runtime_io_park_request and returns a RuntimeIoStatus with park=true. The
// VM turns that into a strand park plus a reactor wait_async registration that
// resumes the strand.
struct RuntimeIoParkRequest {
  int fd = -1;
  bool want_write = false;
  std::optional<std::chrono::steady_clock::time_point> deadline;
};
extern thread_local bool tls_runtime_io_park_enabled;
extern thread_local bool tls_runtime_io_park_requested;
extern thread_local RuntimeIoParkRequest tls_runtime_io_park_request;

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

// Strand-confinement owner identity for IO resources and byte buffers.
//
// Unlike current_runtime_owner_strand_id() -- which returns a bare strand id, a
// bare worker id, or 0, three values drawn from independent counters that all
// start at 1 -- this returns a single id whose low bits tag the namespace it
// came from (strand / worker / native thread). Tagging makes the id collision-
// free across namespaces, so a resource confined to strand N is never confused
// with one confined to worker N (or native thread N) just because the raw
// counters happen to coincide. Resource constructors stamp this value and the
// access checks compare against it, so a comparison can never put, say, a
// worker id on one side and a native-thread id on the other.
std::uint64_t current_runtime_resource_owner_id();

std::uint32_t current_runtime_io_wait_depth();

} // namespace amber::runtime
