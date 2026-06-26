#pragma once

#include "runtime/objects.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace amber::runtime {

enum class RuntimeGcCycle { Young, Full, Shared };

enum class RuntimePinViewKind { Opaque, ValueBuffer };

enum class RuntimePinPermission { ReadOnly, ReadWrite };

struct RuntimeArenaStats {
  std::uint64_t worker_id = 0;
  std::uint64_t allocations = 0;
  std::uint64_t live_objects = 0;
  std::uint64_t remote_queue_depth = 0;
};

struct RuntimeHeapStats {
  std::uint64_t allocations = 0;
  std::uint64_t live_objects = 0;
  // Sum of object-shell allocation_size over records still tracked in objects_.
  // tracked_object_bytes counts every malloc-live shell (including GC-reclaimed
  // shells whose payloads are cleared but whose memory a stale shared_ptr still
  // holds); live_object_bytes counts only logically-live shells. Neither counts
  // interior payloads (item vectors, strings) -- they are a cheap proxy for the
  // fragmentation ratio (RSS / live heap bytes), not an exact live-byte total.
  std::uint64_t live_object_bytes = 0;
  std::uint64_t tracked_object_bytes = 0;
  std::uint64_t local_frees = 0;
  std::uint64_t remote_frees_queued = 0;
  std::uint64_t remote_frees_drained = 0;
  std::uint64_t remote_queue_depth = 0;
  std::uint64_t worker_count = 0;
  std::uint64_t instance_allocations = 0;
  std::uint64_t array_allocations = 0;
  std::uint64_t map_allocations = 0;
  std::uint64_t closure_allocations = 0;
  std::uint64_t young_objects = 0;
  std::uint64_t mature_objects = 0;
  std::uint64_t shared_objects = 0;
  std::uint64_t gc_cycles = 0;
  std::uint64_t gc_young_cycles = 0;
  std::uint64_t gc_full_cycles = 0;
  std::uint64_t gc_shared_cycles = 0;
  std::uint64_t gc_marked_objects = 0;
  std::uint64_t gc_reclaimed_objects = 0;
  std::uint64_t gc_promoted_objects = 0;
  std::uint64_t gc_requested = 0;
  std::uint64_t gc_safepoint_collections = 0;
  std::uint64_t write_barriers = 0;
  std::uint64_t write_barrier_remembered = 0;
  std::uint64_t write_barrier_rejected_lifetime = 0;
  std::uint64_t write_barrier_rejected_isolation = 0;
  std::uint64_t remembered_set_objects = 0;
  std::uint64_t remembered_set_entries = 0;
  std::uint64_t active_pins = 0;
  std::uint64_t pinned_objects = 0;
  std::uint64_t pin_tokens_created = 0;
  std::uint64_t pin_unpins = 0;
  std::uint64_t pin_stale_unpins = 0;
  std::uint64_t opaque_handles_created = 0;
  std::uint64_t active_opaque_handles = 0;
  std::uint64_t buffer_views_created = 0;
  std::uint64_t native_waits_created = 0;
  std::uint64_t native_wait_cancellations = 0;
  std::vector<RuntimeArenaStats> arenas;
};

struct RuntimeGcResult {
  RuntimeGcCycle cycle = RuntimeGcCycle::Full;
  std::uint64_t cycle_id = 0;
  std::uint64_t roots = 0;
  std::uint64_t marked = 0;
  std::uint64_t reclaimed = 0;
  std::uint64_t promoted = 0;
  std::uint64_t remembered_entries = 0;
};

struct RuntimeWriteBarrierResult {
  bool ok = true;
  bool remembered = false;
  std::string error_name;
  std::string message;
};

struct RuntimePinToken {
  std::uint64_t pin_id = 0;
  std::uint64_t pin_epoch = 0;
  std::uint64_t allocation_id = 0;
  RuntimePinViewKind view_kind = RuntimePinViewKind::Opaque;
  RuntimePinPermission permissions = RuntimePinPermission::ReadOnly;
  OwnerToken owner;
  ObjectGeneration generation = ObjectGeneration::Young;
  bool active = false;
};

struct RuntimePinResult {
  bool ok = true;
  RuntimePinToken token;
  std::string error_name;
  std::string message;
};

struct RuntimeUnpinResult {
  bool ok = true;
  bool unpinned = false;
  bool stale = false;
  std::string error_name;
  std::string message;
};

struct RuntimeOpaqueHandle {
  std::uint64_t handle_id = 0;
  std::uint64_t pin_id = 0;
  std::uint64_t pin_epoch = 0;
  std::uint64_t allocation_id = 0;
  HeapObjectKind kind = HeapObjectKind::Instance;
  bool active = false;
};

struct RuntimeOpaqueHandleResult {
  bool ok = true;
  bool released = false;
  RuntimeOpaqueHandle handle;
  Value value = Value::null();
  std::string error_name;
  std::string message;
};

