#include "runtime/heap.h"

#include "runtime/context.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace amber::runtime {

namespace {

void increment_kind_allocation(RuntimeHeapStats &stats, HeapObjectKind kind) {
  switch (kind) {
  case HeapObjectKind::Instance:
    ++stats.instance_allocations;
    return;
  case HeapObjectKind::List:
  case HeapObjectKind::Tuple:
  case HeapObjectKind::Set:
    ++stats.array_allocations;
    return;
  case HeapObjectKind::Map:
    ++stats.map_allocations;
    return;
  case HeapObjectKind::Closure:
    ++stats.closure_allocations;
    return;
  }
}

} // namespace

RuntimeHeap &default_runtime_heap() {
  static RuntimeHeap heap;
  return heap;
}

class RuntimeHeap::Impl
    : public std::enable_shared_from_this<RuntimeHeap::Impl> {
public:
  struct RemoteFree {
    void *ptr = nullptr;
    void (*destroy)(void *) = nullptr;
    std::uint64_t owner_worker_id = 0;
    HeapObjectKind kind = HeapObjectKind::Instance;
    std::uint64_t allocation_id = 0;
  };

  struct ArenaState {
    std::uint64_t allocations = 0;
    std::uint64_t live_objects = 0;
    std::deque<RemoteFree> remote_frees;
  };

  struct ObjectRecord {
    void *ptr = nullptr;
    HeapObjectKind kind = HeapObjectKind::Instance;
    std::uint64_t owner_worker_id = 0;
    std::uint64_t allocation_id = 0;
    std::size_t allocation_size = 0;
    bool logical_live = true;
  };

  struct PinRecord {
    std::uint64_t pin_id = 0;
    std::uint64_t pin_epoch = 0;
    std::uint64_t allocation_id = 0;
    RuntimePinViewKind view_kind = RuntimePinViewKind::Opaque;
    RuntimePinPermission permissions = RuntimePinPermission::ReadOnly;
    OwnerToken owner;
    ObjectGeneration generation = ObjectGeneration::Young;
    Value value = Value::null();
    bool active = false;
  };

  struct OpaqueHandleRecord {
    std::uint64_t handle_id = 0;
    std::uint64_t pin_id = 0;
    std::uint64_t pin_epoch = 0;
    std::uint64_t allocation_id = 0;
    HeapObjectKind kind = HeapObjectKind::Instance;
    bool active = false;
  };

  struct NativeWaitRecord {
    std::uint64_t wait_id = 0;
    std::uint64_t pin_id = 0;
    std::uint64_t pin_epoch = 0;
    bool active = false;
    bool cancellation_requested = false;
  };

  ~Impl() { drain_all_remote_frees(); }

  // Allocations between pressure-driven remote-free drains (RESEARCH §7.5).
  static constexpr std::uint64_t kRemoteDrainAllocInterval = 256;

  template <typename T, typename Init>
  IntrusivePtr<T> allocate(HeapObjectKind kind, Init init) {
    auto *raw = new T();
    const std::uint64_t worker_id = current_runtime_worker_id();
    const std::size_t allocation_size = sizeof(T);
    const std::uint64_t allocation_id = reserve_allocation_id();
    raw->header.kind = kind;
    raw->header.owner.strand_id = current_runtime_owner_strand_id();
    raw->header.allocation_id = allocation_id;
    raw->header.arena_worker_id = worker_id;
    raw->header.allocation_size = allocation_size;
    raw->header.generation = ObjectGeneration::Young;
    // Keepalive to this heap: locates it on the drop path and keeps it alive as
    // long as any object references it (replaces the old deleter's Impl
    // capture).
    raw->header.heap = shared_from_this();
    // The returned handle owns this initial reference (adopted below).
    raw->header.ref_count.store(1, std::memory_order_relaxed);
    init(*raw);

    record_allocation(worker_id, kind, allocation_size, allocation_id, raw);
    IntrusivePtr<T> handle(raw, typename IntrusivePtr<T>::Adopt{});
    maybe_drain_on_pressure(worker_id);
    return handle;
  }

  template <typename T> IntrusivePtr<T> allocate(HeapObjectKind kind) {
    return allocate<T>(kind, [](T &) {});
  }

  // RESEARCH §7.5: drain this worker's queued cross-strand frees under its own
  // allocation pressure (every N allocations), in addition to the fixed
  // interpreter drain points, so dead objects owned by an actively-allocating
  // strand are reclaimed promptly instead of riding out the gaps between those
  // points. drain_remote_frees has a lock-free remote_free_pending_ early-out,
  // so this is ~free when nothing is queued (the common single-strand case).
  void maybe_drain_on_pressure(std::uint64_t worker_id) {
    static thread_local std::uint64_t allocations_since_drain = 0;
    if (++allocations_since_drain < kRemoteDrainAllocInterval) {
      return;
    }
    allocations_since_drain = 0;
    drain_remote_frees(worker_id);
  }

  // Drop-to-zero path for IntrusivePtr (RESEARCH §7.2): identical effect to the
  // old shared_ptr deleter -- free on the owning strand, queue a cross-strand
  // free otherwise. Public so RuntimeHeap::drop_object can reach it.
  void release_intrusive(void *ptr, void (*deleter)(void *),
                         const ObjHeader &header) {
    release({ptr, deleter, header.arena_worker_id, header.kind,
             header.allocation_id});
  }

  std::uint64_t drain_remote_frees(std::uint64_t worker_id) {
    if (remote_free_pending_.load(std::memory_order_acquire) == 0) {
      return 0;
    }
    std::deque<RemoteFree> pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ArenaState &arena = arena_for_worker(worker_id);
      pending.swap(arena.remote_frees);
      const std::uint64_t count = static_cast<std::uint64_t>(pending.size());
      if (count == 0) {
        return 0;
      }
      remote_free_pending_.fetch_sub(count, std::memory_order_acq_rel);
      stats_.remote_queue_depth -= count;
      stats_.remote_frees_drained += count;
      for (const RemoteFree &entry : pending) {
        const auto record = objects_.find(entry.allocation_id);
        if (record == objects_.end() || record->second.logical_live) {
          decrement_live_locked(arena, 1);
        }
        objects_.erase(entry.allocation_id);
        remove_remembered_edges_for_locked(entry.allocation_id);
      }
    }

    RuntimeWorkerScope owner_scope(worker_id);
    for (const RemoteFree &entry : pending) {
      entry.destroy(entry.ptr);
    }
    return static_cast<std::uint64_t>(pending.size());
  }

  RuntimeWriteBarrierResult write_barrier(const Value &owner,
                                          const Value &value) {
    RuntimeWriteBarrierResult out;
    const ObjHeader *owner_header = heap_header_from_value(owner);
    const ObjHeader *value_header = heap_header_from_value(value);
    if (owner_header == nullptr) {
      return out;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.write_barriers;

    const std::optional<std::string> owner_error =
        lifecycle_access_error_name(*owner_header);
    if (owner_error.has_value()) {
      ++stats_.write_barrier_rejected_lifetime;
      out.ok = false;
      out.error_name = *owner_error;
      out.message = lifecycle_access_error_message(*owner_error);
      return out;
    }
    if (value_header == nullptr) {
      return out;
    }

    const std::optional<std::string> value_error =
        lifecycle_access_error_name(*value_header);
    if (value_error.has_value()) {
      ++stats_.write_barrier_rejected_lifetime;
      out.ok = false;
      out.error_name = *value_error;
      out.message = lifecycle_access_error_message(*value_error);
      return out;
    }

    const bool owner_is_shared =
        owner_header->generation == ObjectGeneration::Shared ||
        owner_header->owner.kind == OwnerTokenKind::Shareable ||
        (owner_header->flags & kObjectFlagShareable) != 0U;
    const bool value_is_confined =
        value_header->owner.kind == OwnerTokenKind::Confined &&
        value_header->generation != ObjectGeneration::Shared;
    if (owner_is_shared && value_is_confined) {
      ++stats_.write_barrier_rejected_isolation;
      out.ok = false;
      out.error_name = "IsolationError";
      out.message = "shared object cannot reference confined object";
      return out;
    }

    if (owner_header->generation == ObjectGeneration::Mature &&
        value_header->generation == ObjectGeneration::Young &&
        owner_header->allocation_id != 0 && value_header->allocation_id != 0) {
      remembered_set_[owner_header->allocation_id].insert(
          value_header->allocation_id);
      ++stats_.write_barrier_remembered;
      out.remembered = true;
    }
    return out;
  }

  RuntimeGcResult collect_garbage(const std::vector<Value> &roots,
                                  RuntimeGcCycle cycle, bool from_safepoint) {
    RuntimeGcResult result;
    result.cycle = cycle;
    result.roots = static_cast<std::uint64_t>(roots.size());
    std::vector<Value> deferred_payload_release;

    std::lock_guard<std::mutex> lock(mutex_);
    pending_gc_cycle_.reset();
    gc_request_pending_.store(false, std::memory_order_release);
    ++stats_.gc_cycles;
    if (from_safepoint) {
      ++stats_.gc_safepoint_collections;
    }
    switch (cycle) {
    case RuntimeGcCycle::Young:
      ++stats_.gc_young_cycles;
      break;
    case RuntimeGcCycle::Full:
      ++stats_.gc_full_cycles;
      break;
    case RuntimeGcCycle::Shared:
      ++stats_.gc_shared_cycles;
      break;
    }

    const std::uint64_t mark_epoch = ++gc_epoch_;
    result.cycle_id = mark_epoch;
    std::vector<std::uint64_t> stack;
    std::unordered_set<std::uint64_t> visited;

    auto enqueue_id = [&](std::uint64_t allocation_id) {
      if (allocation_id == 0) {
        return;
      }
      const auto found = objects_.find(allocation_id);
      if (found == objects_.end() || !found->second.logical_live ||
          found->second.ptr == nullptr) {
        return;
      }
      if (visited.insert(allocation_id).second) {
        stack.push_back(allocation_id);
      }
    };

    auto enqueue_value = [&](const Value &value) {
      const ObjHeader *header = heap_header_from_value(value);
      if (header == nullptr) {
        return;
      }
      enqueue_id(header->allocation_id);
    };

    for (const Value &root : roots) {
      enqueue_value(root);
    }

    for (const auto &[pin_id, pin] : pins_) {
      (void)pin_id;
      if (pin.active) {
        ++result.roots;
        enqueue_value(pin.value);
      }
    }

    if (cycle == RuntimeGcCycle::Young) {
      for (const auto &[owner_id, children] : remembered_set_) {
        (void)owner_id;
        for (std::uint64_t child_id : children) {
          enqueue_id(child_id);
        }
      }
    }

    while (!stack.empty()) {
      const std::uint64_t allocation_id = stack.back();
      stack.pop_back();
      auto found = objects_.find(allocation_id);
      if (found == objects_.end() || !found->second.logical_live ||
          found->second.ptr == nullptr) {
        continue;
      }
      ObjectRecord &record = found->second;
      ObjHeader *header = header_for_record(record);
      if (header == nullptr) {
        continue;
      }
      header->gc_mark_epoch = mark_epoch;
      ++result.marked;
      if (header_is_deallocated(*header)) {
        continue;
      }
      std::vector<Value> children;
      append_child_values(record, &children);
      for (const Value &child : children) {
        enqueue_value(child);
      }
    }

    std::vector<std::uint64_t> reclaim_ids;
    for (const auto &[allocation_id, record] : objects_) {
      if (!record.logical_live) {
        continue;
      }
      const ObjHeader *header = header_for_record(record);
      if (header == nullptr) {
        continue;
      }
      if (!cycle_collects_header(cycle, *header)) {
        continue;
      }
      if (active_pin_counts_.find(allocation_id) != active_pin_counts_.end()) {
        continue;
      }
      if (visited.find(allocation_id) == visited.end()) {
        reclaim_ids.push_back(allocation_id);
      }
    }

    for (std::uint64_t allocation_id : reclaim_ids) {
      auto found = objects_.find(allocation_id);
      if (found == objects_.end() || !found->second.logical_live) {
        continue;
      }
      reclaim_record_locked(found->second, &deferred_payload_release);
      ++result.reclaimed;
    }

    for (auto &[allocation_id, record] : objects_) {
      (void)allocation_id;
      if (!record.logical_live ||
          visited.find(record.allocation_id) == visited.end()) {
        continue;
      }
      ObjHeader *header = header_for_record(record);
      if (header == nullptr || header_is_deallocated(*header)) {
        continue;
      }
      if (header->generation == ObjectGeneration::Young &&
          cycle != RuntimeGcCycle::Shared) {
        ++header->gc_age;
        header->generation = ObjectGeneration::Mature;
        ++result.promoted;
      }
    }

    cleanup_remembered_set_locked();
    result.remembered_entries = remembered_entry_count_locked();
    stats_.gc_marked_objects += result.marked;
    stats_.gc_reclaimed_objects += result.reclaimed;
    stats_.gc_promoted_objects += result.promoted;
    return result;
  }

  void request_garbage_collection(RuntimeGcCycle cycle) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_gc_cycle_ = cycle;
    gc_request_pending_.store(true, std::memory_order_release);
    ++stats_.gc_requested;
  }

  std::optional<RuntimeGcCycle> pending_gc_request() const {
    if (!gc_request_pending_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_gc_cycle_;
  }

  RuntimePinResult pin(const Value &value, RuntimePinViewKind view_kind,
                       RuntimePinPermission permissions) {
    RuntimePinResult out;
    ObjHeader *header = mutable_heap_header_from_value(value);
    if (!value_has_heap_payload_tag(value) || header == nullptr) {
      out.ok = false;
      out.error_name = "TypeError";
      out.message = "pin expects a heap object";
      return out;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::optional<std::string> lifecycle_error =
        lifecycle_access_error_name(*header);
    if (lifecycle_error.has_value()) {
      out.ok = false;
      out.error_name = *lifecycle_error;
      out.message = lifecycle_access_error_message(*lifecycle_error);
      return out;
    }
    const auto object = objects_.find(header->allocation_id);
    if (header->allocation_id == 0 || object == objects_.end() ||
        !object->second.logical_live) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "pin expects a live heap-owned object";
      return out;
    }
    if (view_kind == RuntimePinViewKind::ValueBuffer && !value.is_list() &&
        !value.is_tuple()) {
      out.ok = false;
      out.error_name = "TypeError";
      out.message = "buffer pin expects list or tuple storage";
      return out;
    }

    const std::uint64_t pin_id = next_pin_id_++;
    const std::uint64_t pin_epoch = next_pin_epoch_++;
    RuntimePinToken token;
    token.pin_id = pin_id;
    token.pin_epoch = pin_epoch;
    token.allocation_id = header->allocation_id;
    token.view_kind = view_kind;
    token.permissions = permissions;
    token.owner = header->owner;
    token.generation = header->generation;
    token.active = true;

    pins_[pin_id] =
        PinRecord{pin_id,      pin_epoch,     header->allocation_id, view_kind,
                  permissions, header->owner, header->generation,    value,
                  true};
    active_pins_by_object_[header->allocation_id].insert(pin_id);
    active_pin_counts_[header->allocation_id] += 1;
    header->pin_count += 1;
    header->pin_epoch = pin_epoch;
    header->flags |= kObjectFlagPinned;
    ++stats_.active_pins;
    ++stats_.pin_tokens_created;
    out.token = token;
    return out;
  }

  RuntimeUnpinResult unpin(RuntimePinToken *token) {
    RuntimeUnpinResult out;
    if (token == nullptr || token->pin_id == 0 || !token->active) {
      out.stale = true;
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.pin_stale_unpins;
      return out;
    }

    Value deferred_release = Value::null();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto found = pins_.find(token->pin_id);
      if (found == pins_.end() || !found->second.active ||
          found->second.pin_epoch != token->pin_epoch ||
          found->second.allocation_id != token->allocation_id) {
        out.stale = true;
        ++stats_.pin_stale_unpins;
        token->active = false;
        return out;
      }

      PinRecord &pin = found->second;
      ObjHeader *header = mutable_heap_header_from_value(pin.value);
      if (header != nullptr && header->allocation_id == pin.allocation_id) {
        if (header->pin_count > 0) {
          header->pin_count -= 1;
        }
        if (header->pin_count == 0) {
          header->flags &= ~kObjectFlagPinned;
        }
      }

      auto active_count = active_pin_counts_.find(pin.allocation_id);
      if (active_count != active_pin_counts_.end()) {
        if (active_count->second <= 1) {
          active_pin_counts_.erase(active_count);
        } else {
          active_count->second -= 1;
        }
      }
      auto active_set = active_pins_by_object_.find(pin.allocation_id);
      if (active_set != active_pins_by_object_.end()) {
        active_set->second.erase(pin.pin_id);
        if (active_set->second.empty()) {
          active_pins_by_object_.erase(active_set);
        }
      }
      pin.active = false;
      deferred_release = std::move(pin.value);
      pin.value = Value::null();
      if (stats_.active_pins > 0) {
        --stats_.active_pins;
      }
      ++stats_.pin_unpins;
      token->active = false;
      out.unpinned = true;
    }
    (void)deferred_release;
    return out;
  }

  std::uint64_t pin_count(const Value &value) const {
    const ObjHeader *header = heap_header_from_value(value);
    if (header == nullptr || header->allocation_id == 0) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = active_pin_counts_.find(header->allocation_id);
    return found == active_pin_counts_.end() ? 0 : found->second;
  }

  bool is_pinned(const Value &value) const { return pin_count(value) > 0; }

  RuntimeOpaqueHandleResult opaque_handle_for(const RuntimePinToken &token) {
    RuntimeOpaqueHandleResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    PinRecord *pin =
        active_pin_for_token_locked(token, &out.error_name, &out.message);
    if (pin == nullptr) {
      out.ok = false;
      return out;
    }
    if (pin->view_kind != RuntimePinViewKind::Opaque) {
      out.ok = false;
      out.error_name = "TypeError";
      out.message = "opaque handle requires an opaque pin";
      return out;
    }
    const ObjHeader *header = heap_header_from_value(pin->value);
    if (header == nullptr) {
      out.ok = false;
      out.error_name = "UseAfterFreeError";
      out.message = "pinned object is not available";
      return out;
    }
    const std::uint64_t handle_id = next_opaque_handle_id_++;
    RuntimeOpaqueHandle handle;
    handle.handle_id = handle_id;
    handle.pin_id = token.pin_id;
    handle.pin_epoch = token.pin_epoch;
    handle.allocation_id = token.allocation_id;
    handle.kind = header->kind;
    handle.active = true;
    opaque_handles_[handle_id] =
        OpaqueHandleRecord{handle_id,           token.pin_id, token.pin_epoch,
                           token.allocation_id, header->kind, true};
    ++stats_.opaque_handles_created;
    ++stats_.active_opaque_handles;
    out.handle = handle;
    return out;
  }

  RuntimeOpaqueHandleResult release_opaque_handle(RuntimeOpaqueHandle *handle) {
    RuntimeOpaqueHandleResult out;
    if (handle == nullptr || handle->handle_id == 0 || !handle->active) {
      out.ok = true;
      out.released = false;
      out.error_name = "LifetimeError";
      out.message = "opaque handle is stale";
      return out;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = opaque_handles_.find(handle->handle_id);
    if (found == opaque_handles_.end() || !found->second.active ||
        found->second.pin_epoch != handle->pin_epoch) {
      handle->active = false;
      out.ok = true;
      out.released = false;
      out.error_name = "LifetimeError";
      out.message = "opaque handle is stale";
      return out;
    }
    found->second.active = false;
    handle->active = false;
    if (stats_.active_opaque_handles > 0) {
      --stats_.active_opaque_handles;
    }
    out.released = true;
    return out;
  }

  RuntimeOpaqueHandleResult
  resolve_opaque_handle(const RuntimeOpaqueHandle &handle) const {
    RuntimeOpaqueHandleResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto handle_record = opaque_handles_.find(handle.handle_id);
    if (handle.handle_id == 0 || !handle.active ||
        handle_record == opaque_handles_.end() ||
        !handle_record->second.active ||
        handle_record->second.pin_epoch != handle.pin_epoch) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "opaque handle is stale";
      return out;
    }
    const auto pin = pins_.find(handle.pin_id);
    if (pin == pins_.end() || !pin->second.active ||
        pin->second.pin_epoch != handle.pin_epoch ||
        pin->second.allocation_id != handle.allocation_id) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "opaque handle pin is not active";
      return out;
    }
    out.value = pin->second.value;
    out.handle = handle;
    return out;
  }

  RuntimeValueBufferViewResult value_buffer_view(const RuntimePinToken &token) {
    RuntimeValueBufferViewResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    PinRecord *pin =
        active_pin_for_token_locked(token, &out.error_name, &out.message);
    if (pin == nullptr) {
      out.ok = false;
      return out;
    }
    if (pin->view_kind != RuntimePinViewKind::ValueBuffer) {
      out.ok = false;
      out.error_name = "TypeError";
      out.message = "value buffer view requires a buffer pin";
      return out;
    }

    RuntimeValueBufferView view;
    view.pin_id = token.pin_id;
    view.read_only = token.permissions == RuntimePinPermission::ReadOnly;
    view.active = true;
    if (pin->value.is_list()) {
      const IntrusivePtr<ListValue> list = pin->value.as_list();
      if (list == nullptr ||
          lifecycle_access_error_name(list->header).has_value()) {
        out.ok = false;
        out.error_name = "UseAfterFreeError";
        out.message = "pinned list buffer is not live";
        return out;
      }
      view.data = list->items.empty() ? nullptr : list->items.data();
      view.size = list->items.size();
    } else if (pin->value.is_tuple()) {
      const IntrusivePtr<TupleValue> tuple = pin->value.as_tuple();
      if (tuple == nullptr ||
          lifecycle_access_error_name(tuple->header).has_value()) {
        out.ok = false;
        out.error_name = "UseAfterFreeError";
        out.message = "pinned tuple buffer is not live";
        return out;
      }
      view.data = tuple->items.empty() ? nullptr : tuple->items.data();
      view.size = tuple->items.size();
    } else {
      out.ok = false;
      out.error_name = "TypeError";
      out.message = "buffer pin target has no contiguous value storage";
      return out;
    }
    ++stats_.buffer_views_created;
    out.view = view;
    return out;
  }

  RuntimeNativeWaitResult register_native_wait(const RuntimePinToken &token) {
    RuntimeNativeWaitResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    PinRecord *pin =
        active_pin_for_token_locked(token, &out.error_name, &out.message);
    if (pin == nullptr) {
      out.ok = false;
      return out;
    }
    (void)pin;
    const std::uint64_t wait_id = next_native_wait_id_++;
    RuntimeNativeWaitHandle handle;
    handle.wait_id = wait_id;
    handle.pin_id = token.pin_id;
    handle.pin_epoch = token.pin_epoch;
    handle.active = true;
    native_waits_[wait_id] =
        NativeWaitRecord{wait_id, token.pin_id, token.pin_epoch, true, false};
    ++stats_.native_waits_created;
    out.handle = handle;
    return out;
  }

  RuntimeNativeWaitResult cancel_native_wait(RuntimeNativeWaitHandle *handle) {
    RuntimeNativeWaitResult out;
    if (handle == nullptr || handle->wait_id == 0 || !handle->active) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait handle is stale";
      return out;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = native_waits_.find(handle->wait_id);
    if (found == native_waits_.end() || !found->second.active ||
        found->second.pin_epoch != handle->pin_epoch) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait handle is stale";
      return out;
    }
    found->second.cancellation_requested = true;
    handle->cancellation_requested = true;
    ++stats_.native_wait_cancellations;
    out.cancelled = true;
    out.handle = *handle;
    return out;
  }

  RuntimeNativeWaitResult
  poll_native_wait(const RuntimeNativeWaitHandle &handle) const {
    RuntimeNativeWaitResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto wait = native_waits_.find(handle.wait_id);
    if (handle.wait_id == 0 || !handle.active || wait == native_waits_.end() ||
        !wait->second.active || wait->second.pin_epoch != handle.pin_epoch) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait handle is stale";
      return out;
    }
    const auto pin = pins_.find(handle.pin_id);
    if (pin == pins_.end() || !pin->second.active ||
        pin->second.pin_epoch != handle.pin_epoch) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait pin is not active";
      return out;
    }
    out.cancelled = wait->second.cancellation_requested;
    out.handle = handle;
    out.handle.cancellation_requested = out.cancelled;
    return out;
  }

  RuntimeNativeWaitResult finish_native_wait(RuntimeNativeWaitHandle *handle) {
    RuntimeNativeWaitResult out;
    if (handle == nullptr || handle->wait_id == 0 || !handle->active) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait handle is stale";
      return out;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = native_waits_.find(handle->wait_id);
    if (found == native_waits_.end() || !found->second.active ||
        found->second.pin_epoch != handle->pin_epoch) {
      handle->active = false;
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait handle is stale";
      return out;
    }
    found->second.active = false;
    handle->active = false;
    out.finished = true;
    out.cancelled = found->second.cancellation_requested;
    out.handle = *handle;
    return out;
  }

  RuntimeHeapStats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeHeapStats out = stats_;
    out.arenas.clear();
    out.worker_count = static_cast<std::uint64_t>(arenas_.size());
    out.young_objects = 0;
    out.mature_objects = 0;
    out.shared_objects = 0;
    out.live_object_bytes = 0;
    out.tracked_object_bytes = 0;
    for (const auto &[allocation_id, record] : objects_) {
      (void)allocation_id;
      out.tracked_object_bytes += record.allocation_size;
      if (!record.logical_live) {
        continue;
      }
      out.live_object_bytes += record.allocation_size;
      const ObjHeader *header = header_for_record(record);
      if (header == nullptr) {
        continue;
      }
      switch (header->generation) {
      case ObjectGeneration::Young:
        ++out.young_objects;
        break;
      case ObjectGeneration::Mature:
        ++out.mature_objects;
        break;
      case ObjectGeneration::Shared:
        ++out.shared_objects;
        break;
      }
    }
    out.remembered_set_objects =
        static_cast<std::uint64_t>(remembered_set_.size());
    out.remembered_set_entries = remembered_entry_count_locked();
    out.pinned_objects = static_cast<std::uint64_t>(active_pin_counts_.size());
    for (const auto &[worker_id, arena] : arenas_) {
      out.arenas.push_back(RuntimeArenaStats{
          worker_id, arena.allocations, arena.live_objects,
          static_cast<std::uint64_t>(arena.remote_frees.size())});
    }
    return out;
  }