struct RuntimeValueBufferView {
  std::uint64_t pin_id = 0;
  const Value *data = nullptr;
  std::size_t size = 0;
  bool read_only = true;
  bool active = false;
};

struct RuntimeValueBufferViewResult {
  bool ok = true;
  RuntimeValueBufferView view;
  std::string error_name;
  std::string message;
};

struct RuntimeNativeWaitHandle {
  std::uint64_t wait_id = 0;
  std::uint64_t pin_id = 0;
  std::uint64_t pin_epoch = 0;
  bool active = false;
  bool cancellation_requested = false;
};

struct RuntimeNativeWaitResult {
  bool ok = true;
  bool cancelled = false;
  bool finished = false;
  RuntimeNativeWaitHandle handle;
  std::string error_name;
  std::string message;
};

class RuntimeHeap {
public:
  RuntimeHeap();
  ~RuntimeHeap();

  IntrusivePtr<InstanceValue>
  make_instance_value(std::uint32_t class_index = 0);
  IntrusivePtr<ClosureValue> make_closure_value();
  Value make_list_value(std::vector<Value> items, bool frozen = false);
  Value make_tuple_value(std::vector<Value> items);
  Value make_set_value(std::vector<Value> items, bool frozen = false);
  Value make_symbol_map_value(std::vector<MapEntry> entries,
                              bool frozen = false, bool strict = false);

  std::uint64_t drain_remote_frees();
  std::uint64_t drain_remote_frees(std::uint64_t worker_id);
  RuntimeWriteBarrierResult write_barrier(const Value &owner,
                                          const Value &value);
  RuntimeGcResult collect_garbage(const std::vector<Value> &roots = {},
                                  RuntimeGcCycle cycle = RuntimeGcCycle::Full,
                                  bool from_safepoint = false);
  void request_garbage_collection(RuntimeGcCycle cycle = RuntimeGcCycle::Full);
  std::optional<RuntimeGcCycle> pending_gc_request() const;
  RuntimePinResult
  pin(const Value &value,
      RuntimePinViewKind view_kind = RuntimePinViewKind::Opaque,
      RuntimePinPermission permissions = RuntimePinPermission::ReadOnly);
  RuntimeUnpinResult unpin(RuntimePinToken *token);
  std::uint64_t pin_count(const Value &value) const;
  bool is_pinned(const Value &value) const;
  RuntimeOpaqueHandleResult opaque_handle_for(const RuntimePinToken &token);
  RuntimeOpaqueHandleResult release_opaque_handle(RuntimeOpaqueHandle *handle);
  RuntimeOpaqueHandleResult
  resolve_opaque_handle(const RuntimeOpaqueHandle &handle) const;
  RuntimeValueBufferViewResult value_buffer_view(const RuntimePinToken &token);
  RuntimeNativeWaitResult register_native_wait(const RuntimePinToken &token);
  RuntimeNativeWaitResult cancel_native_wait(RuntimeNativeWaitHandle *handle);
  RuntimeNativeWaitResult
  poll_native_wait(const RuntimeNativeWaitHandle &handle) const;
  RuntimeNativeWaitResult finish_native_wait(RuntimeNativeWaitHandle *handle);
  RuntimeHeapStats stats() const;

  // Internal: free an ObjHeader-bearing object whose intrusive refcount hit
  // zero (RESEARCH 7.2). Public only so the out-of-line runtime_heap_release
  // template can reach it; it casts header.heap back to the private Impl and
  // runs the same physical-free / cross-strand-queue path as the old deleter.
  static void drop_object(void *obj, void (*deleter)(void *),
                          const ObjHeader &header) noexcept;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimePinScope {
public:
  RuntimePinScope(
      RuntimeHeap &heap, const Value &value,
      RuntimePinViewKind view_kind = RuntimePinViewKind::Opaque,
      RuntimePinPermission permissions = RuntimePinPermission::ReadOnly);
  RuntimePinScope(const RuntimePinScope &) = delete;
  RuntimePinScope &operator=(const RuntimePinScope &) = delete;
  RuntimePinScope(RuntimePinScope &&other) noexcept;
  RuntimePinScope &operator=(RuntimePinScope &&other) noexcept;
  ~RuntimePinScope();

  bool active() const;
  const RuntimePinResult &result() const;
  const RuntimePinToken &token() const;
  RuntimeUnpinResult unpin();

private:
  RuntimeHeap *heap_ = nullptr;
  RuntimePinResult result_;
};

RuntimeHeap &default_runtime_heap();

Value make_list_value(std::vector<Value> items, bool frozen = false);
Value make_tuple_value(std::vector<Value> items);
Value make_set_value(std::vector<Value> items, bool frozen = false);
Value make_symbol_map_value(std::vector<MapEntry> entries, bool frozen = false,
                            bool strict = false);

} // namespace amber::runtime