private:
  PinRecord *active_pin_for_token_locked(const RuntimePinToken &token,
                                         std::string *error_name,
                                         std::string *message) {
    if (token.pin_id == 0 || !token.active) {
      *error_name = "LifetimeError";
      *message = "pin token is stale";
      return nullptr;
    }
    auto found = pins_.find(token.pin_id);
    if (found == pins_.end() || !found->second.active ||
        found->second.pin_epoch != token.pin_epoch ||
        found->second.allocation_id != token.allocation_id) {
      *error_name = "LifetimeError";
      *message = "pin token is stale";
      return nullptr;
    }
    return &found->second;
  }

  template <typename T> static void destroy(void *ptr) {
    delete static_cast<T *>(ptr);
  }

  static ObjHeader *header_for_record(const ObjectRecord &record) {
    switch (record.kind) {
    case HeapObjectKind::Instance:
      return &static_cast<InstanceValue *>(record.ptr)->header;
    case HeapObjectKind::List:
      return &static_cast<ListValue *>(record.ptr)->header;
    case HeapObjectKind::Tuple:
      return &static_cast<TupleValue *>(record.ptr)->header;
    case HeapObjectKind::Set:
      return &static_cast<SetValue *>(record.ptr)->header;
    case HeapObjectKind::Map:
      return &static_cast<MapValue *>(record.ptr)->header;
    case HeapObjectKind::Closure:
      return &static_cast<ClosureValue *>(record.ptr)->header;
    }
    return nullptr;
  }

  ArenaState &arena_for_worker(std::uint64_t worker_id) {
    return arenas_[worker_id];
  }

  std::uint64_t reserve_allocation_id() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_allocation_id_++;
  }

  void record_allocation(std::uint64_t worker_id, HeapObjectKind kind,
                         std::size_t allocation_size,
                         std::uint64_t allocation_id, void *ptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    ArenaState &arena = arena_for_worker(worker_id);
    ++arena.allocations;
    ++arena.live_objects;
    ++stats_.allocations;
    ++stats_.live_objects;
    increment_kind_allocation(stats_, kind);
    objects_[allocation_id] = ObjectRecord{
        ptr, kind, worker_id, allocation_id, allocation_size, true};
  }

  void release(RemoteFree entry) {
    if (entry.ptr == nullptr || entry.destroy == nullptr) {
      return;
    }

    if (current_runtime_worker_id() == entry.owner_worker_id) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ArenaState &arena = arena_for_worker(entry.owner_worker_id);
        ++stats_.local_frees;
        const auto record = objects_.find(entry.allocation_id);
        if (record == objects_.end() || record->second.logical_live) {
          decrement_live_locked(arena, 1);
        }
        objects_.erase(entry.allocation_id);
        remove_remembered_edges_for_locked(entry.allocation_id);
      }
      entry.destroy(entry.ptr);
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ArenaState &arena = arena_for_worker(entry.owner_worker_id);
    arena.remote_frees.push_back(entry);
    remote_free_pending_.fetch_add(1, std::memory_order_release);
    ++stats_.remote_frees_queued;
    ++stats_.remote_queue_depth;
  }

  bool cycle_collects_header(RuntimeGcCycle cycle,
                             const ObjHeader &header) const {
    switch (cycle) {
    case RuntimeGcCycle::Young:
      return header.generation == ObjectGeneration::Young;
    case RuntimeGcCycle::Full:
      return true;
    case RuntimeGcCycle::Shared:
      return header.generation == ObjectGeneration::Shared;
    }
    return true;
  }

  void append_child_values(const ObjectRecord &record,
                           std::vector<Value> *out) const {
    const ObjHeader *header = header_for_record(record);
    if (header == nullptr || header_is_deallocated(*header)) {
      return;
    }
    switch (record.kind) {
    case HeapObjectKind::Instance: {
      const auto *instance = static_cast<const InstanceValue *>(record.ptr);
      out->insert(out->end(), instance->ivar_storage.begin(),
                  instance->ivar_storage.end());
      for (const auto &[name, value] : instance->ivars) {
        (void)name;
        out->push_back(value);
      }
      return;
    }
    case HeapObjectKind::List: {
      const auto *list = static_cast<const ListValue *>(record.ptr);
      out->insert(out->end(), list->items.begin(), list->items.end());
      return;
    }
    case HeapObjectKind::Tuple: {
      const auto *tuple = static_cast<const TupleValue *>(record.ptr);
      out->insert(out->end(), tuple->items.begin(), tuple->items.end());
      return;
    }
    case HeapObjectKind::Set: {
      const auto *set = static_cast<const SetValue *>(record.ptr);
      out->insert(out->end(), set->items.begin(), set->items.end());
      return;
    }
    case HeapObjectKind::Map: {
      const auto *map = static_cast<const MapValue *>(record.ptr);
      for (const MapEntry &entry : map->entries) {
        out->push_back(entry.key);
        out->push_back(entry.value);
      }
      return;
    }
    case HeapObjectKind::Closure: {
      const auto *closure = static_cast<const ClosureValue *>(record.ptr);
      out->insert(out->end(), closure->captures.begin(),
                  closure->captures.end());
      out->push_back(closure->self);
      return;
    }
    }
  }

  std::shared_ptr<ShapeDescriptor> dead_shape_locked() {
    if (dead_shape_ == nullptr) {
      dead_shape_ = std::make_shared<ShapeDescriptor>();
      dead_shape_->dead = true;
    }
    return dead_shape_;
  }

  void move_values_for_deferred_release(std::vector<Value> *from,
                                        std::vector<Value> *deferred) {
    deferred->insert(deferred->end(), std::make_move_iterator(from->begin()),
                     std::make_move_iterator(from->end()));
    from->clear();
    from->shrink_to_fit();
  }

  void clear_payload_for_gc_locked(ObjectRecord &record,
                                   std::vector<Value> *deferred) {
    const std::shared_ptr<ShapeDescriptor> dead_shape = dead_shape_locked();
    switch (record.kind) {
    case HeapObjectKind::Instance: {
      auto *instance = static_cast<InstanceValue *>(record.ptr);
      move_values_for_deferred_release(&instance->ivar_storage, deferred);
      for (auto &[name, value] : instance->ivars) {
        (void)name;
        deferred->push_back(std::move(value));
      }
      instance->ivars.clear();
      instance->ivar_shape_version = 0;
      instance->header.shape = dead_shape;
      return;
    }
    case HeapObjectKind::List: {
      auto *list = static_cast<ListValue *>(record.ptr);
      move_values_for_deferred_release(&list->items, deferred);
      list->frozen = false;
      list->header.shape = dead_shape;
      return;
    }
    case HeapObjectKind::Tuple: {
      auto *tuple = static_cast<TupleValue *>(record.ptr);
      move_values_for_deferred_release(&tuple->items, deferred);
      tuple->header.shape = dead_shape;
      return;
    }
    case HeapObjectKind::Set: {
      auto *set = static_cast<SetValue *>(record.ptr);
      move_values_for_deferred_release(&set->items, deferred);
      set->frozen = false;
      set->header.shape = dead_shape;
      return;
    }
    case HeapObjectKind::Map: {
      auto *map = static_cast<MapValue *>(record.ptr);
      for (MapEntry &entry : map->entries) {
        deferred->push_back(std::move(entry.key));
        deferred->push_back(std::move(entry.value));
      }
      map->entries.clear();
      map->entries.shrink_to_fit();
      map->frozen = false;
      map->header.shape = dead_shape;
      return;
    }
    case HeapObjectKind::Closure: {
      auto *closure = static_cast<ClosureValue *>(record.ptr);
      move_values_for_deferred_release(&closure->captures, deferred);
      deferred->push_back(std::move(closure->self));
      closure->self = Value::null();
      closure->code_id = 0;
      closure->header.shape = dead_shape;
      return;
    }
    }
  }

  void reclaim_record_locked(ObjectRecord &record,
                             std::vector<Value> *deferred_payload_release) {
    ObjHeader *header = header_for_record(record);
    if (header != nullptr) {
      clear_payload_for_gc_locked(record, deferred_payload_release);
      header->flags &= ~kObjectFlagDestroying;
      header->flags |= kObjectFlagDestroyed | kObjectFlagDead;
      header->lifetime_state = ObjectLifetimeState::Deallocated;
    }
    if (record.logical_live) {
      ArenaState &arena = arena_for_worker(record.owner_worker_id);
      decrement_live_locked(arena, 1);
    }
    record.logical_live = false;
    remove_remembered_edges_for_locked(record.allocation_id);
  }

  void remove_remembered_edges_for_locked(std::uint64_t allocation_id) {
    remembered_set_.erase(allocation_id);
    for (auto it = remembered_set_.begin(); it != remembered_set_.end();) {
      it->second.erase(allocation_id);
      if (it->second.empty()) {
        it = remembered_set_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void cleanup_remembered_set_locked() {
    for (auto it = remembered_set_.begin(); it != remembered_set_.end();) {
      const auto owner = objects_.find(it->first);
      if (owner == objects_.end() || !owner->second.logical_live) {
        it = remembered_set_.erase(it);
        continue;
      }
      const ObjHeader *owner_header = header_for_record(owner->second);
      if (owner_header == nullptr ||
          owner_header->generation != ObjectGeneration::Mature) {
        it = remembered_set_.erase(it);
        continue;
      }
      for (auto child = it->second.begin(); child != it->second.end();) {
        const auto child_record = objects_.find(*child);
        const ObjHeader *child_header =
            child_record == objects_.end()
                ? nullptr
                : header_for_record(child_record->second);
        if (child_record == objects_.end() ||
            !child_record->second.logical_live || child_header == nullptr ||
            child_header->generation != ObjectGeneration::Young) {
          child = it->second.erase(child);
        } else {
          ++child;
        }
      }
      if (it->second.empty()) {
        it = remembered_set_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::uint64_t remembered_entry_count_locked() const {
    std::uint64_t count = 0;
    for (const auto &[owner, children] : remembered_set_) {
      (void)owner;
      count += static_cast<std::uint64_t>(children.size());
    }
    return count;
  }

  void decrement_live_locked(ArenaState &arena, std::uint64_t count) {
    if (arena.live_objects >= count) {
      arena.live_objects -= count;
    } else {
      arena.live_objects = 0;
    }
    if (stats_.live_objects >= count) {
      stats_.live_objects -= count;
    } else {
      stats_.live_objects = 0;
    }
  }

  void drain_all_remote_frees() {
    while (true) {
      std::vector<std::pair<std::uint64_t, std::deque<RemoteFree>>> batches;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &[worker_id, arena] : arenas_) {
          if (arena.remote_frees.empty()) {
            continue;
          }
          std::deque<RemoteFree> pending;
          pending.swap(arena.remote_frees);
          const std::uint64_t count =
              static_cast<std::uint64_t>(pending.size());
          remote_free_pending_.fetch_sub(count, std::memory_order_acq_rel);
          stats_.remote_queue_depth -= count;
          stats_.remote_frees_drained += count;
          for (const RemoteFree &entry : pending) {
            const auto record = objects_.find(entry.allocation_id);
            if (record == objects_.end() || record->second.logical_live) {
              decrement_live_locked(arena, 1);
            }
            objects_.erase(entry.allocation_id);
            remove_remembered_edges_for_locked(entry.allocation_id);
          }
          batches.push_back({worker_id, std::move(pending)});
        }
      }
      if (batches.empty()) {
        return;
      }
      for (const auto &batch : batches) {
        RuntimeWorkerScope owner_scope(batch.first);
        for (const RemoteFree &entry : batch.second) {
          entry.destroy(entry.ptr);
        }
      }
    }
  }

  mutable std::mutex mutex_;
  std::uint64_t next_allocation_id_ = 1;
  std::uint64_t gc_epoch_ = 0;
  std::uint64_t next_pin_id_ = 1;
  std::uint64_t next_pin_epoch_ = 1;
  std::uint64_t next_opaque_handle_id_ = 1;
  std::uint64_t next_native_wait_id_ = 1;
  RuntimeHeapStats stats_;
  std::map<std::uint64_t, ArenaState> arenas_;
  std::unordered_map<std::uint64_t, ObjectRecord> objects_;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>>
      remembered_set_;
  std::unordered_map<std::uint64_t, PinRecord> pins_;
  std::unordered_map<std::uint64_t, std::uint64_t> active_pin_counts_;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>>
      active_pins_by_object_;
  std::unordered_map<std::uint64_t, OpaqueHandleRecord> opaque_handles_;
  std::unordered_map<std::uint64_t, NativeWaitRecord> native_waits_;
  std::shared_ptr<ShapeDescriptor> dead_shape_;
  std::optional<RuntimeGcCycle> pending_gc_cycle_;
  // Lock-free mirrors of the queue depth and pending-GC flag so per-opcode
  // safepoints skip the heap mutex when there is nothing to do.
  std::atomic<std::uint64_t> remote_free_pending_{0};
  std::atomic<bool> gc_request_pending_{false};
};

RuntimeHeap::RuntimeHeap() : impl_(std::make_shared<Impl>()) {}

RuntimeHeap::~RuntimeHeap() = default;

IntrusivePtr<InstanceValue>
RuntimeHeap::make_instance_value(std::uint32_t class_index) {
  return impl_->allocate<InstanceValue>(
      HeapObjectKind::Instance, [class_index](InstanceValue &value) {
        value.class_index = class_index;
        value.header.class_index = class_index;
      });
}

IntrusivePtr<ClosureValue> RuntimeHeap::make_closure_value() {
  return impl_->allocate<ClosureValue>(HeapObjectKind::Closure);
}

Value RuntimeHeap::make_list_value(std::vector<Value> items, bool frozen) {
  auto value = impl_->allocate<ListValue>(
      HeapObjectKind::List, [frozen, &items](ListValue &value) {
        value.header.flags =
            frozen ? kObjectFlagFrozen | kObjectFlagShareable : 0U;
        value.header.owner.kind =
            frozen ? OwnerTokenKind::Shareable : OwnerTokenKind::Confined;
        value.header.generation =
            frozen ? ObjectGeneration::Shared : ObjectGeneration::Young;
        value.items = std::move(items);
        value.frozen = frozen;
      });
  return Value::list(std::move(value));
}

Value RuntimeHeap::make_tuple_value(std::vector<Value> items) {
  auto value = impl_->allocate<TupleValue>(
      HeapObjectKind::Tuple, [&items](TupleValue &value) {
        value.header.flags = kObjectFlagFrozen | kObjectFlagShareable;
        value.header.owner.kind = OwnerTokenKind::Shareable;
        value.header.generation = ObjectGeneration::Shared;
        value.items = std::move(items);
      });
  return Value::tuple(std::move(value));
}

Value RuntimeHeap::make_set_value(std::vector<Value> items, bool frozen) {
  std::vector<Value> unique_items;
  unique_items.reserve(items.size());
  for (Value &item : items) {
    CollectionKeyError error;
    std::optional<Value> normalized = normalize_set_element(item, &error);
    if (!normalized.has_value()) {
      throw RuntimeTaskFailure(error.error_name, error.message);
    }
    const bool exists =
        std::find_if(unique_items.begin(), unique_items.end(),
                     [&](const Value &seen) {
                       return collection_keys_equal(seen, *normalized);
                     }) != unique_items.end();
    if (!exists) {
      unique_items.push_back(std::move(*normalized));
    }
  }

  auto value = impl_->allocate<SetValue>(
      HeapObjectKind::Set, [frozen, &unique_items](SetValue &value) {
        value.header.flags =
            frozen ? kObjectFlagFrozen | kObjectFlagShareable : 0U;
        value.header.owner.kind =
            frozen ? OwnerTokenKind::Shareable : OwnerTokenKind::Confined;
        value.header.generation =
            frozen ? ObjectGeneration::Shared : ObjectGeneration::Young;
        value.items = std::move(unique_items);
        value.frozen = frozen;
      });
  return Value::set(std::move(value));
}

Value RuntimeHeap::make_symbol_map_value(std::vector<MapEntry> entries,
                                         bool frozen, bool strict) {
  CollectionKeyError error;
  std::optional<std::vector<MapEntry>> normalized =
      normalize_map_entries(std::move(entries), strict, &error);
  if (!normalized.has_value()) {
    throw RuntimeTaskFailure(error.error_name, error.message);
  }
  auto value = impl_->allocate<MapValue>(
      HeapObjectKind::Map, [frozen, strict, &normalized](MapValue &value) {
        value.header.flags =
            frozen ? kObjectFlagFrozen | kObjectFlagShareable : 0U;
        value.header.owner.kind =
            frozen ? OwnerTokenKind::Shareable : OwnerTokenKind::Confined;
        value.header.generation =
            frozen ? ObjectGeneration::Shared : ObjectGeneration::Young;
        value.entries = std::move(*normalized);
        value.frozen = frozen;
        value.strict = strict;
      });
  return Value::map(std::move(value));
}

std::uint64_t RuntimeHeap::drain_remote_frees() {
  return drain_remote_frees(current_runtime_worker_id());
}

std::uint64_t RuntimeHeap::drain_remote_frees(std::uint64_t worker_id) {
  return impl_->drain_remote_frees(worker_id);
}

RuntimeWriteBarrierResult RuntimeHeap::write_barrier(const Value &owner,
                                                     const Value &value) {
  return impl_->write_barrier(owner, value);
}

RuntimeGcResult RuntimeHeap::collect_garbage(const std::vector<Value> &roots,
                                             RuntimeGcCycle cycle,
                                             bool from_safepoint) {
  return impl_->collect_garbage(roots, cycle, from_safepoint);
}

void RuntimeHeap::request_garbage_collection(RuntimeGcCycle cycle) {
  impl_->request_garbage_collection(cycle);
}

std::optional<RuntimeGcCycle> RuntimeHeap::pending_gc_request() const {
  return impl_->pending_gc_request();
}

RuntimePinResult RuntimeHeap::pin(const Value &value,
                                  RuntimePinViewKind view_kind,
                                  RuntimePinPermission permissions) {
  return impl_->pin(value, view_kind, permissions);
}

RuntimeUnpinResult RuntimeHeap::unpin(RuntimePinToken *token) {
  return impl_->unpin(token);
}

std::uint64_t RuntimeHeap::pin_count(const Value &value) const {
  return impl_->pin_count(value);
}

bool RuntimeHeap::is_pinned(const Value &value) const {
  return impl_->is_pinned(value);
}

RuntimeOpaqueHandleResult
RuntimeHeap::opaque_handle_for(const RuntimePinToken &token) {
  return impl_->opaque_handle_for(token);
}

RuntimeOpaqueHandleResult
RuntimeHeap::release_opaque_handle(RuntimeOpaqueHandle *handle) {
  return impl_->release_opaque_handle(handle);
}

RuntimeOpaqueHandleResult
RuntimeHeap::resolve_opaque_handle(const RuntimeOpaqueHandle &handle) const {
  return impl_->resolve_opaque_handle(handle);
}

RuntimeValueBufferViewResult
RuntimeHeap::value_buffer_view(const RuntimePinToken &token) {
  return impl_->value_buffer_view(token);
}

RuntimeNativeWaitResult
RuntimeHeap::register_native_wait(const RuntimePinToken &token) {
  return impl_->register_native_wait(token);
}

RuntimeNativeWaitResult
RuntimeHeap::cancel_native_wait(RuntimeNativeWaitHandle *handle) {
  return impl_->cancel_native_wait(handle);
}

RuntimeNativeWaitResult
RuntimeHeap::poll_native_wait(const RuntimeNativeWaitHandle &handle) const {
  return impl_->poll_native_wait(handle);
}

RuntimeNativeWaitResult
RuntimeHeap::finish_native_wait(RuntimeNativeWaitHandle *handle) {
  return impl_->finish_native_wait(handle);
}

RuntimeHeapStats RuntimeHeap::stats() const { return impl_->stats(); }

// --- Intrusive refcount drop path (RESEARCH §7.2) ---------------------------
// drop_object is a RuntimeHeap member so it may name the private Impl; it casts
// header.heap back and runs the same free/queue path as the old deleter.
void RuntimeHeap::drop_object(void *obj, void (*deleter)(void *),
                              const ObjHeader &header) noexcept {
  auto *impl = static_cast<RuntimeHeap::Impl *>(header.heap.get());
  impl->release_intrusive(obj, deleter, header);
}

RuntimePinScope::RuntimePinScope(RuntimeHeap &heap, const Value &value,
                                 RuntimePinViewKind view_kind,
                                 RuntimePinPermission permissions)
    : heap_(&heap), result_(heap.pin(value, view_kind, permissions)) {}

RuntimePinScope::RuntimePinScope(RuntimePinScope &&other) noexcept
    : heap_(other.heap_), result_(other.result_) {
  other.heap_ = nullptr;
  other.result_.token.active = false;
}

RuntimePinScope &RuntimePinScope::operator=(RuntimePinScope &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  (void)unpin();
  heap_ = other.heap_;
  result_ = other.result_;
  other.heap_ = nullptr;
  other.result_.token.active = false;
  return *this;
}

RuntimePinScope::~RuntimePinScope() { (void)unpin(); }

bool RuntimePinScope::active() const {
  return heap_ != nullptr && result_.ok && result_.token.active;
}

const RuntimePinResult &RuntimePinScope::result() const { return result_; }

const RuntimePinToken &RuntimePinScope::token() const { return result_.token; }

RuntimeUnpinResult RuntimePinScope::unpin() {
  if (heap_ == nullptr || !result_.token.active) {
    RuntimeUnpinResult out;
    out.stale = true;
    return out;
  }
  RuntimeUnpinResult out = heap_->unpin(&result_.token);
  if (out.unpinned || out.stale) {
    heap_ = nullptr;
  }
  return out;
}

namespace {
template <class T> void heap_object_deleter(void *ptr) noexcept {
  delete static_cast<T *>(ptr);
}
} // namespace

template <class T> void runtime_heap_add_ref(T *obj) noexcept {
  obj->header.ref_count.fetch_add(1, std::memory_order_relaxed);
}

template <class T> void runtime_heap_release(T *obj) noexcept {
  if (obj->header.ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    if (obj->header.heap == nullptr) {
      // Unmanaged object (make_intrusive): no RuntimeHeap, just delete it.
      delete obj;
      return;
    }
    // Hold a strong ref to the heap across the free so that, if this is the
    // last object, the heap's destructor runs here -- outside any Impl method
    // (whose mutex the free path locks) -- rather than mid-release. Mirrors how
    // the old deleter's captured shared_ptr<Impl> outlived the release() call.
    std::shared_ptr<void> keepalive = obj->header.heap;
    RuntimeHeap::drop_object(obj, &heap_object_deleter<T>, obj->header);
  }
}

template <class T> IntrusivePtr<T> make_intrusive() {
  T *obj = new T();
  obj->header.ref_count.store(1, std::memory_order_relaxed);
  return IntrusivePtr<T>(obj, typename IntrusivePtr<T>::Adopt{});
}

// IntrusivePtr<T> is only ever instantiated for the six ObjHeader-bearing
// kinds; these explicit instantiations satisfy the out-of-line declarations in
// value.h.
template void runtime_heap_add_ref<ClosureValue>(ClosureValue *) noexcept;
template void runtime_heap_add_ref<InstanceValue>(InstanceValue *) noexcept;
template void runtime_heap_add_ref<ListValue>(ListValue *) noexcept;
template void runtime_heap_add_ref<TupleValue>(TupleValue *) noexcept;
template void runtime_heap_add_ref<SetValue>(SetValue *) noexcept;
template void runtime_heap_add_ref<MapValue>(MapValue *) noexcept;
template void runtime_heap_release<ClosureValue>(ClosureValue *) noexcept;
template void runtime_heap_release<InstanceValue>(InstanceValue *) noexcept;
template void runtime_heap_release<ListValue>(ListValue *) noexcept;
template void runtime_heap_release<TupleValue>(TupleValue *) noexcept;
template void runtime_heap_release<SetValue>(SetValue *) noexcept;
template void runtime_heap_release<MapValue>(MapValue *) noexcept;
template IntrusivePtr<ClosureValue> make_intrusive<ClosureValue>();
template IntrusivePtr<InstanceValue> make_intrusive<InstanceValue>();
template IntrusivePtr<ListValue> make_intrusive<ListValue>();
template IntrusivePtr<TupleValue> make_intrusive<TupleValue>();
template IntrusivePtr<SetValue> make_intrusive<SetValue>();
template IntrusivePtr<MapValue> make_intrusive<MapValue>();

Value make_list_value(std::vector<Value> items, bool frozen) {
  return default_runtime_heap().make_list_value(std::move(items), frozen);
}

Value make_tuple_value(std::vector<Value> items) {
  return default_runtime_heap().make_tuple_value(std::move(items));
}

Value make_set_value(std::vector<Value> items, bool frozen) {
  return default_runtime_heap().make_set_value(std::move(items), frozen);
}

Value make_symbol_map_value(std::vector<MapEntry> entries, bool frozen,
                            bool strict) {
  return default_runtime_heap().make_symbol_map_value(std::move(entries),
                                                      frozen, strict);
}

} // namespace amber::runtime
