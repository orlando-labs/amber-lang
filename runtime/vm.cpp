#include "runtime/vm.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace amber::runtime {

namespace {

thread_local std::uint64_t tls_runtime_worker_id = 0;

void increment_kind_allocation(RuntimeHeapStats &stats, HeapObjectKind kind) {
  switch (kind) {
  case HeapObjectKind::Instance:
    ++stats.instance_allocations;
    return;
  case HeapObjectKind::List:
  case HeapObjectKind::Tuple:
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

bool value_has_heap_payload_tag(const Value &value);
const ObjHeader *heap_header_from_value(const Value &value);
ObjHeader *mutable_heap_header_from_value(const Value &value);
bool header_is_deallocated(const ObjHeader &header);
bool header_is_destroyed(const ObjHeader &header);
std::optional<std::string> lifecycle_access_error_name(const ObjHeader &header);
std::string lifecycle_access_error_message(const std::string &error_name);

} // namespace

std::uint64_t current_runtime_worker_id() { return tls_runtime_worker_id; }

RuntimeWorkerScope::RuntimeWorkerScope(std::uint64_t worker_id)
    : previous_worker_id_(tls_runtime_worker_id) {
  tls_runtime_worker_id = worker_id;
}

RuntimeWorkerScope::~RuntimeWorkerScope() {
  tls_runtime_worker_id = previous_worker_id_;
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

  ~Impl() { drain_all_remote_frees(); }

  template <typename T, typename Init>
  std::shared_ptr<T> allocate(HeapObjectKind kind, Init init) {
    auto *raw = new T();
    const std::uint64_t worker_id = current_runtime_worker_id();
    const std::size_t allocation_size = sizeof(T);
    const std::uint64_t allocation_id = reserve_allocation_id();
    raw->header.kind = kind;
    raw->header.owner.strand_id = worker_id;
    raw->header.allocation_id = allocation_id;
    raw->header.arena_worker_id = worker_id;
    raw->header.allocation_size = allocation_size;
    raw->header.generation = ObjectGeneration::Young;
    init(*raw);

    std::shared_ptr<Impl> impl = shared_from_this();
    std::shared_ptr<T> handle(
        raw, [impl, worker_id, kind, allocation_id](T *ptr) {
          impl->release({ptr, destroy<T>, worker_id, kind, allocation_id});
        });
    record_allocation(worker_id, kind, allocation_size, allocation_id, raw);
    return handle;
  }

  template <typename T> std::shared_ptr<T> allocate(HeapObjectKind kind) {
    return allocate<T>(kind, [](T &) {});
  }

  std::uint64_t drain_remote_frees(std::uint64_t worker_id) {
    std::deque<RemoteFree> pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ArenaState &arena = arena_for_worker(worker_id);
      pending.swap(arena.remote_frees);
      const std::uint64_t count = static_cast<std::uint64_t>(pending.size());
      if (count == 0) {
        return 0;
      }
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
    ++stats_.gc_requested;
  }

  std::optional<RuntimeGcCycle> pending_gc_request() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_gc_cycle_;
  }

  RuntimeHeapStats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeHeapStats out = stats_;
    out.arenas.clear();
    out.worker_count = static_cast<std::uint64_t>(arenas_.size());
    out.young_objects = 0;
    out.mature_objects = 0;
    out.shared_objects = 0;
    for (const auto &[allocation_id, record] : objects_) {
      (void)allocation_id;
      if (!record.logical_live) {
        continue;
      }
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
    for (const auto &[worker_id, arena] : arenas_) {
      out.arenas.push_back(RuntimeArenaStats{
          worker_id, arena.allocations, arena.live_objects,
          static_cast<std::uint64_t>(arena.remote_frees.size())});
    }
    return out;
  }

private:
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
    case HeapObjectKind::Map: {
      const auto *map = static_cast<const MapValue *>(record.ptr);
      for (const MapEntry &entry : map->entries) {
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
    case HeapObjectKind::Map: {
      auto *map = static_cast<MapValue *>(record.ptr);
      for (MapEntry &entry : map->entries) {
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
  RuntimeHeapStats stats_;
  std::map<std::uint64_t, ArenaState> arenas_;
  std::unordered_map<std::uint64_t, ObjectRecord> objects_;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>>
      remembered_set_;
  std::shared_ptr<ShapeDescriptor> dead_shape_;
  std::optional<RuntimeGcCycle> pending_gc_cycle_;
};

RuntimeHeap::RuntimeHeap() : impl_(std::make_shared<Impl>()) {}

RuntimeHeap::~RuntimeHeap() = default;

std::shared_ptr<InstanceValue>
RuntimeHeap::make_instance_value(std::uint32_t class_index) {
  return impl_->allocate<InstanceValue>(
      HeapObjectKind::Instance, [class_index](InstanceValue &value) {
        value.class_index = class_index;
        value.header.class_index = class_index;
      });
}

std::shared_ptr<ClosureValue> RuntimeHeap::make_closure_value() {
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
  return {std::move(value)};
}

Value RuntimeHeap::make_tuple_value(std::vector<Value> items) {
  auto value = impl_->allocate<TupleValue>(
      HeapObjectKind::Tuple, [&items](TupleValue &value) {
        value.header.flags = kObjectFlagFrozen | kObjectFlagShareable;
        value.header.owner.kind = OwnerTokenKind::Shareable;
        value.header.generation = ObjectGeneration::Shared;
        value.items = std::move(items);
      });
  return {std::move(value)};
}

Value RuntimeHeap::make_symbol_map_value(std::vector<MapEntry> entries,
                                         bool frozen) {
  auto value = impl_->allocate<MapValue>(
      HeapObjectKind::Map, [frozen, &entries](MapValue &value) {
        value.header.flags =
            frozen ? kObjectFlagFrozen | kObjectFlagShareable : 0U;
        value.header.owner.kind =
            frozen ? OwnerTokenKind::Shareable : OwnerTokenKind::Confined;
        value.header.generation =
            frozen ? ObjectGeneration::Shared : ObjectGeneration::Young;
        value.entries = std::move(entries);
        value.frozen = frozen;
      });
  return {std::move(value)};
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

RuntimeHeapStats RuntimeHeap::stats() const { return impl_->stats(); }

RuntimeHeap &default_runtime_heap() {
  static RuntimeHeap heap;
  return heap;
}

Value Value::null() { return {std::monostate{}}; }

Value Value::boolean(bool value) { return {value}; }

Value Value::integer(std::int64_t value) { return {value}; }

Value Value::floating(double value) { return {value}; }

Value Value::symbol(std::uint32_t symbol_id) {
  return {SymbolValue{symbol_id}};
}

Value Value::string(std::uint32_t string_id) {
  return {StringValue{string_id}};
}

Value Value::class_object(std::uint32_t class_index) {
  return {ClassObjectValue{class_index}};
}

Value Value::closure(std::shared_ptr<ClosureValue> value) {
  return {std::move(value)};
}

Value Value::instance(std::shared_ptr<InstanceValue> value) {
  return {std::move(value)};
}

bool Value::is_null() const {
  return std::holds_alternative<std::monostate>(payload);
}

bool Value::is_bool() const { return std::holds_alternative<bool>(payload); }

bool Value::is_integer() const {
  return std::holds_alternative<std::int64_t>(payload);
}

bool Value::is_float() const { return std::holds_alternative<double>(payload); }

bool Value::is_symbol() const {
  return std::holds_alternative<SymbolValue>(payload);
}

bool Value::is_string() const {
  return std::holds_alternative<StringValue>(payload);
}

bool Value::is_class_object() const {
  return std::holds_alternative<ClassObjectValue>(payload);
}

bool Value::is_closure() const {
  return std::holds_alternative<std::shared_ptr<ClosureValue>>(payload);
}

bool Value::is_instance_object() const {
  return std::holds_alternative<std::shared_ptr<InstanceValue>>(payload);
}

bool Value::is_list() const {
  return std::holds_alternative<std::shared_ptr<ListValue>>(payload);
}

bool Value::is_tuple() const {
  return std::holds_alternative<std::shared_ptr<TupleValue>>(payload);
}

bool Value::is_map() const {
  return std::holds_alternative<std::shared_ptr<MapValue>>(payload);
}

bool Value::as_bool() const { return std::get<bool>(payload); }

std::int64_t Value::as_integer() const {
  return std::get<std::int64_t>(payload);
}

double Value::as_float() const { return std::get<double>(payload); }

SymbolValue Value::as_symbol() const { return std::get<SymbolValue>(payload); }

StringValue Value::as_string() const { return std::get<StringValue>(payload); }

ClassObjectValue Value::as_class_object() const {
  return std::get<ClassObjectValue>(payload);
}

std::shared_ptr<ClosureValue> Value::as_closure() const {
  return std::get<std::shared_ptr<ClosureValue>>(payload);
}

std::shared_ptr<InstanceValue> Value::as_instance_object() const {
  return std::get<std::shared_ptr<InstanceValue>>(payload);
}

std::shared_ptr<ListValue> Value::as_list() const {
  return std::get<std::shared_ptr<ListValue>>(payload);
}

std::shared_ptr<TupleValue> Value::as_tuple() const {
  return std::get<std::shared_ptr<TupleValue>>(payload);
}

std::shared_ptr<MapValue> Value::as_map() const {
  return std::get<std::shared_ptr<MapValue>>(payload);
}

Value make_list_value(std::vector<Value> items, bool frozen) {
  return default_runtime_heap().make_list_value(std::move(items), frozen);
}

Value make_tuple_value(std::vector<Value> items) {
  return default_runtime_heap().make_tuple_value(std::move(items));
}

Value make_symbol_map_value(std::vector<MapEntry> entries, bool frozen) {
  return default_runtime_heap().make_symbol_map_value(std::move(entries),
                                                      frozen);
}

namespace {

using amber::bytecode::BcCode;
using amber::bytecode::BcModule;
using amber::bytecode::CodeKind;
using amber::bytecode::Constant;
using amber::bytecode::ConstantKind;
using amber::bytecode::Instruction;
using amber::bytecode::Opcode;

constexpr std::uint32_t kMethodFlagInstance = 1U;
constexpr std::uint32_t kMethodFlagClass = 2U;
constexpr std::int64_t kPatternFailModeSoft = 0;
constexpr std::int64_t kPatternFailModeMatchError = 1;

bool value_has_heap_payload_tag(const Value &value) {
  return value.is_closure() || value.is_instance_object() || value.is_list() ||
         value.is_tuple() || value.is_map();
}

const ObjHeader *heap_header_from_value(const Value &value) {
  if (value.is_closure()) {
    const std::shared_ptr<ClosureValue> object = value.as_closure();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_instance_object()) {
    const std::shared_ptr<InstanceValue> object = value.as_instance_object();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_list()) {
    const std::shared_ptr<ListValue> object = value.as_list();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_tuple()) {
    const std::shared_ptr<TupleValue> object = value.as_tuple();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_map()) {
    const std::shared_ptr<MapValue> object = value.as_map();
    return object == nullptr ? nullptr : &object->header;
  }
  return nullptr;
}

ObjHeader *mutable_heap_header_from_value(const Value &value) {
  if (value.is_closure()) {
    const std::shared_ptr<ClosureValue> object = value.as_closure();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_instance_object()) {
    const std::shared_ptr<InstanceValue> object = value.as_instance_object();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_list()) {
    const std::shared_ptr<ListValue> object = value.as_list();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_tuple()) {
    const std::shared_ptr<TupleValue> object = value.as_tuple();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_map()) {
    const std::shared_ptr<MapValue> object = value.as_map();
    return object == nullptr ? nullptr : &object->header;
  }
  return nullptr;
}

bool header_is_deallocated(const ObjHeader &header) {
  return header.lifetime_state == ObjectLifetimeState::Deallocated ||
         (header.flags & kObjectFlagDead) != 0U ||
         (header.shape != nullptr && header.shape->dead);
}

bool header_is_destroyed(const ObjHeader &header) {
  return header.lifetime_state == ObjectLifetimeState::Destroyed ||
         header.lifetime_state == ObjectLifetimeState::Destroying ||
         (header.flags & kObjectFlagDestroyed) != 0U ||
         (header.flags & kObjectFlagDestroying) != 0U;
}

std::optional<std::string>
lifecycle_access_error_name(const ObjHeader &header) {
  if (header_is_deallocated(header)) {
    return "UseAfterFreeError";
  }
  if (header_is_destroyed(header)) {
    return "DestroyedAccessError";
  }
  return std::nullopt;
}

std::string lifecycle_access_error_message(const std::string &error_name) {
  return error_name == "UseAfterFreeError" ? "access to deallocated object"
                                           : "access to destroyed object";
}

std::string lifecycle_debug_label(const ObjHeader &header) {
  if (header_is_deallocated(header)) {
    return "deallocated";
  }
  if (header_is_destroyed(header)) {
    return "destroyed";
  }
  return "";
}

struct PreparedSeqState {
  std::vector<Value> items;
  std::size_t rest_start = 0;
  bool source_was_tuple = false;
};

struct PreparedMapState {
  std::vector<MapEntry> entries;
  std::unordered_map<std::uint32_t, std::size_t> index_by_key;
  std::vector<std::uint32_t> requested_keys;
  bool needs_full = false;
  bool rest_bound = false;
  std::uint32_t fail_pc = 0;
};

struct CoercedSeqState {
  Value value = Value::null();
  std::vector<Value> items;
  bool source_was_tuple = false;
};

struct CoercedMapState {
  Value value = Value::null();
  std::vector<MapEntry> entries;
};

struct CallCacheEntry {
  bool valid = false;
  std::uint32_t receiver_class_index = 0;
  std::uint32_t dispatch_flags = 0;
  std::uint32_t selector_symbol_id = 0;
  std::uint64_t method_version = 0;
  std::uint64_t world_epoch = 0;
  bytecode::BcMethod method;
};

struct IvarCacheEntry {
  bool valid = false;
  std::uint32_t receiver_class_index = 0;
  std::uint32_t symbol_id = 0;
  std::uint64_t shape_id = 0;
  std::uint64_t shape_version = 0;
  std::uint32_t slot_index = 0;
};

struct Frame {
  const BcCode *code = nullptr;
  std::size_t pc = 0;
  std::vector<Value> regs;
  std::vector<Value> captures;
  Value self = Value::null();
  Value block = Value::null();
  Value last_result = Value::null();
  std::optional<std::uint32_t> caller_result_reg;
  std::optional<std::uint32_t> active_call_pc;
  std::optional<Value> return_override;
  std::unordered_map<std::uint32_t, PreparedSeqState> prepared_seq_regs;
  std::unordered_map<std::uint32_t, PreparedMapState> prepared_map_regs;
  std::unordered_map<std::uint32_t, Value> pending_pattern_bindings;
};

struct MethodTableDescriptor {
  std::unordered_map<std::uint32_t, bytecode::BcMethod> entries;
};

struct ClassRuntimeState {
  std::unordered_map<std::string, Value> cvars;
  MethodTableDescriptor instance_method_table;
  MethodTableDescriptor class_method_table;
  std::vector<std::uint32_t> direct_include_indices;
  std::vector<std::uint32_t> direct_extend_indices;
  std::uint32_t owner_flags = 0;
  std::uint32_t ivar_schema_id = 0;
  bool has_superclass_ref = false;
  std::uint32_t superclass_ref = 0;
  bool method_range_valid = true;
  std::uint64_t method_version = 1;
};

struct RuntimeState {
  RuntimeHeap heap;
  std::vector<ClassRuntimeState> classes;
  bool owners_initialized = false;
  std::uint64_t world_epoch = 1;
  std::unordered_map<std::uint64_t, CallCacheEntry> call_caches;
  std::unordered_map<std::uint64_t, IvarCacheEntry> ivar_caches;
  std::uint64_t next_shape_id = 1;
  std::vector<std::shared_ptr<ShapeDescriptor>> root_shapes;
  std::unordered_map<std::string, std::shared_ptr<ShapeDescriptor>>
      shape_transitions;
  std::shared_ptr<ShapeDescriptor> dead_shape;

  static std::string shape_transition_key(std::uint64_t parent_id,
                                          const std::string &name) {
    return std::to_string(parent_id) + "\x1f" + name;
  }

  void initialize_for_module(const BcModule &module) {
    if (dead_shape == nullptr) {
      dead_shape = std::make_shared<ShapeDescriptor>();
      dead_shape->shape_id = 0;
      dead_shape->shape_version = 0;
      dead_shape->dead = true;
    }
    if (classes.size() < module.classes.size()) {
      classes.resize(module.classes.size());
    }
    if (root_shapes.size() < module.classes.size()) {
      root_shapes.resize(module.classes.size());
    }
    if (owners_initialized) {
      return;
    }
    for (std::uint32_t index = 0; index < module.classes.size(); ++index) {
      ClassRuntimeState &runtime = classes[index];
      const bytecode::BcClass &owner = module.classes[index];
      runtime.owner_flags = owner.flags;
      runtime.ivar_schema_id = owner.ivar_schema_id;
      runtime.has_superclass_ref = owner.has_superclass_ref;
      runtime.superclass_ref = owner.superclass_ref;
      runtime.method_range_valid =
          owner.method_range_start + owner.method_range_count <=
          module.methods.size();
      if (!runtime.method_range_valid) {
        continue;
      }
      for (std::uint32_t offset = 0; offset < owner.method_range_count;
           ++offset) {
        bytecode::BcMethod method =
            module.methods[owner.method_range_start + offset];
        MethodTableDescriptor &table = method.flags == kMethodFlagClass
                                           ? runtime.class_method_table
                                           : runtime.instance_method_table;
        table.entries[method.selector_sym_id] = std::move(method);
      }
    }
    owners_initialized = true;
  }

  std::shared_ptr<const ShapeDescriptor>
  root_shape_for_class(std::uint32_t class_index) {
    if (root_shapes.size() <= class_index) {
      root_shapes.resize(static_cast<std::size_t>(class_index) + 1U);
    }
    if (root_shapes[class_index] == nullptr) {
      auto shape = std::make_shared<ShapeDescriptor>();
      shape->shape_id = next_shape_id++;
      shape->shape_version = shape->shape_id;
      root_shapes[class_index] = shape;
    }
    return root_shapes[class_index];
  }

  std::shared_ptr<const ShapeDescriptor>
  transition_shape(std::shared_ptr<const ShapeDescriptor> parent,
                   const std::string &name) {
    if (parent == nullptr || parent->dead) {
      return parent;
    }
    if (parent->ivar_slots.find(name) != parent->ivar_slots.end()) {
      return parent;
    }
    const std::string key = shape_transition_key(parent->shape_id, name);
    const auto found = shape_transitions.find(key);
    if (found != shape_transitions.end()) {
      return found->second;
    }
    auto next = std::make_shared<ShapeDescriptor>();
    next->shape_id = next_shape_id++;
    next->shape_version = next->shape_id;
    next->ivar_slots = parent->ivar_slots;
    next->slot_names = parent->slot_names;
    next->parent_shape = std::move(parent);
    next->ivar_slots[name] =
        static_cast<std::uint32_t>(next->slot_names.size());
    next->slot_names.push_back(name);
    shape_transitions[key] = next;
    return next;
  }

  void invalidate_dispatch_owner(std::uint32_t class_index) {
    if (class_index >= classes.size()) {
      return;
    }
    ++classes[class_index].method_version;
    ++world_epoch;
  }
};

const BcCode *find_code(const BcModule &module, std::uint32_t code_id) {
  for (const BcCode &code : module.code_objects) {
    if (code.code_id == code_id) {
      return &code;
    }
  }
  return nullptr;
}

bool is_truthy(const Value &value) {
  return !value.is_null() && !(value.is_bool() && !value.as_bool());
}

bool value_equals(const Value &lhs, const Value &rhs) {
  if (lhs.payload.index() != rhs.payload.index()) {
    return false;
  }
  if (lhs.is_null()) {
    return true;
  }
  if (lhs.is_bool()) {
    return lhs.as_bool() == rhs.as_bool();
  }
  if (lhs.is_integer()) {
    return lhs.as_integer() == rhs.as_integer();
  }
  if (lhs.is_float()) {
    return lhs.as_float() == rhs.as_float();
  }
  if (lhs.is_symbol()) {
    return lhs.as_symbol().symbol_id == rhs.as_symbol().symbol_id;
  }
  if (lhs.is_string()) {
    return lhs.as_string().string_id == rhs.as_string().string_id;
  }
  if (lhs.is_class_object()) {
    return lhs.as_class_object().class_index ==
           rhs.as_class_object().class_index;
  }
  if (lhs.is_closure()) {
    return lhs.as_closure() == rhs.as_closure();
  }
  if (lhs.is_instance_object()) {
    return lhs.as_instance_object() == rhs.as_instance_object();
  }
  if (lhs.is_list()) {
    const std::shared_ptr<ListValue> left = lhs.as_list();
    const std::shared_ptr<ListValue> right = rhs.as_list();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (left->items.size() != right->items.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left->items.size(); ++i) {
      if (!value_equals(left->items[i], right->items[i])) {
        return false;
      }
    }
    return true;
  }
  if (lhs.is_tuple()) {
    const std::shared_ptr<TupleValue> left = lhs.as_tuple();
    const std::shared_ptr<TupleValue> right = rhs.as_tuple();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (left->items.size() != right->items.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left->items.size(); ++i) {
      if (!value_equals(left->items[i], right->items[i])) {
        return false;
      }
    }
    return true;
  }
  if (lhs.is_map()) {
    const std::shared_ptr<MapValue> left = lhs.as_map();
    const std::shared_ptr<MapValue> right = rhs.as_map();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (left->entries.size() != right->entries.size()) {
      return false;
    }
    std::unordered_map<std::uint32_t, std::size_t> right_index;
    for (std::size_t i = 0; i < right->entries.size(); ++i) {
      right_index[right->entries[i].symbol_id] = i;
    }
    for (const MapEntry &entry : left->entries) {
      const auto right_it = right_index.find(entry.symbol_id);
      if (right_it == right_index.end()) {
        return false;
      }
      if (!value_equals(entry.value, right->entries[right_it->second].value)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

enum class SendStatus { Matched, NotHandled, Faulted };

struct BoundMethodArg {
  bool present = false;
  Value value = Value::null();
};

struct NestedExecution {
  Value value = Value::null();
  std::vector<Value> regs;
  std::optional<Fault> fault;

  bool ok() const { return !fault.has_value(); }
};

class Vm {
public:
  explicit Vm(const BcModule &module,
              std::shared_ptr<RuntimeState> state = nullptr)
      : module_(module),
        state_(state == nullptr ? std::make_shared<RuntimeState>()
                                : std::move(state)) {
    state_->initialize_for_module(module_);
  }

  ExecutionResult execute(std::uint32_t code_id, const std::vector<Value> &args,
                          Value self, Value block) {
    const BcCode *entry = find_code(module_, code_id);
    if (entry == nullptr) {
      return fail("VMError", "unknown code id", code_id, 0);
    }
    push_frame(*entry, args, {}, std::move(self), std::move(block),
               std::nullopt);
    while (fault_ == std::nullopt && !frames_.empty()) {
      step();
    }
    state_->heap.drain_remote_frees();
    if (fault_.has_value()) {
      return {Value::null(), fault_};
    }
    return {final_value_, std::nullopt};
  }

private:
  ExecutionResult fail(const std::string &error_name,
                       const std::string &message, std::uint32_t code_id,
                       std::uint32_t pc) {
    return {Value::null(), Fault{error_name, message, code_id, pc}};
  }

  bool ensure_lifecycle_access(const Frame &frame, const Value &value) {
    if (!value_has_heap_payload_tag(value)) {
      return true;
    }
    const ObjHeader *header = heap_header_from_value(value);
    if (header == nullptr) {
      set_fault(frame, "TypeError", "heap object reference is null");
      return false;
    }
    const std::optional<std::string> error_name =
        lifecycle_access_error_name(*header);
    if (error_name.has_value()) {
      set_fault(frame, *error_name,
                lifecycle_access_error_message(*error_name));
      return false;
    }
    return true;
  }

  enum class LifecycleTargetStatus { NonHeap, Heap };

  std::optional<LifecycleTargetStatus>
  lifecycle_target_header(const Frame &frame, const Value &value,
                          ObjHeader **out_header) {
    if (!value_has_heap_payload_tag(value)) {
      *out_header = nullptr;
      return LifecycleTargetStatus::NonHeap;
    }
    ObjHeader *header = mutable_heap_header_from_value(value);
    if (header == nullptr) {
      set_fault(frame, "TypeError", "heap object reference is null");
      return std::nullopt;
    }
    *out_header = header;
    return LifecycleTargetStatus::Heap;
  }

  bool check_lifecycle_preconditions(const Frame &frame,
                                     const ObjHeader &header) {
    if (header.owner.kind != OwnerTokenKind::Confined) {
      set_fault(frame, "LifetimeError",
                "explicit lifecycle operation requires confined object");
      return false;
    }
    if (header.owner.strand_id != current_runtime_worker_id()) {
      set_fault(frame, "IsolationError",
                "lifecycle operation must run on the owner strand");
      return false;
    }
    return true;
  }

  bool run_destroy_methods(Frame &frame, const Value &value) {
    if (!value.is_instance_object()) {
      return true;
    }
    const std::shared_ptr<InstanceValue> instance = value.as_instance_object();
    if (instance == nullptr ||
        instance->class_index >= module_.classes.size()) {
      return true;
    }

    std::vector<bytecode::BcMethod> methods;
    std::vector<bool> active(module_.classes.size(), false);
    std::uint32_t current = instance->class_index;
    while (current < module_.classes.size()) {
      if (active[current]) {
        set_fault(frame, "VMError", "cycle detected in destructor chain");
        return false;
      }
      active[current] = true;
      const bytecode::BcMethod *method = find_method_in_owner_table(
          frame, current, "destroy!", kMethodFlagInstance);
      if (fault_.has_value()) {
        return false;
      }
      if (method != nullptr) {
        methods.push_back(*method);
      }

      const bytecode::BcClass &klass = module_.classes[current];
      if (!klass.has_superclass_ref) {
        break;
      }
      if (!resolve_class_ref(frame, klass.superclass_ref, &current)) {
        return false;
      }
    }

    for (const bytecode::BcMethod &method : methods) {
      const std::vector<Value> args;
      const std::vector<std::pair<std::uint32_t, Value>> kw_args;
      const std::optional<Value> result = execute_method_to_value(
          frame, method, args, kw_args, value, Value::null());
      (void)result;
      if (fault_.has_value()) {
        return false;
      }
    }
    return true;
  }

  bool perform_destroy(Frame &frame, const Value &value, ObjHeader &header,
                       bool *changed) {
    *changed = false;
    if (header_is_deallocated(header) || header_is_destroyed(header)) {
      return true;
    }
    const bool destructors_ok = run_destroy_methods(frame, value);
    header.flags &= ~kObjectFlagDestroying;
    header.flags |= kObjectFlagDestroyed;
    header.lifetime_state = ObjectLifetimeState::Destroyed;
    *changed = true;
    return destructors_ok && !fault_.has_value();
  }

  bool lifecycle_destroy(Frame &frame, const Value &value, bool *changed) {
    ObjHeader *header = nullptr;
    const std::optional<LifecycleTargetStatus> target =
        lifecycle_target_header(frame, value, &header);
    if (!target.has_value()) {
      return false;
    }
    if (*target == LifecycleTargetStatus::NonHeap) {
      *changed = false;
      return true;
    }
    if (header_is_deallocated(*header) || header_is_destroyed(*header)) {
      *changed = false;
      return true;
    }
    if (!check_lifecycle_preconditions(frame, *header)) {
      return false;
    }
    return perform_destroy(frame, value, *header, changed);
  }

  void release_payload_for_dealloc(const Value &value) {
    if (value.is_instance_object()) {
      const std::shared_ptr<InstanceValue> instance =
          value.as_instance_object();
      if (instance == nullptr) {
        return;
      }
      instance->ivar_storage.clear();
      instance->ivar_storage.shrink_to_fit();
      instance->ivars.clear();
      instance->ivar_shape_version = 0;
      instance->header.shape = state_->dead_shape;
      return;
    }
    if (value.is_list()) {
      const std::shared_ptr<ListValue> list = value.as_list();
      if (list != nullptr) {
        list->items.clear();
        list->items.shrink_to_fit();
        list->frozen = false;
        list->header.shape = state_->dead_shape;
      }
      return;
    }
    if (value.is_tuple()) {
      const std::shared_ptr<TupleValue> tuple = value.as_tuple();
      if (tuple != nullptr) {
        tuple->items.clear();
        tuple->items.shrink_to_fit();
        tuple->header.shape = state_->dead_shape;
      }
      return;
    }
    if (value.is_map()) {
      const std::shared_ptr<MapValue> map = value.as_map();
      if (map != nullptr) {
        map->entries.clear();
        map->entries.shrink_to_fit();
        map->frozen = false;
        map->header.shape = state_->dead_shape;
      }
      return;
    }
    if (value.is_closure()) {
      const std::shared_ptr<ClosureValue> closure = value.as_closure();
      if (closure != nullptr) {
        closure->captures.clear();
        closure->captures.shrink_to_fit();
        closure->self = Value::null();
        closure->code_id = 0;
        closure->header.shape = state_->dead_shape;
      }
    }
  }

  bool lifecycle_dealloc(Frame &frame, const Value &value, bool *changed) {
    ObjHeader *header = nullptr;
    const std::optional<LifecycleTargetStatus> target =
        lifecycle_target_header(frame, value, &header);
    if (!target.has_value()) {
      return false;
    }
    if (*target == LifecycleTargetStatus::NonHeap) {
      *changed = false;
      return true;
    }
    if (header_is_deallocated(*header)) {
      *changed = false;
      return true;
    }
    if (header->lifetime_state == ObjectLifetimeState::Destroying ||
        (header->flags & kObjectFlagDestroying) != 0U) {
      set_fault(frame, "LifetimeError",
                "cannot deallocate object while destroy is active");
      return false;
    }
    if (!check_lifecycle_preconditions(frame, *header)) {
      return false;
    }
    if (!header_is_destroyed(*header)) {
      bool ignored_destroyed = false;
      const bool destroyed =
          perform_destroy(frame, value, *header, &ignored_destroyed);
      if (!destroyed && !fault_.has_value()) {
        return false;
      }
    }

    release_payload_for_dealloc(value);
    header->flags &= ~kObjectFlagDestroying;
    header->flags |= kObjectFlagDestroyed | kObjectFlagDead;
    header->lifetime_state = ObjectLifetimeState::Deallocated;
    header->shape = state_->dead_shape;
    state_->heap.drain_remote_frees();
    *changed = true;
    return !fault_.has_value();
  }

  Value make_list_value(std::vector<Value> items, bool frozen = false) {
    return state_->heap.make_list_value(std::move(items), frozen);
  }

  Value make_tuple_value(std::vector<Value> items) {
    return state_->heap.make_tuple_value(std::move(items));
  }

  Value make_symbol_map_value(std::vector<MapEntry> entries,
                              bool frozen = false) {
    return state_->heap.make_symbol_map_value(std::move(entries), frozen);
  }

  std::shared_ptr<ClosureValue> make_closure_value() {
    return state_->heap.make_closure_value();
  }

  std::shared_ptr<InstanceValue>
  make_instance_value(std::uint32_t class_index) {
    return state_->heap.make_instance_value(class_index);
  }

  bool apply_write_barrier(const Frame &frame, const Value &owner,
                           const Value &value) {
    const RuntimeWriteBarrierResult barrier =
        state_->heap.write_barrier(owner, value);
    if (!barrier.ok) {
      set_fault(frame, barrier.error_name, barrier.message);
      return false;
    }
    return true;
  }

  void append_value_root(std::vector<Value> *roots, const Value &value) const {
    if (value_has_heap_payload_tag(value)) {
      roots->push_back(value);
    }
  }

  void append_frame_roots(std::vector<Value> *roots, const Frame &frame) const {
    for (const Value &value : frame.regs) {
      append_value_root(roots, value);
    }
    for (const Value &value : frame.captures) {
      append_value_root(roots, value);
    }
    append_value_root(roots, frame.self);
    append_value_root(roots, frame.block);
    append_value_root(roots, frame.last_result);
    if (frame.return_override.has_value()) {
      append_value_root(roots, *frame.return_override);
    }
    for (const auto &[reg, value] : frame.pending_pattern_bindings) {
      (void)reg;
      append_value_root(roots, value);
    }
    for (const auto &[reg, state] : frame.prepared_seq_regs) {
      (void)reg;
      for (const Value &value : state.items) {
        append_value_root(roots, value);
      }
    }
    for (const auto &[reg, state] : frame.prepared_map_regs) {
      (void)reg;
      for (const MapEntry &entry : state.entries) {
        append_value_root(roots, entry.value);
      }
    }
  }

  std::vector<Value> collect_gc_roots() const {
    std::vector<Value> roots;
    for (const Frame &frame : frames_) {
      append_frame_roots(&roots, frame);
    }
    for (const Value &value : last_completed_regs_) {
      append_value_root(&roots, value);
    }
    append_value_root(&roots, final_value_);
    for (const ClassRuntimeState &klass : state_->classes) {
      for (const auto &[name, value] : klass.cvars) {
        (void)name;
        append_value_root(&roots, value);
      }
    }
    return roots;
  }

  void run_safepoint() {
    state_->heap.drain_remote_frees();
    const std::optional<RuntimeGcCycle> requested =
        state_->heap.pending_gc_request();
    if (!requested.has_value()) {
      return;
    }
    state_->heap.collect_garbage(collect_gc_roots(), *requested, true);
  }

  TraceFrame trace_frame_for(const Frame &frame, std::uint32_t pc) const {
    TraceFrame out;
    out.code_id = frame.code == nullptr ? 0U : frame.code->code_id;
    out.pc = pc;
    if (frame.code == nullptr) {
      return out;
    }

    for (const bytecode::SourceSpanEntry &entry : frame.code->source_spans) {
      if (entry.pc_from <= pc && pc < entry.pc_to) {
        out.file = entry.span.file;
        out.line = static_cast<std::uint32_t>(entry.span.start.line);
        out.column = static_cast<std::uint32_t>(entry.span.start.col);
        return out;
      }
    }

    std::uint32_t best_pc = 0;
    for (const bytecode::LineEntry &entry : module_.line_table) {
      if (entry.code_id == frame.code->code_id && entry.pc <= pc &&
          (out.line == 0U || entry.pc >= best_pc)) {
        best_pc = entry.pc;
        out.line = entry.line;
      }
    }
    return out;
  }

  std::string format_trace_text(const std::string &error_name,
                                const std::string &message,
                                const std::vector<TraceFrame> &trace) const {
    std::ostringstream out;
    out << error_name << ": " << message;
    for (const TraceFrame &frame : trace) {
      out << "\n  at c" << frame.code_id << ":" << frame.pc;
      if (!frame.file.empty()) {
        out << " (" << frame.file;
        if (frame.line != 0U) {
          out << ":" << frame.line;
          if (frame.column != 0U) {
            out << ":" << frame.column;
          }
        }
        out << ")";
      } else if (frame.line != 0U) {
        out << " (line " << frame.line << ")";
      }
    }
    return out.str();
  }

  Fault make_fault(const Frame &primary, const std::string &error_name,
                   const std::string &message) const {
    const std::uint32_t code_id =
        primary.code == nullptr ? 0U : primary.code->code_id;
    const std::uint32_t pc = static_cast<std::uint32_t>(primary.pc);
    Fault fault{error_name, message, code_id, pc};
    fault.trace.push_back(trace_frame_for(primary, pc));
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
      if (&*it == &primary) {
        continue;
      }
      const std::uint32_t frame_pc =
          it->active_call_pc.value_or(static_cast<std::uint32_t>(it->pc));
      fault.trace.push_back(trace_frame_for(*it, frame_pc));
    }
    fault.trace_text =
        format_trace_text(fault.error_name, fault.message, fault.trace);
    return fault;
  }

  void set_fault(const Frame &frame, const std::string &error_name,
                 const std::string &message) {
    fault_ = make_fault(frame, error_name, message);
  }

  void push_frame(const BcCode &code, const std::vector<Value> &args,
                  std::vector<Value> captures, Value self, Value block,
                  std::optional<std::uint32_t> caller_result_reg) {
    Frame frame;
    frame.code = &code;
    frame.regs.assign(code.reg_count, Value::null());
    for (std::size_t i = 0; i < args.size() && i < frame.regs.size(); ++i) {
      frame.regs[i] = args[i];
    }
    frame.captures = std::move(captures);
    frame.self = std::move(self);
    frame.block = std::move(block);
    frame.caller_result_reg = caller_result_reg;
    frames_.push_back(std::move(frame));
  }

  bool operand_u32(const Frame &frame, const Instruction &insn, std::size_t idx,
                   std::uint32_t *out) {
    if (idx >= insn.operands.size()) {
      set_fault(frame, "VMError", "missing operand");
      return false;
    }
    const std::int64_t value = insn.operands[idx].value;
    if (value < 0) {
      set_fault(frame, "VMError", "negative operand in unsigned slot");
      return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
  }

  bool operand_i64(const Frame &frame, const Instruction &insn, std::size_t idx,
                   std::int64_t *out) {
    if (idx >= insn.operands.size()) {
      set_fault(frame, "VMError", "missing operand");
      return false;
    }
    *out = insn.operands[idx].value;
    return true;
  }

  std::optional<std::uint32_t> optional_operand_u32(const Frame &frame,
                                                    const Instruction &insn,
                                                    std::size_t idx) {
    if (idx >= insn.operands.size()) {
      return std::nullopt;
    }
    std::uint32_t value = 0;
    if (!operand_u32(frame, insn, idx, &value)) {
      return std::nullopt;
    }
    return value;
  }

  Value load_constant(const Frame &frame, std::uint32_t const_id) {
    if (const_id >= module_.const_pool.size()) {
      set_fault(frame, "VMError", "constant ref out of range");
      return Value::null();
    }
    const Constant &constant = module_.const_pool[const_id];
    switch (constant.kind) {
    case ConstantKind::Null:
      return Value::null();
    case ConstantKind::Bool:
      return Value::boolean(constant.bool_value);
    case ConstantKind::Integer:
      return Value::integer(constant.int_value);
    case ConstantKind::Float:
      return Value::floating(constant.float_value);
    case ConstantKind::SymbolRef:
      return Value::symbol(constant.ref_id);
    case ConstantKind::StringRef:
      return Value::string(constant.ref_id);
    case ConstantKind::CodeRef:
    case ConstantKind::Path:
      set_fault(frame, "VMError", "constant kind is not executable in W5.1");
      return Value::null();
    case ConstantKind::KeySet: {
      std::vector<Value> items;
      items.reserve(constant.items.size());
      for (std::uint32_t symbol_id : constant.items) {
        items.push_back(Value::symbol(symbol_id));
      }
      return make_tuple_value(std::move(items));
    }
    }
    set_fault(frame, "VMError", "unknown constant kind");
    return Value::null();
  }

  bool path_segments_from_constant(const Frame &frame, const Constant &constant,
                                   std::vector<std::string> *out) {
    if (constant.kind != ConstantKind::Path) {
      set_fault(frame, "VMError",
                "path lookup expects path constant in current runtime");
      return false;
    }
    if (constant.items.empty()) {
      set_fault(frame, "VMError", "path constant is empty");
      return false;
    }
    out->clear();
    out->reserve(constant.items.size());
    for (std::uint32_t symbol_id : constant.items) {
      if (symbol_id >= module_.symbols.size()) {
        set_fault(frame, "VMError", "path symbol ref is out of range");
        return false;
      }
      out->push_back(module_.symbols[symbol_id]);
    }
    return true;
  }

  std::string join_path_segments(const std::vector<std::string> &segments) {
    std::string out;
    for (std::size_t i = 0; i < segments.size(); ++i) {
      if (i != 0U) {
        out += ".";
      }
      out += segments[i];
    }
    return out;
  }

  bool find_class_by_path_segments(const Frame &frame,
                                   const std::vector<std::string> &segments,
                                   std::uint32_t *out_class_index) {
    if (segments.empty()) {
      set_fault(frame, "VMError", "class path is empty");
      return false;
    }

    const std::string full_path = join_path_segments(segments);
    for (std::uint32_t index = 0; index < module_.classes.size(); ++index) {
      const std::uint32_t symbol_id = module_.classes[index].class_name_sym_id;
      if (symbol_id < module_.symbols.size() &&
          module_.symbols[symbol_id] == full_path) {
        *out_class_index = index;
        return true;
      }
    }

    const std::string &leaf = segments.back();
    std::optional<std::uint32_t> match;
    for (std::uint32_t index = 0; index < module_.classes.size(); ++index) {
      const std::uint32_t symbol_id = module_.classes[index].class_name_sym_id;
      if (symbol_id >= module_.symbols.size()) {
        continue;
      }
      if (module_.symbols[symbol_id] != leaf) {
        continue;
      }
      if (match.has_value()) {
        set_fault(frame, "VMError", "class path ref is ambiguous");
        return false;
      }
      match = index;
    }
    if (match.has_value()) {
      *out_class_index = *match;
      return true;
    }
    set_fault(frame, "VMError", "class path ref target is unknown");
    return false;
  }

  Value lookup_constant(const Frame &frame, std::uint32_t const_id) {
    if (const_id >= module_.const_pool.size()) {
      set_fault(frame, "VMError", "constant ref out of range");
      return Value::null();
    }
    const Constant &constant = module_.const_pool[const_id];
    if (constant.kind != ConstantKind::Path) {
      set_fault(frame, "VMError",
                "LOOKUP_CONST expects path constant in current runtime");
      return Value::null();
    }
    std::vector<std::string> segments;
    if (!path_segments_from_constant(frame, constant, &segments)) {
      return Value::null();
    }
    std::uint32_t class_index = 0;
    if (find_class_by_path_segments(frame, segments, &class_index)) {
      return Value::class_object(class_index);
    }
    return Value::null();
  }

  Value read_reg(const Frame &frame, std::uint32_t reg) {
    if (reg >= frame.regs.size()) {
      set_fault(frame, "VMError", "register out of range");
      return Value::null();
    }
    return frame.regs[reg];
  }

  bool write_reg(Frame &frame, std::uint32_t reg, Value value) {
    if (reg >= frame.regs.size()) {
      set_fault(frame, "VMError", "register out of range");
      return false;
    }
    frame.regs[reg] = std::move(value);
    frame.prepared_seq_regs.erase(reg);
    frame.prepared_map_regs.erase(reg);
    frame.pending_pattern_bindings.erase(reg);
    return true;
  }

  void clear_pattern_state(Frame &frame) {
    frame.prepared_seq_regs.clear();
    frame.prepared_map_regs.clear();
    frame.pending_pattern_bindings.clear();
  }

  bool has_active_pattern_state(const Frame &frame) const {
    return !frame.prepared_seq_regs.empty() ||
           !frame.prepared_map_regs.empty() ||
           !frame.pending_pattern_bindings.empty();
  }

  std::optional<std::vector<Value>>
  extract_sequence_items(const Frame &frame, const Value &value,
                         bool *out_source_was_tuple) {
    if (value.is_list()) {
      const std::shared_ptr<ListValue> list = value.as_list();
      if (list == nullptr) {
        set_fault(frame, "TypeError", "list value is null");
        return std::nullopt;
      }
      if (!ensure_lifecycle_access(frame, value)) {
        return std::nullopt;
      }
      *out_source_was_tuple = false;
      return list->items;
    }
    if (value.is_tuple()) {
      const std::shared_ptr<TupleValue> tuple = value.as_tuple();
      if (tuple == nullptr) {
        set_fault(frame, "TypeError", "tuple value is null");
        return std::nullopt;
      }
      if (!ensure_lifecycle_access(frame, value)) {
        return std::nullopt;
      }
      *out_source_was_tuple = true;
      return tuple->items;
    }
    return std::nullopt;
  }

  std::optional<std::vector<MapEntry>> extract_map_entries(const Frame &frame,
                                                           const Value &value) {
    if (!value.is_map()) {
      return std::nullopt;
    }
    const std::shared_ptr<MapValue> map = value.as_map();
    if (map == nullptr) {
      set_fault(frame, "TypeError", "map value is null");
      return std::nullopt;
    }
    if (!ensure_lifecycle_access(frame, value)) {
      return std::nullopt;
    }
    return map->entries;
  }

  Value materialize_sequence_rest(const PreparedSeqState &state) {
    std::vector<Value> rest;
    if (state.rest_start < state.items.size()) {
      rest.assign(state.items.begin() +
                      static_cast<std::ptrdiff_t>(state.rest_start),
                  state.items.end());
    }
    if (state.source_was_tuple) {
      return make_tuple_value(std::move(rest));
    }
    return make_list_value(std::move(rest));
  }

  Value materialize_map_rest(const PreparedMapState &state) {
    std::vector<MapEntry> rest;
    std::unordered_map<std::uint32_t, bool> requested;
    for (std::uint32_t symbol_id : state.requested_keys) {
      requested[symbol_id] = true;
    }
    for (const MapEntry &entry : state.entries) {
      if (requested.find(entry.symbol_id) == requested.end()) {
        rest.push_back(entry);
      }
    }
    return make_symbol_map_value(std::move(rest));
  }

  bool prepared_map_has_extras(const PreparedMapState &state) const {
    if (!state.needs_full) {
      return false;
    }
    std::unordered_map<std::uint32_t, bool> requested;
    for (std::uint32_t symbol_id : state.requested_keys) {
      requested[symbol_id] = true;
    }
    for (const MapEntry &entry : state.entries) {
      if (requested.find(entry.symbol_id) == requested.end()) {
        return true;
      }
    }
    return false;
  }

  bool finalize_pattern_success(Frame &frame, bool allow_pending_bindings) {
    if (!has_active_pattern_state(frame)) {
      return true;
    }
    std::optional<std::uint32_t> fail_pc;
    for (const auto &[reg, state] : frame.prepared_map_regs) {
      (void)reg;
      if (state.needs_full && !state.rest_bound &&
          prepared_map_has_extras(state)) {
        fail_pc = state.fail_pc;
        break;
      }
    }
    if (fail_pc.has_value()) {
      if (*fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return false;
      }
      clear_pattern_state(frame);
      frame.pc = *fail_pc;
      return false;
    }
    if (!allow_pending_bindings && !frame.pending_pattern_bindings.empty()) {
      set_fault(frame, "VMError",
                "pattern bindings reached success boundary without P_COMMIT");
      clear_pattern_state(frame);
      return false;
    }
    frame.prepared_seq_regs.clear();
    frame.prepared_map_regs.clear();
    return true;
  }

  std::optional<std::string>
  selector_text_from_symbol(std::uint32_t symbol_id) {
    if (symbol_id >= module_.symbols.size()) {
      return std::nullopt;
    }
    return module_.symbols[symbol_id];
  }

  std::optional<std::string> string_text_from_id(std::uint32_t string_id) {
    if (string_id >= module_.strings.size()) {
      return std::nullopt;
    }
    return module_.strings[string_id];
  }

  std::optional<std::uint32_t> symbol_id_for_text(const std::string &text) {
    for (std::uint32_t i = 0; i < module_.symbols.size(); ++i) {
      if (module_.symbols[i] == text) {
        return i;
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> selector_text_from_value(const Value &value) {
    if (value.is_symbol()) {
      return selector_text_from_symbol(value.as_symbol().symbol_id);
    }
    if (value.is_string()) {
      const std::uint32_t string_id = value.as_string().string_id;
      if (string_id >= module_.strings.size()) {
        return std::nullopt;
      }
      return module_.strings[string_id];
    }
    return std::nullopt;
  }

  std::optional<std::string> class_name_for_index(std::uint32_t class_index) {
    if (class_index >= module_.classes.size()) {
      return std::nullopt;
    }
    const std::uint32_t symbol_id =
        module_.classes[class_index].class_name_sym_id;
    if (symbol_id >= module_.symbols.size()) {
      return std::nullopt;
    }
    return module_.symbols[symbol_id];
  }

  std::string exception_error_name(const Value &exception) {
    if (exception.is_instance_object()) {
      const std::shared_ptr<InstanceValue> instance =
          exception.as_instance_object();
      if (instance != nullptr) {
        const std::optional<std::string> name =
            class_name_for_index(instance->class_index);
        if (name.has_value()) {
          return *name;
        }
      }
    }
    if (exception.is_class_object()) {
      const std::optional<std::string> name =
          class_name_for_index(exception.as_class_object().class_index);
      if (name.has_value()) {
        return *name;
      }
    }
    if (exception.is_symbol()) {
      const std::optional<std::string> name =
          selector_text_from_symbol(exception.as_symbol().symbol_id);
      if (name.has_value()) {
        return *name;
      }
    }
    if (exception.is_string()) {
      const std::optional<std::string> name =
          string_text_from_id(exception.as_string().string_id);
      if (name.has_value()) {
        return *name;
      }
    }
    return "Exception";
  }

  Value
  selector_value_for_method_missing(const std::string &selector_text,
                                    const std::optional<Value> &original) {
    if (original.has_value() && original->is_symbol()) {
      return *original;
    }
    const std::optional<std::uint32_t> symbol_id =
        symbol_id_for_text(selector_text);
    if (symbol_id.has_value()) {
      return Value::symbol(*symbol_id);
    }
    if (original.has_value()) {
      return *original;
    }
    return Value::null();
  }

  std::uint64_t inline_cache_key(const Frame &frame,
                                 std::uint32_t site_id) const {
    const std::uint64_t code_id =
        frame.code == nullptr ? 0U : frame.code->code_id;
    return (code_id << 32U) | site_id;
  }

  const bytecode::BcMethod *probe_call_cache(const Frame &frame,
                                             std::uint32_t site_id,
                                             std::uint32_t receiver_class_index,
                                             std::uint32_t dispatch_flags,
                                             std::uint32_t selector_symbol_id) {
    const auto found =
        state_->call_caches.find(inline_cache_key(frame, site_id));
    if (found == state_->call_caches.end()) {
      return nullptr;
    }
    const CallCacheEntry &entry = found->second;
    if (!entry.valid || entry.receiver_class_index != receiver_class_index ||
        entry.dispatch_flags != dispatch_flags ||
        entry.selector_symbol_id != selector_symbol_id ||
        entry.world_epoch != state_->world_epoch ||
        receiver_class_index >= state_->classes.size() ||
        entry.method_version !=
            state_->classes[receiver_class_index].method_version) {
      return nullptr;
    }
    return &entry.method;
  }

  void update_call_cache(const Frame &frame, std::uint32_t site_id,
                         std::uint32_t receiver_class_index,
                         std::uint32_t dispatch_flags,
                         std::uint32_t selector_symbol_id,
                         const bytecode::BcMethod &method) {
    if (receiver_class_index >= state_->classes.size()) {
      return;
    }
    CallCacheEntry entry;
    entry.valid = true;
    entry.receiver_class_index = receiver_class_index;
    entry.dispatch_flags = dispatch_flags;
    entry.selector_symbol_id = selector_symbol_id;
    entry.method_version = state_->classes[receiver_class_index].method_version;
    entry.world_epoch = state_->world_epoch;
    entry.method = method;
    state_->call_caches[inline_cache_key(frame, site_id)] = entry;
  }

  std::optional<std::uint32_t> probe_ivar_cache(const Frame &frame,
                                                std::uint32_t site_id,
                                                const InstanceValue &instance,
                                                std::uint32_t symbol_id) {
    const auto found =
        state_->ivar_caches.find(inline_cache_key(frame, site_id));
    if (found == state_->ivar_caches.end()) {
      return std::nullopt;
    }
    const IvarCacheEntry &entry = found->second;
    const std::shared_ptr<const ShapeDescriptor> shape = instance.header.shape;
    if (shape == nullptr || shape->dead) {
      return std::nullopt;
    }
    if (!entry.valid || entry.receiver_class_index != instance.class_index ||
        entry.symbol_id != symbol_id || entry.shape_id != shape->shape_id ||
        entry.shape_version != shape->shape_version ||
        entry.slot_index >= instance.ivar_storage.size()) {
      return std::nullopt;
    }
    return entry.slot_index;
  }

  void update_ivar_cache(const Frame &frame, std::uint32_t site_id,
                         const InstanceValue &instance, std::uint32_t symbol_id,
                         std::uint32_t slot_index) {
    const std::shared_ptr<const ShapeDescriptor> shape = instance.header.shape;
    if (shape == nullptr || shape->dead) {
      return;
    }
    IvarCacheEntry entry;
    entry.valid = true;
    entry.receiver_class_index = instance.class_index;
    entry.symbol_id = symbol_id;
    entry.shape_id = shape->shape_id;
    entry.shape_version = shape->shape_version;
    entry.slot_index = slot_index;
    state_->ivar_caches[inline_cache_key(frame, site_id)] = std::move(entry);
  }

  bool has_optional_reg(std::int64_t raw) const {
    return raw >= 0 && raw != static_cast<std::int64_t>(
                                  std::numeric_limits<std::uint32_t>::max());
  }

  bool expect_instance_receiver(const Frame &frame, const Value &receiver,
                                std::shared_ptr<InstanceValue> *out) {
    if (!receiver.is_instance_object()) {
      set_fault(frame, "TypeError",
                "ivar access expects instance receiver in current runtime");
      return false;
    }
    *out = receiver.as_instance_object();
    if (*out == nullptr) {
      set_fault(frame, "TypeError", "instance receiver is null");
      return false;
    }
    if (!ensure_instance_layout(frame, *out)) {
      return false;
    }
    return true;
  }

  bool ensure_instance_layout(const Frame &frame,
                              const std::shared_ptr<InstanceValue> &instance) {
    if (instance == nullptr) {
      set_fault(frame, "TypeError", "instance receiver is null");
      return false;
    }
    if (instance->class_index >= state_->classes.size()) {
      state_->classes.resize(static_cast<std::size_t>(instance->class_index) +
                             1U);
    }
    instance->header.kind = HeapObjectKind::Instance;
    instance->header.class_index = instance->class_index;
    const std::optional<std::string> lifecycle_error =
        lifecycle_access_error_name(instance->header);
    if (lifecycle_error.has_value()) {
      set_fault(frame, *lifecycle_error,
                lifecycle_access_error_message(*lifecycle_error));
      return false;
    }
    if (instance->header.shape == nullptr) {
      instance->header.shape =
          state_->root_shape_for_class(instance->class_index);
      if (!instance->ivars.empty()) {
        std::map<std::string, Value> ordered(instance->ivars.begin(),
                                             instance->ivars.end());
        instance->ivar_storage.clear();
        for (const auto &[name, value] : ordered) {
          if (!store_instance_ivar_slow(frame, instance, name, value)) {
            return false;
          }
        }
      }
    }
    if (instance->header.shape == nullptr) {
      set_fault(frame, "VMError", "instance shape is missing");
      return false;
    }
    if (instance->ivar_storage.size() <
        instance->header.shape->slot_names.size()) {
      instance->ivar_storage.resize(instance->header.shape->slot_names.size(),
                                    Value::null());
    }
    instance->ivar_shape_version = instance->header.shape->shape_version;
    return true;
  }

  bool store_instance_ivar_slow(const Frame &frame,
                                const std::shared_ptr<InstanceValue> &instance,
                                const std::string &name, Value value) {
    if (instance == nullptr) {
      set_fault(frame, "TypeError", "instance receiver is null");
      return false;
    }
    const std::optional<std::string> lifecycle_error =
        lifecycle_access_error_name(instance->header);
    if (lifecycle_error.has_value()) {
      set_fault(frame, *lifecycle_error,
                lifecycle_access_error_message(*lifecycle_error));
      return false;
    }
    if (instance->header.shape == nullptr || instance->header.shape->dead) {
      set_fault(frame, "UseAfterFreeError", "access to deallocated object");
      return false;
    }
    if (!apply_write_barrier(frame, Value::instance(instance), value)) {
      return false;
    }
    std::shared_ptr<const ShapeDescriptor> shape = instance->header.shape;
    const auto existing_slot = shape->ivar_slots.find(name);
    if (existing_slot == shape->ivar_slots.end()) {
      const std::shared_ptr<const ShapeDescriptor> next =
          state_->transition_shape(shape, name);
      if (next == nullptr || next->dead) {
        set_fault(frame, "UseAfterFreeError", "access to deallocated object");
        return false;
      }
      std::vector<Value> storage(next->slot_names.size(), Value::null());
      for (std::size_t i = 0;
           i < instance->ivar_storage.size() && i < storage.size(); ++i) {
        storage[i] = instance->ivar_storage[i];
      }
      instance->ivar_storage = std::move(storage);
      instance->header.shape = next;
      shape = next;
    } else if (instance->ivar_storage.size() < shape->slot_names.size()) {
      instance->ivar_storage.resize(shape->slot_names.size(), Value::null());
    }

    const auto slot = shape->ivar_slots.find(name);
    if (slot == shape->ivar_slots.end() ||
        slot->second >= instance->ivar_storage.size()) {
      set_fault(frame, "VMError", "ivar slot allocation failed");
      return false;
    }
    instance->ivar_storage[slot->second] = value;
    instance->ivars[name] = std::move(value);
    instance->ivar_shape_version = shape->shape_version;
    return true;
  }

  std::optional<std::uint32_t>
  ensure_instance_ivar_slot(const Frame &frame,
                            const std::shared_ptr<InstanceValue> &instance,
                            const std::string &name) {
    if (!ensure_instance_layout(frame, instance)) {
      return std::nullopt;
    }
    const std::shared_ptr<const ShapeDescriptor> shape = instance->header.shape;
    const auto slot = shape->ivar_slots.find(name);
    if (slot != shape->ivar_slots.end()) {
      return slot->second;
    }
    const auto legacy = instance->ivars.find(name);
    if (legacy == instance->ivars.end()) {
      return std::nullopt;
    }
    const Value value = legacy->second;
    if (!store_instance_ivar_slow(frame, instance, name, value)) {
      return std::nullopt;
    }
    const auto imported_slot = instance->header.shape->ivar_slots.find(name);
    if (imported_slot == instance->header.shape->ivar_slots.end()) {
      set_fault(frame, "VMError", "legacy ivar import failed");
      return std::nullopt;
    }
    return imported_slot->second;
  }

  bool store_instance_ivar(const Frame &frame,
                           const std::shared_ptr<InstanceValue> &instance,
                           const std::string &name, Value value,
                           std::uint32_t symbol_id, std::uint32_t site_id) {
    if (!ensure_instance_layout(frame, instance)) {
      return false;
    }
    if (!store_instance_ivar_slow(frame, instance, name, std::move(value))) {
      return false;
    }
    const auto slot = instance->header.shape->ivar_slots.find(name);
    if (slot == instance->header.shape->ivar_slots.end()) {
      set_fault(frame, "VMError", "stored ivar slot is missing");
      return false;
    }
    update_ivar_cache(frame, site_id, *instance, symbol_id, slot->second);
    return true;
  }

  bool expect_class_owner(const Frame &frame, const Value &owner,
                          std::uint32_t *out_class_index) {
    if (owner.is_class_object()) {
      const std::uint32_t class_index = owner.as_class_object().class_index;
      if (class_index >= state_->classes.size()) {
        set_fault(frame, "VMError", "class owner index is out of range");
        return false;
      }
      *out_class_index = class_index;
      return true;
    }
    if (owner.is_instance_object()) {
      const std::shared_ptr<InstanceValue> instance =
          owner.as_instance_object();
      if (instance == nullptr) {
        set_fault(frame, "TypeError", "instance owner is null");
        return false;
      }
      if (!ensure_instance_layout(frame, instance)) {
        return false;
      }
      *out_class_index = instance->class_index;
      return true;
    }
    set_fault(frame, "TypeError",
              "class-variable access expects class or instance owner");
    return false;
  }

  bool value_is_instance_of(const Frame &frame, std::uint32_t value_class_index,
                            std::uint32_t target_class_index) {
    std::vector<bool> active(module_.classes.size(), false);
    std::uint32_t current = value_class_index;
    while (true) {
      if (current >= module_.classes.size()) {
        set_fault(frame, "VMError", "instance class index is out of range");
        return false;
      }
      if (current == target_class_index) {
        return true;
      }
      if (active[current]) {
        set_fault(frame, "VMError", "cycle detected in superclass chain");
        return false;
      }
      active[current] = true;
      const bytecode::BcClass &klass = module_.classes[current];
      if (!klass.has_superclass_ref) {
        return false;
      }
      if (!resolve_class_ref(frame, klass.superclass_ref, &current)) {
        return false;
      }
      if (fault_.has_value()) {
        return false;
      }
    }
  }

  bool pattern_triple_eq(const Frame &frame, const Value &matcher,
                         const Value &value, bool *out) {
    if (matcher.is_class_object()) {
      const std::uint32_t target_class_index =
          matcher.as_class_object().class_index;
      if (value.is_instance_object()) {
        if (!ensure_lifecycle_access(frame, value)) {
          return false;
        }
        const std::shared_ptr<InstanceValue> instance =
            value.as_instance_object();
        if (instance == nullptr) {
          set_fault(frame, "TypeError", "instance matcher value is null");
          return false;
        }
        *out = value_is_instance_of(frame, instance->class_index,
                                    target_class_index);
        return !fault_.has_value();
      }
      if (value.is_class_object()) {
        *out = value.as_class_object().class_index == target_class_index;
        return true;
      }
      *out = false;
      return true;
    }

    Value result = Value::null();
    const SendStatus scalar_status =
        try_apply_scalar_send(frame, matcher, "===", {value}, &result);
    if (scalar_status == SendStatus::Faulted) {
      return false;
    }
    if (scalar_status == SendStatus::Matched) {
      if (!result.is_bool()) {
        set_fault(frame, "TypeError", "pattern triple-eq did not return bool");
        return false;
      }
      *out = result.as_bool();
      return true;
    }

    *out = value_equals(matcher, value);
    return true;
  }

  bool load_method_params(const Frame &frame, const bytecode::BcMethod &method,
                          std::vector<bytecode::MethodParamEntry> *out) {
    if (!method.params.empty()) {
      *out = method.params;
      return true;
    }
    if (method.signature_blob_id >= module_.const_pool.size()) {
      set_fault(frame, "VMError", "method signature blob ref is out of range");
      return false;
    }
    const Constant &constant = module_.const_pool[method.signature_blob_id];
    if (constant.kind != ConstantKind::Path) {
      set_fault(frame, "VMError", "method signature blob is not a path record");
      return false;
    }
    out->clear();
    out->reserve(constant.items.size());
    for (std::uint32_t symbol_id : constant.items) {
      bytecode::MethodParamEntry entry;
      entry.external_name_sym_id = symbol_id;
      out->push_back(entry);
    }
    if (method.default_thunk_ids.size() > out->size()) {
      set_fault(frame, "VMError", "default thunk table exceeds param count");
      return false;
    }
    const std::size_t defaults_begin =
        out->size() - method.default_thunk_ids.size();
    for (std::size_t index = defaults_begin; index < out->size(); ++index) {
      (*out)[index].flags |= bytecode::kMethodParamFlagHasDefault;
    }
    return true;
  }

  std::optional<std::uint32_t>
  local_slot_for_name(const Frame &frame, const BcCode &code,
                      std::uint32_t local_name_str_id) {
    for (const bytecode::SlotLayoutEntry &entry : code.local_layout) {
      if (entry.name_str_id == local_name_str_id) {
        return entry.slot;
      }
    }
    set_fault(frame, "VMError", "auto-assign local slot is missing");
    return std::nullopt;
  }

  bool apply_auto_assigns(Frame &frame, const bytecode::BcMethod &method,
                          const BcCode &code) {
    if (method.auto_assign_desc.empty()) {
      return true;
    }
    std::shared_ptr<InstanceValue> instance;
    bool have_instance = false;
    for (const bytecode::AutoAssignEntry &entry : method.auto_assign_desc) {
      const std::optional<std::uint32_t> slot =
          local_slot_for_name(frame, code, entry.local_name_str_id);
      if (!slot.has_value()) {
        return false;
      }
      if (*slot >= frame.regs.size()) {
        set_fault(frame, "VMError", "auto-assign local slot is out of range");
        return false;
      }
      const std::optional<std::string> target =
          string_text_from_id(entry.target_name_str_id);
      if (!target.has_value()) {
        set_fault(frame, "VMError", "auto-assign target string is invalid");
        return false;
      }
      if (target->rfind("@@", 0) == 0) {
        std::uint32_t class_index = 0;
        if (!expect_class_owner(frame, frame.self, &class_index)) {
          return false;
        }
        state_->classes[class_index].cvars[target->substr(2)] =
            frame.regs[*slot];
        continue;
      }
      if (target->empty() || (*target)[0] != '@') {
        set_fault(frame, "VMError", "auto-assign target is not an ivar");
        return false;
      }
      if (!have_instance) {
        if (!expect_instance_receiver(frame, frame.self, &instance)) {
          return false;
        }
        have_instance = true;
      }
      const std::string ivar_name = target->substr(1);
      if (!store_instance_ivar_slow(frame, instance, ivar_name,
                                    frame.regs[*slot])) {
        return false;
      }
    }
    return true;
  }

  bool
  shape_method_call(const Frame &frame, const bytecode::BcMethod &method,
                    const std::vector<Value> &pos_args,
                    const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
                    std::vector<bytecode::MethodParamEntry> *out_params,
                    std::vector<BoundMethodArg> *out_slots) {
    if (!load_method_params(frame, method, out_params)) {
      return false;
    }
    out_slots->assign(out_params->size(), {});

    for (std::size_t i = 0; i < kw_args.size(); ++i) {
      for (std::size_t j = i + 1; j < kw_args.size(); ++j) {
        if (kw_args[i].first == kw_args[j].first) {
          set_fault(frame, "TypeError",
                    "duplicate keyword argument in runtime dispatch");
          return false;
        }
      }
    }

    std::size_t positional_cursor = 0;
    for (std::size_t i = 0; i < out_params->size(); ++i) {
      const bytecode::MethodParamEntry &entry = (*out_params)[i];
      if ((entry.flags & bytecode::kMethodParamFlagKeyword) != 0U) {
        continue;
      }
      if (positional_cursor < pos_args.size()) {
        (*out_slots)[i].present = true;
        (*out_slots)[i].value = pos_args[positional_cursor++];
      }
    }
    if (positional_cursor != pos_args.size()) {
      set_fault(frame, "TypeError", "too many positional arguments");
      return false;
    }

    std::vector<bool> kw_consumed(kw_args.size(), false);
    for (std::size_t i = 0; i < out_params->size(); ++i) {
      const bytecode::MethodParamEntry &entry = (*out_params)[i];
      if ((entry.flags & bytecode::kMethodParamFlagKeyword) == 0U) {
        continue;
      }
      for (std::size_t kw_index = 0; kw_index < kw_args.size(); ++kw_index) {
        if (kw_args[kw_index].first == entry.external_name_sym_id) {
          (*out_slots)[i].present = true;
          (*out_slots)[i].value = kw_args[kw_index].second;
          kw_consumed[kw_index] = true;
          break;
        }
      }
    }

    for (std::size_t kw_index = 0; kw_index < kw_args.size(); ++kw_index) {
      if (!kw_consumed[kw_index]) {
        set_fault(frame, "TypeError", "unknown keyword argument");
        return false;
      }
    }

    for (std::size_t i = 0; i < out_params->size(); ++i) {
      if ((*out_slots)[i].present) {
        continue;
      }
      if (((*out_params)[i].flags & bytecode::kMethodParamFlagHasDefault) ==
          0U) {
        set_fault(frame, "TypeError", "missing required parameter");
        return false;
      }
    }
    return true;
  }

  bool
  materialize_defaults(Frame &frame, const bytecode::BcMethod &method,
                       const std::vector<bytecode::MethodParamEntry> &params,
                       const std::vector<BoundMethodArg> &slots) {
    if (params.size() > frame.regs.size()) {
      set_fault(frame, "VMError", "parameter slots exceed frame register file");
      return false;
    }
    for (std::size_t slot = 0; slot < slots.size(); ++slot) {
      if (slots[slot].present) {
        frame.regs[slot] = slots[slot].value;
      }
    }

    std::size_t thunk_index = 0;
    for (std::size_t slot = 0; slot < params.size(); ++slot) {
      if ((params[slot].flags & bytecode::kMethodParamFlagHasDefault) == 0U) {
        continue;
      }
      if (!slots[slot].present) {
        if (thunk_index >= method.default_thunk_ids.size()) {
          set_fault(frame, "VMError",
                    "missing default thunk for parameter slot");
          return false;
        }
        Vm nested(module_, state_);
        const ExecutionResult result =
            nested.execute(method.default_thunk_ids[thunk_index], frame.regs,
                           frame.self, frame.block);
        if (!result.ok()) {
          fault_ = result.fault;
          return false;
        }
        frame.regs[slot] = result.value;
      }
      ++thunk_index;
    }
    return true;
  }

  NestedExecution execute_nested_code(std::uint32_t code_id,
                                      const std::vector<Value> &regs,
                                      const Value &self, const Value &block) {
    NestedExecution out;
    const BcCode *entry = find_code(module_, code_id);
    if (entry == nullptr) {
      out.fault = Fault{"VMError", "unknown nested code id", code_id, 0};
      return out;
    }

    Vm nested(module_, state_);
    nested.push_frame(*entry, {}, {}, self, block, std::nullopt);
    Frame &nested_frame = nested.frames_.back();
    const std::size_t copy_count =
        std::min(regs.size(), nested_frame.regs.size());
    for (std::size_t i = 0; i < copy_count; ++i) {
      nested_frame.regs[i] = regs[i];
    }

    while (nested.fault_ == std::nullopt && !nested.frames_.empty()) {
      nested.step();
    }

    out.value = nested.final_value_;
    out.regs = std::move(nested.last_completed_regs_);
    out.fault = nested.fault_;
    return out;
  }

  NestedExecution execute_prepared_frame(Frame frame) {
    NestedExecution out;
    Vm nested(module_, state_);
    nested.frames_.push_back(std::move(frame));
    while (nested.fault_ == std::nullopt && !nested.frames_.empty()) {
      nested.step();
    }
    out.value = nested.final_value_;
    out.regs = std::move(nested.last_completed_regs_);
    out.fault = nested.fault_;
    return out;
  }

  std::optional<Value> execute_method_to_value(
      Frame &caller, const bytecode::BcMethod &method,
      const std::vector<Value> &pos_args,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      const Value &self, const Value &block) {
    std::vector<bytecode::MethodParamEntry> params;
    std::vector<BoundMethodArg> slots;
    if (!shape_method_call(caller, method, pos_args, kw_args, &params,
                           &slots)) {
      return std::nullopt;
    }
    const BcCode *code = find_code(module_, method.entry_code_id);
    if (code == nullptr) {
      set_fault(caller, "VMError", "method entry code id is unknown");
      return std::nullopt;
    }

    Frame callee;
    callee.code = code;
    callee.regs.assign(code->reg_count, Value::null());
    callee.self = self;
    callee.block = block;
    if (!materialize_defaults(callee, method, params, slots) ||
        !apply_auto_assigns(callee, method, *code)) {
      return std::nullopt;
    }

    if (method.clause_table.empty()) {
      NestedExecution body = execute_prepared_frame(std::move(callee));
      if (!body.ok()) {
        fault_ = body.fault;
        return std::nullopt;
      }
      return body.value;
    }

    const std::vector<Value> base_regs = callee.regs;
    for (const bytecode::ClauseEntry &entry : method.clause_table) {
      const NestedExecution pattern =
          execute_nested_code(entry.pattern_code_id, base_regs, self, block);
      if (!pattern.ok()) {
        fault_ = pattern.fault;
        return std::nullopt;
      }
      if (!pattern.value.is_bool()) {
        set_fault(caller, "VMError",
                  "clause pattern probe did not return bool");
        return std::nullopt;
      }
      if (!pattern.value.as_bool()) {
        continue;
      }

      std::vector<Value> matched_regs = base_regs;
      const std::size_t copy_count =
          std::min(matched_regs.size(), pattern.regs.size());
      for (std::size_t i = 0; i < copy_count; ++i) {
        matched_regs[i] = pattern.regs[i];
      }

      const NestedExecution guard =
          execute_nested_code(entry.guard_code_id, matched_regs, self, block);
      if (!guard.ok()) {
        fault_ = guard.fault;
        return std::nullopt;
      }
      if (!is_truthy(guard.value)) {
        continue;
      }

      const NestedExecution body =
          execute_nested_code(entry.body_code_id, matched_regs, self, block);
      if (!body.ok()) {
        fault_ = body.fault;
        return std::nullopt;
      }
      return body.value;
    }

    NestedExecution fallback =
        execute_nested_code(method.entry_code_id, base_regs, self, block);
    if (!fallback.ok()) {
      fault_ = fallback.fault;
      return std::nullopt;
    }
    return fallback.value;
  }

  bool try_apply_protocol_send(Frame &frame, const Value &receiver,
                               const std::string &selector,
                               const std::vector<Value> &args, Value *out,
                               bool *handled) {
    *handled = false;
    const SendStatus scalar_status =
        try_apply_scalar_send(frame, receiver, selector, args, out);
    if (scalar_status == SendStatus::Faulted) {
      return false;
    }
    if (scalar_status == SendStatus::Matched) {
      *handled = true;
      return true;
    }

    std::uint32_t class_index = 0;
    std::uint32_t dispatch_flags = 0;
    if (receiver.is_instance_object()) {
      const std::shared_ptr<InstanceValue> instance =
          receiver.as_instance_object();
      if (instance == nullptr) {
        set_fault(frame, "TypeError", "instance receiver is null");
        return false;
      }
      class_index = instance->class_index;
      dispatch_flags = kMethodFlagInstance;
    } else if (receiver.is_class_object()) {
      class_index = receiver.as_class_object().class_index;
      dispatch_flags = kMethodFlagClass;
    } else {
      return true;
    }

    const bytecode::BcMethod *method =
        find_method_for_dispatch(frame, class_index, selector, dispatch_flags);
    if (fault_.has_value()) {
      return false;
    }
    if (method == nullptr) {
      return true;
    }
    const std::vector<std::pair<std::uint32_t, Value>> kw_args;
    const std::optional<Value> value = execute_method_to_value(
        frame, *method, args, kw_args, receiver, Value::null());
    if (!value.has_value()) {
      return false;
    }
    *out = *value;
    *handled = true;
    return true;
  }

  std::optional<CoercedSeqState>
  coerce_sequence_for_pattern(Frame &frame, const Value &source) {
    bool source_was_tuple = false;
    const std::optional<std::vector<Value>> native_items =
        extract_sequence_items(frame, source, &source_was_tuple);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (native_items.has_value()) {
      return CoercedSeqState{source, *native_items, source_was_tuple};
    }

    Value result = Value::null();
    bool handled = false;
    if (!try_apply_protocol_send(frame, source, "deconstruct", {}, &result,
                                 &handled)) {
      return std::nullopt;
    }
    if (!handled || result.is_null()) {
      return std::nullopt;
    }
    bool result_was_tuple = false;
    const std::optional<std::vector<Value>> result_items =
        extract_sequence_items(frame, result, &result_was_tuple);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (!result_items.has_value()) {
      set_fault(frame, "TypeError", "deconstruct returned non-sequence value");
      return std::nullopt;
    }
    return CoercedSeqState{result, *result_items, result_was_tuple};
  }

  std::optional<CoercedMapState> coerce_map_for_pattern(Frame &frame,
                                                        const Value &source,
                                                        const Constant &keyset,
                                                        bool needs_full) {
    const std::optional<std::vector<MapEntry>> native_entries =
        extract_map_entries(frame, source);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (native_entries.has_value()) {
      return CoercedMapState{source, *native_entries};
    }

    Value keys_arg = Value::null();
    if (!needs_full) {
      std::vector<Value> keys;
      keys.reserve(keyset.items.size());
      for (std::uint32_t symbol_id : keyset.items) {
        keys.push_back(Value::symbol(symbol_id));
      }
      keys_arg = make_tuple_value(std::move(keys));
    }

    Value result = Value::null();
    bool handled = false;
    if (!try_apply_protocol_send(frame, source, "deconstruct_keys", {keys_arg},
                                 &result, &handled)) {
      return std::nullopt;
    }
    if (!handled || result.is_null()) {
      return std::nullopt;
    }
    const std::optional<std::vector<MapEntry>> result_entries =
        extract_map_entries(frame, result);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (!result_entries.has_value()) {
      set_fault(frame, "TypeError", "deconstruct_keys returned non-map value");
      return std::nullopt;
    }
    return CoercedMapState{result, *result_entries};
  }

  bool complete_invoke_result(Frame &caller,
                              std::optional<std::uint32_t> caller_result_reg,
                              Value value) {
    if (caller_result_reg.has_value()) {
      if (!write_reg(caller, *caller_result_reg, std::move(value))) {
        return false;
      }
    } else {
      caller.last_result = std::move(value);
    }
    ++caller.pc;
    return true;
  }

  bool execute_clause_method(
      Frame &caller, const bytecode::BcMethod &method, const BcCode &entry_code,
      const std::vector<bytecode::MethodParamEntry> &params,
      const std::vector<BoundMethodArg> &slots, const Value &self,
      const Value &block, std::optional<std::uint32_t> caller_result_reg,
      const std::optional<Value> &return_override) {
    Frame callee;
    callee.code = &entry_code;
    callee.regs.assign(entry_code.reg_count, Value::null());
    callee.self = self;
    callee.block = block;

    if (!materialize_defaults(callee, method, params, slots) ||
        !apply_auto_assigns(callee, method, entry_code)) {
      return false;
    }

    const std::vector<Value> base_regs = callee.regs;
    for (const bytecode::ClauseEntry &entry : method.clause_table) {
      const NestedExecution pattern =
          execute_nested_code(entry.pattern_code_id, base_regs, self, block);
      if (!pattern.ok()) {
        fault_ = pattern.fault;
        return false;
      }
      if (!pattern.value.is_bool()) {
        set_fault(caller, "VMError",
                  "clause pattern probe did not return bool");
        return false;
      }
      if (!pattern.value.as_bool()) {
        continue;
      }

      std::vector<Value> matched_regs = base_regs;
      const std::size_t copy_count =
          std::min(matched_regs.size(), pattern.regs.size());
      for (std::size_t i = 0; i < copy_count; ++i) {
        matched_regs[i] = pattern.regs[i];
      }

      const NestedExecution guard =
          execute_nested_code(entry.guard_code_id, matched_regs, self, block);
      if (!guard.ok()) {
        fault_ = guard.fault;
        return false;
      }
      if (!is_truthy(guard.value)) {
        continue;
      }

      const NestedExecution body =
          execute_nested_code(entry.body_code_id, matched_regs, self, block);
      if (!body.ok()) {
        fault_ = body.fault;
        return false;
      }

      Value value = return_override.has_value() ? *return_override : body.value;
      return complete_invoke_result(caller, caller_result_reg,
                                    std::move(value));
    }

    const NestedExecution fallback =
        execute_nested_code(method.entry_code_id, base_regs, self, block);
    if (!fallback.ok()) {
      fault_ = fallback.fault;
      return false;
    }
    Value value =
        return_override.has_value() ? *return_override : fallback.value;
    return complete_invoke_result(caller, caller_result_reg, std::move(value));
  }

  bool
  invoke_method(Frame &caller, const bytecode::BcMethod &method,
                const std::vector<Value> &pos_args,
                const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
                Value self, Value block,
                std::optional<std::uint32_t> caller_result_reg,
                std::optional<Value> return_override = std::nullopt) {
    std::vector<bytecode::MethodParamEntry> params;
    std::vector<BoundMethodArg> slots;
    if (!shape_method_call(caller, method, pos_args, kw_args, &params,
                           &slots)) {
      return false;
    }
    const BcCode *code = find_code(module_, method.entry_code_id);
    if (code == nullptr) {
      set_fault(caller, "VMError", "method entry code id is unknown");
      return false;
    }
    if (!method.clause_table.empty()) {
      return execute_clause_method(caller, method, *code, params, slots, self,
                                   block, caller_result_reg, return_override);
    }
    const std::uint32_t call_pc = static_cast<std::uint32_t>(caller.pc);
    ++caller.pc;
    caller.active_call_pc = call_pc;
    push_frame(*code, {}, {}, std::move(self), std::move(block),
               caller_result_reg);
    Frame &callee = frames_.back();
    callee.return_override = std::move(return_override);
    if (!materialize_defaults(callee, method, params, slots)) {
      return false;
    }
    return apply_auto_assigns(callee, method, *code);
  }

  bool resolve_class_ref(const Frame &frame, std::uint32_t path_ref,
                         std::uint32_t *out_class_index) {
    if (path_ref >= module_.const_pool.size()) {
      set_fault(frame, "VMError", "class path ref is out of range");
      return false;
    }
    const Constant &constant = module_.const_pool[path_ref];
    if (constant.kind != ConstantKind::Path) {
      set_fault(frame, "VMError", "class ref must point to path constant");
      return false;
    }
    std::vector<std::string> segments;
    if (!path_segments_from_constant(frame, constant, &segments)) {
      return false;
    }
    return find_class_by_path_segments(frame, segments, out_class_index);
  }

  const bytecode::BcMethod *
  find_method_in_owner_table(const Frame &frame, std::uint32_t owner_index,
                             const std::string &selector,
                             std::uint32_t method_flags) {
    if (owner_index >= module_.classes.size()) {
      set_fault(frame, "VMError", "class dispatch ref is out of range");
      return nullptr;
    }
    if (owner_index >= state_->classes.size()) {
      set_fault(frame, "VMError", "runtime class state is out of range");
      return nullptr;
    }
    const ClassRuntimeState &runtime_owner = state_->classes[owner_index];
    if (!runtime_owner.method_range_valid) {
      set_fault(frame, "VMError", "class method range is out of range");
      return nullptr;
    }
    const MethodTableDescriptor &method_table =
        method_flags == kMethodFlagClass ? runtime_owner.class_method_table
                                         : runtime_owner.instance_method_table;
    for (const auto &[selector_id, method] : method_table.entries) {
      if (selector_id >= module_.symbols.size()) {
        set_fault(frame, "VMError", "method selector symbol ref is invalid");
        return nullptr;
      }
      if (module_.symbols[selector_id] == selector) {
        return &method;
      }
    }
    return nullptr;
  }

  const bytecode::BcMethod *
  find_method_in_mixin_chain(const Frame &frame, std::uint32_t mixin_index,
                             const std::string &selector,
                             std::vector<bool> *seen_mixins,
                             std::vector<bool> *active_mixins) {
    if (mixin_index >= module_.classes.size()) {
      set_fault(frame, "VMError", "mixin dispatch ref is out of range");
      return nullptr;
    }
    const bytecode::BcClass &mixin = module_.classes[mixin_index];
    if ((mixin.flags & amber::bytecode::kClassFlagMixin) == 0U) {
      set_fault(frame, "TypeError", "include/extend target is not a mixin");
      return nullptr;
    }
    if ((*active_mixins)[mixin_index]) {
      set_fault(frame, "IncludeCycleError",
                "cycle detected in mixin include graph");
      return nullptr;
    }
    if ((*seen_mixins)[mixin_index]) {
      return nullptr;
    }
    (*seen_mixins)[mixin_index] = true;
    (*active_mixins)[mixin_index] = true;

    const bytecode::BcMethod *found = find_method_in_owner_table(
        frame, mixin_index, selector, kMethodFlagInstance);
    if (found != nullptr || fault_.has_value()) {
      (*active_mixins)[mixin_index] = false;
      return found;
    }

    if (mixin_index >= state_->classes.size()) {
      set_fault(frame, "VMError", "runtime mixin state is out of range");
      (*active_mixins)[mixin_index] = false;
      return nullptr;
    }
    found = find_method_in_mixin_indices(
        frame, state_->classes[mixin_index].direct_include_indices, selector,
        seen_mixins, active_mixins);
    if (found != nullptr || fault_.has_value()) {
      (*active_mixins)[mixin_index] = false;
      return found;
    }

    for (auto ref = mixin.direct_include_refs.rbegin();
         ref != mixin.direct_include_refs.rend(); ++ref) {
      std::uint32_t included_index = 0;
      if (!resolve_class_ref(frame, *ref, &included_index)) {
        (*active_mixins)[mixin_index] = false;
        return nullptr;
      }
      found = find_method_in_mixin_chain(frame, included_index, selector,
                                         seen_mixins, active_mixins);
      if (found != nullptr || fault_.has_value()) {
        (*active_mixins)[mixin_index] = false;
        return found;
      }
    }

    (*active_mixins)[mixin_index] = false;
    return nullptr;
  }

  const bytecode::BcMethod *find_method_in_mixin_refs(
      const Frame &frame, const std::vector<std::uint32_t> &refs,
      const std::string &selector, std::vector<bool> *seen_mixins,
      std::vector<bool> *active_mixins) {
    for (auto ref = refs.rbegin(); ref != refs.rend(); ++ref) {
      std::uint32_t mixin_index = 0;
      if (!resolve_class_ref(frame, *ref, &mixin_index)) {
        return nullptr;
      }
      const bytecode::BcMethod *found = find_method_in_mixin_chain(
          frame, mixin_index, selector, seen_mixins, active_mixins);
      if (found != nullptr || fault_.has_value()) {
        return found;
      }
    }
    return nullptr;
  }

  const bytecode::BcMethod *find_method_in_mixin_indices(
      const Frame &frame, const std::vector<std::uint32_t> &indices,
      const std::string &selector, std::vector<bool> *seen_mixins,
      std::vector<bool> *active_mixins) {
    for (auto index = indices.rbegin(); index != indices.rend(); ++index) {
      const bytecode::BcMethod *found = find_method_in_mixin_chain(
          frame, *index, selector, seen_mixins, active_mixins);
      if (found != nullptr || fault_.has_value()) {
        return found;
      }
    }
    return nullptr;
  }

  const bytecode::BcMethod *
  find_method_for_dispatch_impl(const Frame &frame, std::uint32_t class_index,
                                const std::string &selector, bool class_side,
                                std::vector<bool> *seen_mixins,
                                std::vector<bool> *active_mixins,
                                std::vector<bool> *active_classes) {
    if (class_index >= module_.classes.size()) {
      set_fault(frame, "VMError", "class dispatch ref is out of range");
      return nullptr;
    }
    if ((*active_classes)[class_index]) {
      set_fault(frame, "VMError", "cycle detected in superclass chain");
      return nullptr;
    }
    (*active_classes)[class_index] = true;

    const bytecode::BcMethod *found = find_method_in_owner_table(
        frame, class_index, selector,
        class_side ? kMethodFlagClass : kMethodFlagInstance);
    if (found != nullptr || fault_.has_value()) {
      (*active_classes)[class_index] = false;
      return found;
    }

    const bytecode::BcClass &klass = module_.classes[class_index];
    if (class_index >= state_->classes.size()) {
      set_fault(frame, "VMError", "runtime class state is out of range");
      (*active_classes)[class_index] = false;
      return nullptr;
    }
    const std::vector<std::uint32_t> &dynamic_mixins =
        class_side ? state_->classes[class_index].direct_extend_indices
                   : state_->classes[class_index].direct_include_indices;
    found = find_method_in_mixin_indices(frame, dynamic_mixins, selector,
                                         seen_mixins, active_mixins);
    if (found != nullptr || fault_.has_value()) {
      (*active_classes)[class_index] = false;
      return found;
    }

    found = find_method_in_mixin_refs(frame,
                                      class_side ? klass.direct_extend_refs
                                                 : klass.direct_include_refs,
                                      selector, seen_mixins, active_mixins);
    if (found != nullptr || fault_.has_value()) {
      (*active_classes)[class_index] = false;
      return found;
    }

    if (klass.has_superclass_ref) {
      std::uint32_t superclass_index = 0;
      if (!resolve_class_ref(frame, klass.superclass_ref, &superclass_index)) {
        (*active_classes)[class_index] = false;
        return nullptr;
      }
      found = find_method_for_dispatch_impl(frame, superclass_index, selector,
                                            class_side, seen_mixins,
                                            active_mixins, active_classes);
      (*active_classes)[class_index] = false;
      return found;
    }

    (*active_classes)[class_index] = false;
    return nullptr;
  }

  const bytecode::BcMethod *
  find_method_for_dispatch(const Frame &frame, std::uint32_t class_index,
                           const std::string &selector,
                           std::uint32_t expected_flags) {
    std::vector<bool> seen_mixins(module_.classes.size(), false);
    std::vector<bool> active_mixins(module_.classes.size(), false);
    std::vector<bool> active_classes(module_.classes.size(), false);
    return find_method_for_dispatch_impl(
        frame, class_index, selector, expected_flags == kMethodFlagClass,
        &seen_mixins, &active_mixins, &active_classes);
  }

  bool try_invoke_method_missing(
      Frame &frame, std::uint32_t class_index, std::uint32_t dispatch_flags,
      const std::string &selector_text,
      const std::optional<Value> &selector_value,
      const std::vector<Value> &pos_args,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      const Value &receiver, const Value &block, std::uint32_t dst) {
    if (selector_text == "method_missing") {
      return false;
    }
    const bytecode::BcMethod *method = find_method_for_dispatch(
        frame, class_index, "method_missing", dispatch_flags);
    if (fault_.has_value() || method == nullptr) {
      return false;
    }
    std::vector<Value> fallback_args;
    fallback_args.reserve(pos_args.size() + 1);
    fallback_args.push_back(
        selector_value_for_method_missing(selector_text, selector_value));
    for (const Value &arg : pos_args) {
      fallback_args.push_back(arg);
    }
    return invoke_method(frame, *method, fallback_args, kw_args, receiver,
                         block, dst);
  }

  const bytecode::HandlerEntry *find_handler_for_pc(const Frame &frame,
                                                    std::uint32_t pc) const {
    if (frame.code == nullptr) {
      return nullptr;
    }
    const bytecode::HandlerEntry *best = nullptr;
    std::uint32_t best_width = std::numeric_limits<std::uint32_t>::max();
    for (const bytecode::HandlerEntry &entry : frame.code->handler_table) {
      if (entry.protected_from <= pc && pc < entry.protected_to) {
        const std::uint32_t width = entry.protected_to - entry.protected_from;
        if (best == nullptr || width < best_width) {
          best = &entry;
          best_width = width;
        }
      }
    }
    return best;
  }

  bool find_unwind_target(std::size_t *frame_index,
                          const bytecode::HandlerEntry **handler) const {
    for (std::size_t index = frames_.size(); index > 0; --index) {
      const std::size_t candidate = index - 1U;
      const Frame &frame = frames_[candidate];
      const std::uint32_t pc =
          frame.active_call_pc.value_or(static_cast<std::uint32_t>(frame.pc));
      const bytecode::HandlerEntry *entry = find_handler_for_pc(frame, pc);
      if (entry != nullptr) {
        *frame_index = candidate;
        *handler = entry;
        return true;
      }
    }
    return false;
  }

  bool raise_value(const Frame &raising_frame, const Value &exception) {
    std::size_t target_index = 0;
    const bytecode::HandlerEntry *handler = nullptr;
    if (!find_unwind_target(&target_index, &handler)) {
      const std::string error_name = exception_error_name(exception);
      const std::string message =
          "unhandled exception " + value_to_debug_string(exception, &module_);
      fault_ = make_fault(raising_frame, error_name, message);
      return false;
    }

    const BcCode *handler_code = find_code(module_, handler->handler_code_id);
    if (handler_code == nullptr) {
      set_fault(raising_frame, "VMError", "handler code id is unknown");
      return false;
    }
    if (target_index >= frames_.size()) {
      set_fault(raising_frame, "VMError", "handler frame is out of range");
      return false;
    }

    while (frames_.size() > target_index + 1U) {
      frames_.pop_back();
    }

    Frame &target = frames_[target_index];
    if (target.code == nullptr ||
        handler->handler_pc >= target.code->instructions.size()) {
      set_fault(target, "VMError", "handler pc is out of range");
      return false;
    }

    clear_pattern_state(target);
    target.pending_pattern_bindings.clear();
    target.active_call_pc.reset();
    target.pc = handler->handler_pc;
    push_frame(*handler_code, {exception}, target.captures, target.self,
               target.block, 0U);
    return true;
  }

  bool is_pattern_opcode(Opcode opcode) const {
    switch (opcode) {
    case Opcode::PPrepSeq:
    case Opcode::PPrepMap:
    case Opcode::PCheckEq:
    case Opcode::PCheckPin:
    case Opcode::PCheckLenEq:
    case Opcode::PCheckLenGte:
    case Opcode::PGetIndex:
    case Opcode::PHasKey:
    case Opcode::PGetKey:
    case Opcode::PTripleEq:
    case Opcode::PBind:
    case Opcode::PCommit:
    case Opcode::PFail:
      return true;
    default:
      return false;
    }
  }

  SendStatus try_apply_scalar_send(const Frame &frame, const Value &receiver,
                                   const std::string &selector,
                                   const std::vector<Value> &args, Value *out) {
    if (!ensure_lifecycle_access(frame, receiver)) {
      return SendStatus::Faulted;
    }

    auto require_arity = [&](std::size_t expected) -> bool {
      if (args.size() != expected) {
        set_fault(frame, "TypeError", "wrong builtin SEND arity");
        return false;
      }
      return true;
    };

    auto require_integer_arg = [&](std::size_t index,
                                   std::int64_t *value) -> bool {
      if (index >= args.size() || !args[index].is_integer()) {
        set_fault(frame, "TypeError", "builtin SEND expects integer argument");
        return false;
      }
      *value = args[index].as_integer();
      return true;
    };

    if (selector == "==" || selector == "===") {
      if (!require_arity(1)) {
        return SendStatus::Faulted;
      }
      if (!ensure_lifecycle_access(frame, args[0])) {
        return SendStatus::Faulted;
      }
      *out = Value::boolean(value_equals(receiver, args[0]));
      return SendStatus::Matched;
    }

    if (receiver.is_list() || receiver.is_tuple()) {
      std::vector<Value> items;
      bool source_was_tuple = false;
      const std::optional<std::vector<Value>> extracted =
          extract_sequence_items(frame, receiver, &source_was_tuple);
      if (fault_.has_value()) {
        return SendStatus::Faulted;
      }
      if (extracted.has_value()) {
        items = *extracted;
        if (selector == "empty?") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          *out = Value::boolean(items.empty());
          return SendStatus::Matched;
        }
        if (selector == "[]") {
          std::int64_t index = 0;
          if (!require_arity(1) || !require_integer_arg(0, &index)) {
            return SendStatus::Faulted;
          }
          if (index < 0 || static_cast<std::size_t>(index) >= items.size()) {
            *out = Value::null();
          } else {
            *out = items[static_cast<std::size_t>(index)];
          }
          return SendStatus::Matched;
        }
        if (selector == "deconstruct") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          *out = receiver;
          return SendStatus::Matched;
        }
      }
    }

    if (receiver.is_map()) {
      const std::optional<std::vector<MapEntry>> extracted =
          extract_map_entries(frame, receiver);
      if (fault_.has_value()) {
        return SendStatus::Faulted;
      }
      if (extracted.has_value()) {
        if (selector == "empty?") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          *out = Value::boolean(extracted->empty());
          return SendStatus::Matched;
        }
        if (selector == "[]") {
          if (!require_arity(1)) {
            return SendStatus::Faulted;
          }
          std::optional<std::uint32_t> key_symbol_id;
          if (args[0].is_symbol()) {
            key_symbol_id = args[0].as_symbol().symbol_id;
          } else if (args[0].is_string()) {
            const std::optional<std::string> text =
                string_text_from_id(args[0].as_string().string_id);
            if (!text.has_value()) {
              set_fault(frame, "VMError", "map index string ref is invalid");
              return SendStatus::Faulted;
            }
            key_symbol_id = symbol_id_for_text(*text);
          } else {
            set_fault(frame, "TypeError",
                      "map index expects Symbol or String key");
            return SendStatus::Faulted;
          }
          if (!key_symbol_id.has_value()) {
            *out = Value::null();
            return SendStatus::Matched;
          }
          Value found = Value::null();
          for (const MapEntry &entry : *extracted) {
            if (entry.symbol_id == *key_symbol_id) {
              found = entry.value;
              break;
            }
          }
          *out = found;
          return SendStatus::Matched;
        }
        if (selector == "deconstruct_keys") {
          if (!require_arity(1)) {
            return SendStatus::Faulted;
          }
          bool keyset_is_valid = false;
          if (args[0].is_tuple() || args[0].is_list()) {
            bool keyset_source_was_tuple = false;
            const std::optional<std::vector<Value>> keyset_items =
                extract_sequence_items(frame, args[0],
                                       &keyset_source_was_tuple);
            if (fault_.has_value()) {
              return SendStatus::Faulted;
            }
            keyset_is_valid = keyset_items.has_value();
            if (keyset_items.has_value()) {
              for (const Value &item : *keyset_items) {
                if (!item.is_symbol()) {
                  keyset_is_valid = false;
                  break;
                }
              }
            }
          }
          if (!keyset_is_valid) {
            set_fault(frame, "TypeError",
                      "deconstruct_keys expects Symbol tuple/list keyset");
            return SendStatus::Faulted;
          }
          *out = receiver;
          return SendStatus::Matched;
        }
      }
    }

    if (receiver.is_integer()) {
      const std::int64_t lhs = receiver.as_integer();
      std::int64_t rhs = 0;
      if (selector == "+") {
        if (!require_arity(1) || !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs + rhs);
        return SendStatus::Matched;
      }
      if (selector == "-") {
        if (!require_arity(1) || !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs - rhs);
        return SendStatus::Matched;
      }
      if (selector == "*") {
        if (!require_arity(1) || !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs * rhs);
        return SendStatus::Matched;
      }
      if (selector == "/") {
        if (!require_arity(1) || !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        if (rhs == 0) {
          set_fault(frame, "TypeError", "division by zero");
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs / rhs);
        return SendStatus::Matched;
      }
      if (selector == ">") {
        if (!require_arity(1) || !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs > rhs);
        return SendStatus::Matched;
      }
      if (selector == "<") {
        if (!require_arity(1) || !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs < rhs);
        return SendStatus::Matched;
      }
      if (selector == ">=") {
        if (!require_arity(1) || !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs >= rhs);
        return SendStatus::Matched;
      }
      if (selector == "<=") {
        if (!require_arity(1) || !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs <= rhs);
        return SendStatus::Matched;
      }
    }

    return SendStatus::NotHandled;
  }

  bool step_send(Frame &frame, const Instruction &insn, bool dynamic_selector) {
    std::uint32_t dst = 0;
    std::uint32_t recv_reg = 0;
    if (!operand_u32(frame, insn, 0, &dst) ||
        !operand_u32(frame, insn, 1, &recv_reg)) {
      return false;
    }

    std::size_t operand_index = 2;
    std::optional<std::string> selector;
    std::optional<Value> selector_value;
    std::optional<std::uint32_t> selector_symbol_id_for_cache;
    if (dynamic_selector) {
      std::uint32_t selector_reg = 0;
      if (!operand_u32(frame, insn, operand_index++, &selector_reg)) {
        return false;
      }
      selector_value = read_reg(frame, selector_reg);
      selector = selector_text_from_value(*selector_value);
      if (fault_.has_value()) {
        return false;
      }
      if (!selector.has_value()) {
        set_fault(frame, "TypeError",
                  "SEND_DYN expects Symbol or String selector");
        return false;
      }
      if (selector_value->is_symbol()) {
        selector_symbol_id_for_cache = selector_value->as_symbol().symbol_id;
      } else {
        selector_symbol_id_for_cache = symbol_id_for_text(*selector);
      }
    } else {
      std::uint32_t selector_id = 0;
      if (!operand_u32(frame, insn, operand_index++, &selector_id)) {
        return false;
      }
      selector_value = Value::symbol(selector_id);
      selector_symbol_id_for_cache = selector_id;
      selector = selector_text_from_symbol(selector_id);
      if (!selector.has_value()) {
        set_fault(frame, "VMError", "selector symbol ref is out of range");
        return false;
      }
    }

    std::uint32_t pos_count = 0;
    if (!operand_u32(frame, insn, operand_index++, &pos_count)) {
      return false;
    }
    std::vector<Value> args;
    args.reserve(pos_count);
    for (std::uint32_t i = 0; i < pos_count; ++i) {
      std::uint32_t reg = 0;
      if (!operand_u32(frame, insn, operand_index++, &reg)) {
        return false;
      }
      args.push_back(read_reg(frame, reg));
      if (fault_.has_value()) {
        return false;
      }
    }

    std::uint32_t kw_count = 0;
    if (!operand_u32(frame, insn, operand_index++, &kw_count)) {
      return false;
    }
    std::vector<std::pair<std::uint32_t, Value>> kw_args;
    kw_args.reserve(kw_count);
    for (std::uint32_t i = 0; i < kw_count; ++i) {
      std::uint32_t name_symbol_id = 0;
      std::uint32_t reg = 0;
      if (!operand_u32(frame, insn, operand_index++, &name_symbol_id) ||
          !operand_u32(frame, insn, operand_index++, &reg)) {
        return false;
      }
      kw_args.push_back({name_symbol_id, read_reg(frame, reg)});
      if (fault_.has_value()) {
        return false;
      }
    }

    std::int64_t block_reg = -1;
    if (!operand_i64(frame, insn, operand_index++, &block_reg)) {
      return false;
    }
    const std::optional<std::uint32_t> site_id =
        optional_operand_u32(frame, insn, operand_index++);
    if (fault_.has_value()) {
      return false;
    }
    const Value block =
        has_optional_reg(block_reg)
            ? read_reg(frame, static_cast<std::uint32_t>(block_reg))
            : Value::null();
    if (fault_.has_value()) {
      return false;
    }

    const Value receiver = read_reg(frame, recv_reg);
    if (fault_.has_value()) {
      return false;
    }
    if (*selector == "destroy!") {
      if (!args.empty() || !kw_args.empty() || !block.is_null()) {
        set_fault(frame, "TypeError", "destroy! accepts no arguments");
        return false;
      }
      bool changed = false;
      if (!lifecycle_destroy(frame, receiver, &changed) || fault_.has_value()) {
        return false;
      }
      if (!write_reg(frame, dst, Value::boolean(changed))) {
        return false;
      }
      ++frame.pc;
      return true;
    }
    Value result = Value::null();
    const SendStatus scalar_status =
        try_apply_scalar_send(frame, receiver, *selector, args, &result);
    if (scalar_status == SendStatus::Faulted) {
      return false;
    }
    if (scalar_status == SendStatus::Matched) {
      if (!block.is_null()) {
        set_fault(frame, "TypeError",
                  "builtin SEND does not accept block arguments");
        return false;
      }
      if (!kw_args.empty()) {
        set_fault(frame, "TypeError",
                  "builtin SEND does not accept keyword arguments");
        return false;
      }
      if (!write_reg(frame, dst, std::move(result))) {
        return false;
      }
      ++frame.pc;
      return true;
    }

    std::uint32_t class_index = 0;
    std::uint32_t dispatch_flags = 0;
    if (receiver.is_instance_object()) {
      const std::shared_ptr<InstanceValue> instance =
          receiver.as_instance_object();
      if (instance == nullptr) {
        set_fault(frame, "TypeError", "instance receiver is null");
        return false;
      }
      class_index = instance->class_index;
      dispatch_flags = kMethodFlagInstance;
    } else if (receiver.is_class_object()) {
      class_index = receiver.as_class_object().class_index;
      dispatch_flags = kMethodFlagClass;
    } else {
      set_fault(frame, "NoMethodError",
                "selector is not implemented in current runtime baseline");
      return false;
    }

    if (site_id.has_value() && selector_symbol_id_for_cache.has_value()) {
      const bytecode::BcMethod *cached =
          probe_call_cache(frame, *site_id, class_index, dispatch_flags,
                           *selector_symbol_id_for_cache);
      if (cached != nullptr) {
        return invoke_method(frame, *cached, args, kw_args, receiver, block,
                             dst);
      }
    }

    const bytecode::BcMethod *method =
        find_method_for_dispatch(frame, class_index, *selector, dispatch_flags);
    if (fault_.has_value()) {
      return false;
    }
    if (method == nullptr) {
      if (try_invoke_method_missing(frame, class_index, dispatch_flags,
                                    *selector, selector_value, args, kw_args,
                                    receiver, block, dst)) {
        return true;
      }
      if (fault_.has_value()) {
        return false;
      }
      set_fault(frame, "NoMethodError",
                "selector is not implemented in current runtime baseline");
      return false;
    }
    if (site_id.has_value() && selector_symbol_id_for_cache.has_value()) {
      update_call_cache(frame, *site_id, class_index, dispatch_flags,
                        *selector_symbol_id_for_cache, *method);
    }
    return invoke_method(frame, *method, args, kw_args, receiver, block, dst);
  }

  void step() {
    Frame &frame = frames_.back();
    if (frame.code == nullptr || frame.pc >= frame.code->instructions.size()) {
      set_fault(frame, "VMError", "program counter out of range");
      return;
    }

    const Instruction &insn = frame.code->instructions[frame.pc];
    if (!is_pattern_opcode(insn.opcode) && has_active_pattern_state(frame)) {
      if (!finalize_pattern_success(frame, false)) {
        return;
      }
      if (frame.pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "program counter out of range");
        return;
      }
    }
    switch (insn.opcode) {
    case Opcode::LoadK: {
      std::uint32_t dst = 0;
      std::uint32_t const_id = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &const_id)) {
        return;
      }
      if (!write_reg(frame, dst, load_constant(frame, const_id))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadNull: {
      std::uint32_t dst = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !write_reg(frame, dst, Value::null())) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadBool: {
      std::uint32_t dst = 0;
      std::int64_t raw = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_i64(frame, insn, 1, &raw) ||
          !write_reg(frame, dst, Value::boolean(raw != 0))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::Move: {
      std::uint32_t dst = 0;
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &src)) {
        return;
      }
      if (!write_reg(frame, dst, read_reg(frame, src))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::MakeList:
    case Opcode::MakeTuple: {
      std::uint32_t dst = 0;
      std::uint32_t first_reg = 0;
      std::uint32_t count = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &first_reg) ||
          !operand_u32(frame, insn, 2, &count)) {
        return;
      }
      std::vector<Value> items;
      items.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        items.push_back(read_reg(frame, first_reg + i));
        if (fault_.has_value()) {
          return;
        }
      }
      const Value value = insn.opcode == Opcode::MakeTuple
                              ? make_tuple_value(std::move(items))
                              : make_list_value(std::move(items));
      if (!write_reg(frame, dst, value)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::MakeMap: {
      std::uint32_t dst = 0;
      std::uint32_t count = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &count)) {
        return;
      }
      std::size_t operand_index = 2;
      std::vector<MapEntry> entries;
      entries.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t symbol_id = 0;
        std::uint32_t reg = 0;
        if (!operand_u32(frame, insn, operand_index++, &symbol_id) ||
            !operand_u32(frame, insn, operand_index++, &reg)) {
          return;
        }
        entries.push_back({symbol_id, read_reg(frame, reg)});
        if (fault_.has_value()) {
          return;
        }
      }
      if (!write_reg(frame, dst, make_symbol_map_value(std::move(entries)))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::Freeze: {
      std::uint32_t dst = 0;
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &src)) {
        return;
      }
      const Value value = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      if (value.is_list()) {
        const std::shared_ptr<ListValue> list = value.as_list();
        if (list == nullptr) {
          set_fault(frame, "TypeError", "list value is null");
          return;
        }
        if (!ensure_lifecycle_access(frame, value)) {
          return;
        }
        if (!write_reg(frame, dst, make_list_value(list->items, true))) {
          return;
        }
      } else if (value.is_map()) {
        const std::shared_ptr<MapValue> map = value.as_map();
        if (map == nullptr) {
          set_fault(frame, "TypeError", "map value is null");
          return;
        }
        if (!ensure_lifecycle_access(frame, value)) {
          return;
        }
        if (!write_reg(frame, dst, make_symbol_map_value(map->entries, true))) {
          return;
        }
      } else {
        if (!write_reg(frame, dst, value)) {
          return;
        }
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadSelf: {
      std::uint32_t dst = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !write_reg(frame, dst, frame.self)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadIvar: {
      std::uint32_t dst = 0;
      std::uint32_t recv_reg = 0;
      std::uint32_t symbol_id = 0;
      std::uint32_t site_id = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &recv_reg) ||
          !operand_u32(frame, insn, 2, &symbol_id) ||
          !operand_u32(frame, insn, 3, &site_id)) {
        return;
      }
      const Value receiver = read_reg(frame, recv_reg);
      if (fault_.has_value()) {
        return;
      }
      std::shared_ptr<InstanceValue> instance;
      if (!expect_instance_receiver(frame, receiver, &instance)) {
        return;
      }
      std::optional<std::uint32_t> slot =
          probe_ivar_cache(frame, site_id, *instance, symbol_id);
      if (!slot.has_value()) {
        const std::optional<std::string> ivar_name =
            selector_text_from_symbol(symbol_id);
        if (!ivar_name.has_value()) {
          set_fault(frame, "VMError", "ivar symbol ref is out of range");
          return;
        }
        slot = ensure_instance_ivar_slot(frame, instance, *ivar_name);
        if (fault_.has_value()) {
          return;
        }
        if (slot.has_value()) {
          update_ivar_cache(frame, site_id, *instance, symbol_id, *slot);
        }
      }
      const Value value =
          slot.has_value() && *slot < instance->ivar_storage.size()
              ? instance->ivar_storage[*slot]
              : Value::null();
      if (!write_reg(frame, dst, value)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadCvar: {
      std::uint32_t dst = 0;
      std::uint32_t owner_reg = 0;
      std::uint32_t symbol_id = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &owner_reg) ||
          !operand_u32(frame, insn, 2, &symbol_id)) {
        return;
      }
      const Value owner = read_reg(frame, owner_reg);
      if (fault_.has_value()) {
        return;
      }
      std::uint32_t class_index = 0;
      if (!expect_class_owner(frame, owner, &class_index)) {
        return;
      }
      const std::optional<std::string> cvar_name =
          selector_text_from_symbol(symbol_id);
      if (!cvar_name.has_value()) {
        set_fault(frame, "VMError", "cvar symbol ref is out of range");
        return;
      }
      const auto cvar = state_->classes[class_index].cvars.find(*cvar_name);
      const Value value = cvar == state_->classes[class_index].cvars.end()
                              ? Value::null()
                              : cvar->second;
      if (!write_reg(frame, dst, value)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::StoreIvar: {
      std::uint32_t recv_reg = 0;
      std::uint32_t symbol_id = 0;
      std::uint32_t src = 0;
      std::uint32_t site_id = 0;
      if (!operand_u32(frame, insn, 0, &recv_reg) ||
          !operand_u32(frame, insn, 1, &symbol_id) ||
          !operand_u32(frame, insn, 2, &src) ||
          !operand_u32(frame, insn, 3, &site_id)) {
        return;
      }
      const Value receiver = read_reg(frame, recv_reg);
      if (fault_.has_value()) {
        return;
      }
      std::shared_ptr<InstanceValue> instance;
      if (!expect_instance_receiver(frame, receiver, &instance)) {
        return;
      }
      std::optional<std::uint32_t> cached_slot =
          probe_ivar_cache(frame, site_id, *instance, symbol_id);
      (void)cached_slot;
      const std::optional<std::string> ivar_name =
          selector_text_from_symbol(symbol_id);
      if (!ivar_name.has_value()) {
        set_fault(frame, "VMError", "ivar symbol ref is out of range");
        return;
      }
      const Value value = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      if (!store_instance_ivar(frame, instance, *ivar_name, value, symbol_id,
                               site_id)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::StoreCvar: {
      std::uint32_t owner_reg = 0;
      std::uint32_t symbol_id = 0;
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &owner_reg) ||
          !operand_u32(frame, insn, 1, &symbol_id) ||
          !operand_u32(frame, insn, 2, &src)) {
        return;
      }
      const Value owner = read_reg(frame, owner_reg);
      if (fault_.has_value()) {
        return;
      }
      std::uint32_t class_index = 0;
      if (!expect_class_owner(frame, owner, &class_index)) {
        return;
      }
      const std::optional<std::string> cvar_name =
          selector_text_from_symbol(symbol_id);
      if (!cvar_name.has_value()) {
        set_fault(frame, "VMError", "cvar symbol ref is out of range");
        return;
      }
      state_->classes[class_index].cvars[*cvar_name] = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LookupConst: {
      std::uint32_t dst = 0;
      std::uint32_t const_id = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &const_id)) {
        return;
      }
      if (!write_reg(frame, dst, lookup_constant(frame, const_id))) {
        return;
      }
      if (fault_.has_value()) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::GetLast: {
      std::uint32_t dst = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !write_reg(frame, dst, frame.last_result)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::SetLast: {
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &src)) {
        return;
      }
      frame.last_result = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadUpval: {
      std::uint32_t dst = 0;
      std::uint32_t slot = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &slot)) {
        return;
      }
      if (slot >= frame.captures.size()) {
        set_fault(frame, "VMError", "capture slot out of range");
        return;
      }
      if (!write_reg(frame, dst, frame.captures[slot])) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::StoreUpval: {
      std::uint32_t slot = 0;
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &slot) ||
          !operand_u32(frame, insn, 1, &src)) {
        return;
      }
      if (slot >= frame.captures.size()) {
        set_fault(frame, "VMError", "capture slot out of range");
        return;
      }
      frame.captures[slot] = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::MakeClosure: {
      std::uint32_t dst = 0;
      std::uint32_t code_id = 0;
      std::uint32_t capture_count = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &code_id) ||
          !operand_u32(frame, insn, 2, &capture_count)) {
        return;
      }
      std::size_t operand_index = 3;
      auto closure = make_closure_value();
      closure->code_id = code_id;
      closure->self = frame.self;
      closure->captures.reserve(capture_count);
      for (std::uint32_t i = 0; i < capture_count; ++i) {
        std::uint32_t kind = 0;
        std::uint32_t slot = 0;
        if (!operand_u32(frame, insn, operand_index++, &kind) ||
            !operand_u32(frame, insn, operand_index++, &slot)) {
          return;
        }
        if (kind == 0U) {
          closure->captures.push_back(read_reg(frame, slot));
        } else {
          if (slot >= frame.captures.size()) {
            set_fault(frame, "VMError", "capture slot out of range");
            return;
          }
          closure->captures.push_back(frame.captures[slot]);
        }
        if (fault_.has_value()) {
          return;
        }
      }
      if (!write_reg(frame, dst, Value::closure(std::move(closure)))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::ObjDestroy:
    case Opcode::ObjDealloc: {
      std::uint32_t dst = 0;
      std::uint32_t obj_reg = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &obj_reg)) {
        return;
      }
      const Value object = read_reg(frame, obj_reg);
      if (fault_.has_value()) {
        return;
      }
      bool changed = false;
      const bool ok = insn.opcode == Opcode::ObjDestroy
                          ? lifecycle_destroy(frame, object, &changed)
                          : lifecycle_dealloc(frame, object, &changed);
      if (!ok || fault_.has_value()) {
        return;
      }
      if (!write_reg(frame, dst, Value::boolean(changed))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::Call: {
      std::uint32_t dst = 0;
      std::uint32_t callee_reg = 0;
      std::uint32_t pos_count = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &callee_reg) ||
          !operand_u32(frame, insn, 2, &pos_count)) {
        return;
      }
      std::size_t operand_index = 3;
      std::vector<Value> args;
      args.reserve(pos_count);
      for (std::uint32_t i = 0; i < pos_count; ++i) {
        std::uint32_t reg = 0;
        if (!operand_u32(frame, insn, operand_index++, &reg)) {
          return;
        }
        args.push_back(read_reg(frame, reg));
        if (fault_.has_value()) {
          return;
        }
      }

      std::uint32_t kw_count = 0;
      if (!operand_u32(frame, insn, operand_index++, &kw_count)) {
        return;
      }
      std::vector<std::pair<std::uint32_t, Value>> kw_args;
      kw_args.reserve(kw_count);
      for (std::uint32_t i = 0; i < kw_count; ++i) {
        std::uint32_t name_symbol_id = 0;
        std::uint32_t reg = 0;
        if (!operand_u32(frame, insn, operand_index++, &name_symbol_id) ||
            !operand_u32(frame, insn, operand_index++, &reg)) {
          return;
        }
        kw_args.push_back({name_symbol_id, read_reg(frame, reg)});
        if (fault_.has_value()) {
          return;
        }
      }

      std::int64_t block_reg = -1;
      if (!operand_i64(frame, insn, operand_index++, &block_reg)) {
        return;
      }

      Value callee = read_reg(frame, callee_reg);
      if (fault_.has_value()) {
        return;
      }
      const Value block =
          has_optional_reg(block_reg)
              ? read_reg(frame, static_cast<std::uint32_t>(block_reg))
              : Value::null();
      if (fault_.has_value()) {
        return;
      }

      if (callee.is_closure()) {
        if (!kw_args.empty()) {
          set_fault(frame, "TypeError",
                    "closure CALL does not accept keyword arguments");
          return;
        }
        if (!ensure_lifecycle_access(frame, callee)) {
          return;
        }
        const std::shared_ptr<ClosureValue> closure = callee.as_closure();
        if (closure == nullptr) {
          set_fault(frame, "TypeError", "closure value is null");
          return;
        }
        const BcCode *code = find_code(module_, closure->code_id);
        if (code == nullptr) {
          set_fault(frame, "VMError", "closure code id is unknown");
          return;
        }
        const std::uint32_t call_pc = static_cast<std::uint32_t>(frame.pc);
        ++frame.pc;
        frame.active_call_pc = call_pc;
        push_frame(*code, args, closure->captures, closure->self, block, dst);
        return;
      }

      if (callee.is_class_object()) {
        const std::uint32_t class_index = callee.as_class_object().class_index;
        auto instance = make_instance_value(class_index);
        if (!ensure_instance_layout(frame, instance)) {
          return;
        }
        const Value instance_value = Value::instance(instance);
        const bytecode::BcMethod *init = find_method_for_dispatch(
            frame, class_index, "init", kMethodFlagInstance);
        if (fault_.has_value()) {
          return;
        }
        if (init == nullptr) {
          if (!args.empty()) {
            set_fault(
                frame, "TypeError",
                "class call without init accepts no positional arguments");
            return;
          }
          if (!kw_args.empty()) {
            set_fault(
                frame, "TypeError",
                "class call without init does not accept keyword arguments");
            return;
          }
          if (!block.is_null()) {
            set_fault(frame, "TypeError",
                      "class call without init does not accept block");
            return;
          }
          if (!write_reg(frame, dst, instance_value)) {
            return;
          }
          ++frame.pc;
          return;
        }
        if (!invoke_method(frame, *init, args, kw_args, instance_value, block,
                           dst, instance_value)) {
          return;
        }
        return;
      }

      set_fault(frame, "TypeError",
                "CALL expects closure or class object in current runtime");
      return;
    }
    case Opcode::Send:
      step_send(frame, insn, false);
      return;
    case Opcode::SendDyn:
      step_send(frame, insn, true);
      return;
    case Opcode::Jump: {
      std::uint32_t target = 0;
      if (!operand_u32(frame, insn, 0, &target)) {
        return;
      }
      if (target >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "jump target out of range");
        return;
      }
      frame.pc = target;
      return;
    }
    case Opcode::JumpIfTrue:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull: {
      std::uint32_t cond_reg = 0;
      std::uint32_t target = 0;
      if (!operand_u32(frame, insn, 0, &cond_reg) ||
          !operand_u32(frame, insn, 1, &target)) {
        return;
      }
      if (target >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "jump target out of range");
        return;
      }
      const Value cond = read_reg(frame, cond_reg);
      if (fault_.has_value()) {
        return;
      }
      const bool take =
          insn.opcode == Opcode::JumpIfTrue
              ? is_truthy(cond)
              : (insn.opcode == Opcode::JumpIfFalse ? !is_truthy(cond)
                                                    : cond.is_null());
      frame.pc = take ? target : frame.pc + 1U;
      return;
    }
    case Opcode::PPrepSeq: {
      std::uint32_t dst = 0;
      std::uint32_t src_reg = 0;
      std::uint32_t ignored_mode = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &src_reg) ||
          !operand_u32(frame, insn, 2, &ignored_mode) ||
          !operand_u32(frame, insn, 3, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const Value source = read_reg(frame, src_reg);
      if (fault_.has_value()) {
        return;
      }
      const std::optional<CoercedSeqState> sequence =
          coerce_sequence_for_pattern(frame, source);
      if (fault_.has_value()) {
        return;
      }
      if (!sequence.has_value()) {
        clear_pattern_state(frame);
        frame.pc = fail_pc;
        return;
      }
      if (!write_reg(frame, dst, sequence->value)) {
        return;
      }
      frame.prepared_seq_regs[dst] =
          PreparedSeqState{sequence->items, 0U, sequence->source_was_tuple};
      ++frame.pc;
      (void)ignored_mode;
      return;
    }
    case Opcode::PPrepMap: {
      std::uint32_t dst = 0;
      std::uint32_t src_reg = 0;
      std::uint32_t keyset_id = 0;
      std::uint32_t needs_full = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &src_reg) ||
          !operand_u32(frame, insn, 2, &keyset_id) ||
          !operand_u32(frame, insn, 3, &needs_full) ||
          !operand_u32(frame, insn, 4, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      if (keyset_id >= module_.const_pool.size()) {
        set_fault(frame, "VMError", "keyset ref is out of range");
        return;
      }
      const Constant &keyset = module_.const_pool[keyset_id];
      if (keyset.kind != ConstantKind::KeySet) {
        set_fault(frame, "VMError", "P_PREP_MAP expects keyset constant");
        return;
      }
      const Value source = read_reg(frame, src_reg);
      if (fault_.has_value()) {
        return;
      }
      const std::optional<CoercedMapState> map =
          coerce_map_for_pattern(frame, source, keyset, needs_full != 0U);
      if (fault_.has_value()) {
        return;
      }
      if (!map.has_value()) {
        clear_pattern_state(frame);
        frame.pc = fail_pc;
        return;
      }
      PreparedMapState state;
      state.entries = map->entries;
      state.requested_keys = keyset.items;
      state.needs_full = needs_full != 0U;
      state.fail_pc = fail_pc;
      for (std::size_t i = 0; i < state.entries.size(); ++i) {
        state.index_by_key[state.entries[i].symbol_id] = i;
      }
      if (!write_reg(frame, dst, map->value)) {
        return;
      }
      frame.prepared_map_regs[dst] = std::move(state);
      ++frame.pc;
      return;
    }
    case Opcode::PCheckEq: {
      std::uint32_t value_reg = 0;
      std::uint32_t const_id = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &value_reg) ||
          !operand_u32(frame, insn, 1, &const_id) ||
          !operand_u32(frame, insn, 2, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const Value actual = read_reg(frame, value_reg);
      const Value expected = load_constant(frame, const_id);
      if (fault_.has_value()) {
        return;
      }
      if (!ensure_lifecycle_access(frame, actual) ||
          !ensure_lifecycle_access(frame, expected)) {
        return;
      }
      frame.pc = value_equals(actual, expected) ? frame.pc + 1U : fail_pc;
      if (frame.pc == fail_pc) {
        clear_pattern_state(frame);
      }
      return;
    }
    case Opcode::PCheckPin: {
      std::uint32_t value_reg = 0;
      std::uint32_t slot = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &value_reg) ||
          !operand_u32(frame, insn, 1, &slot) ||
          !operand_u32(frame, insn, 2, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const Value actual = read_reg(frame, value_reg);
      const Value expected = read_reg(frame, slot);
      if (fault_.has_value()) {
        return;
      }
      if (!ensure_lifecycle_access(frame, actual) ||
          !ensure_lifecycle_access(frame, expected)) {
        return;
      }
      frame.pc = value_equals(actual, expected) ? frame.pc + 1U : fail_pc;
      if (frame.pc == fail_pc) {
        clear_pattern_state(frame);
      }
      return;
    }
    case Opcode::PCheckLenEq:
    case Opcode::PCheckLenGte: {
      std::uint32_t seq_reg = 0;
      std::uint32_t expected = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &seq_reg) ||
          !operand_u32(frame, insn, 1, &expected) ||
          !operand_u32(frame, insn, 2, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const auto state = frame.prepared_seq_regs.find(seq_reg);
      if (state == frame.prepared_seq_regs.end()) {
        set_fault(frame, "VMError", "P_CHECK_LEN expects prepared sequence");
        return;
      }
      const std::size_t size = state->second.items.size();
      const bool ok = insn.opcode == Opcode::PCheckLenEq ? size == expected
                                                         : size >= expected;
      if (!ok) {
        clear_pattern_state(frame);
        frame.pc = fail_pc;
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::PGetIndex: {
      std::uint32_t dst = 0;
      std::uint32_t seq_reg = 0;
      std::uint32_t index = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &seq_reg) ||
          !operand_u32(frame, insn, 2, &index)) {
        return;
      }
      const auto state = frame.prepared_seq_regs.find(seq_reg);
      if (state == frame.prepared_seq_regs.end()) {
        set_fault(frame, "VMError", "P_GET_INDEX expects prepared sequence");
        return;
      }
      if (index >= state->second.items.size()) {
        set_fault(frame, "VMError", "P_GET_INDEX is out of range");
        return;
      }
      state->second.rest_start = std::max(state->second.rest_start,
                                          static_cast<std::size_t>(index) + 1U);
      if (!write_reg(frame, dst, state->second.items[index])) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::PHasKey: {
      std::uint32_t map_reg = 0;
      std::uint32_t key_id = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &map_reg) ||
          !operand_u32(frame, insn, 1, &key_id) ||
          !operand_u32(frame, insn, 2, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const auto state = frame.prepared_map_regs.find(map_reg);
      if (state == frame.prepared_map_regs.end()) {
        set_fault(frame, "VMError", "P_HAS_KEY expects prepared map");
        return;
      }
      if (state->second.index_by_key.find(key_id) ==
          state->second.index_by_key.end()) {
        clear_pattern_state(frame);
        frame.pc = fail_pc;
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::PGetKey: {
      std::uint32_t dst = 0;
      std::uint32_t map_reg = 0;
      std::uint32_t key_id = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &map_reg) ||
          !operand_u32(frame, insn, 2, &key_id)) {
        return;
      }
      const auto state = frame.prepared_map_regs.find(map_reg);
      if (state == frame.prepared_map_regs.end()) {
        set_fault(frame, "VMError", "P_GET_KEY expects prepared map");
        return;
      }
      const auto index = state->second.index_by_key.find(key_id);
      if (index == state->second.index_by_key.end()) {
        set_fault(frame, "VMError", "P_GET_KEY missing requested key");
        return;
      }
      if (!write_reg(frame, dst, state->second.entries[index->second].value)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::PTripleEq: {
      std::uint32_t matcher_reg = 0;
      std::uint32_t value_reg = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &matcher_reg) ||
          !operand_u32(frame, insn, 1, &value_reg) ||
          !operand_u32(frame, insn, 2, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const Value matcher = read_reg(frame, matcher_reg);
      const Value value = read_reg(frame, value_reg);
      if (fault_.has_value()) {
        return;
      }
      bool matched = false;
      if (!pattern_triple_eq(frame, matcher, value, &matched)) {
        return;
      }
      if (fault_.has_value()) {
        return;
      }
      frame.pc = matched ? frame.pc + 1U : fail_pc;
      if (frame.pc == fail_pc) {
        clear_pattern_state(frame);
      }
      return;
    }
    case Opcode::PBind: {
      std::uint32_t slot = 0;
      std::uint32_t value_reg = 0;
      if (!operand_u32(frame, insn, 0, &slot) ||
          !operand_u32(frame, insn, 1, &value_reg)) {
        return;
      }
      Value bound = Value::null();
      const auto seq_state = frame.prepared_seq_regs.find(value_reg);
      if (seq_state != frame.prepared_seq_regs.end()) {
        bound = materialize_sequence_rest(seq_state->second);
      } else {
        const auto map_state = frame.prepared_map_regs.find(value_reg);
        if (map_state != frame.prepared_map_regs.end()) {
          map_state->second.rest_bound = true;
          bound = materialize_map_rest(map_state->second);
        } else {
          bound = read_reg(frame, value_reg);
          if (fault_.has_value()) {
            return;
          }
        }
      }
      frame.pending_pattern_bindings[slot] = std::move(bound);
      ++frame.pc;
      return;
    }
    case Opcode::PCommit: {
      std::uint32_t base_slot = 0;
      std::uint32_t count = 0;
      if (!operand_u32(frame, insn, 0, &base_slot) ||
          !operand_u32(frame, insn, 1, &count)) {
        return;
      }
      if (!finalize_pattern_success(frame, true)) {
        return;
      }
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t slot = base_slot + i;
        const auto pending = frame.pending_pattern_bindings.find(slot);
        if (pending == frame.pending_pattern_bindings.end()) {
          set_fault(frame, "VMError", "P_COMMIT slot is missing pending bind");
          clear_pattern_state(frame);
          return;
        }
        if (!write_reg(frame, slot, pending->second)) {
          clear_pattern_state(frame);
          return;
        }
      }
      frame.pending_pattern_bindings.clear();
      clear_pattern_state(frame);
      ++frame.pc;
      return;
    }
    case Opcode::PFail: {
      std::int64_t mode = 0;
      if (!operand_i64(frame, insn, 0, &mode)) {
        return;
      }
      clear_pattern_state(frame);
      if (mode == kPatternFailModeSoft) {
        ++frame.pc;
        return;
      }
      if (mode == kPatternFailModeMatchError) {
        set_fault(frame, "MatchError", "pattern match failed");
        return;
      }
      set_fault(frame, "VMError", "unknown pattern fail mode");
      return;
    }
    case Opcode::Return: {
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &src)) {
        return;
      }
      Value value = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      if (frame.return_override.has_value()) {
        value = *frame.return_override;
      }
      std::vector<Value> completed_regs = frame.regs;
      const std::optional<std::uint32_t> caller_reg = frame.caller_result_reg;
      frames_.pop_back();
      last_completed_regs_ = std::move(completed_regs);
      if (frames_.empty()) {
        final_value_ = value;
        return;
      }
      Frame &caller = frames_.back();
      caller.active_call_pc.reset();
      if (!caller_reg.has_value() || !write_reg(caller, *caller_reg, value)) {
        return;
      }
      return;
    }
    case Opcode::Raise: {
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &src)) {
        return;
      }
      const Value exception = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      raise_value(frame, exception);
      return;
    }
    case Opcode::CloseUpvalues:
      ++frame.pc;
      return;
    case Opcode::Safepoint:
      run_safepoint();
      ++frame.pc;
      return;
    default:
      set_fault(frame, "VMError", "opcode is not implemented in W5.1 runtime");
      return;
    }
  }

  const BcModule &module_;
  std::shared_ptr<RuntimeState> state_;
  std::vector<Frame> frames_;
  std::optional<Fault> fault_;
  std::vector<Value> last_completed_regs_;
  Value final_value_ = Value::null();
};

} // namespace

ExecutionResult execute_code(const bytecode::BcModule &module,
                             std::uint32_t code_id,
                             const std::vector<Value> &args, Value self,
                             Value block) {
  Vm vm(module);
  return vm.execute(code_id, args, std::move(self), std::move(block));
}

struct RuntimeWorld::Impl {
  explicit Impl(const bytecode::BcModule &module_ref)
      : module(&module_ref), state(std::make_shared<RuntimeState>()) {
    state->initialize_for_module(*module);
  }

  const bytecode::BcModule *module = nullptr;
  std::shared_ptr<RuntimeState> state;
};

RuntimeWorld::RuntimeWorld(const bytecode::BcModule &module)
    : impl_(std::make_shared<Impl>(module)) {}

RuntimeWorld::~RuntimeWorld() = default;

ExecutionResult RuntimeWorld::execute(std::uint32_t code_id,
                                      const std::vector<Value> &args,
                                      Value self, Value block) {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {Value::null(),
            Fault{"VMError", "runtime world is not bound", 0, 0}};
  }
  impl_->state->initialize_for_module(*impl_->module);
  Vm vm(*impl_->module, impl_->state);
  return vm.execute(code_id, args, std::move(self), std::move(block));
}

ExecutionResult
RuntimeWorld::define_instance_method(std::uint32_t class_index,
                                     bytecode::BcMethod method) {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {Value::null(),
            Fault{"VMError", "runtime world is not bound", 0, 0}};
  }
  const bytecode::BcModule &module = *impl_->module;
  if (class_index >= module.classes.size()) {
    return {
        Value::null(),
        Fault{"VMError", "define_method class index is out of range", 0, 0}};
  }
  if (method.selector_sym_id >= module.symbols.size()) {
    return {Value::null(),
            Fault{"VMError", "define_method selector is out of range", 0, 0}};
  }
  if (find_code(module, method.entry_code_id) == nullptr) {
    return {Value::null(),
            Fault{"VMError", "define_method entry code id is unknown", 0, 0}};
  }
  if (impl_->state->classes.size() < module.classes.size()) {
    impl_->state->classes.resize(module.classes.size());
  }
  impl_->state->initialize_for_module(module);
  method.owner_dispatch_ref = class_index;
  method.flags = kMethodFlagInstance;
  impl_->state->classes[class_index]
      .instance_method_table.entries[method.selector_sym_id] =
      std::move(method);
  impl_->state->invalidate_dispatch_owner(class_index);
  return {Value::null(), std::nullopt};
}

namespace {

ExecutionResult add_runtime_mixin(const bytecode::BcModule &module,
                                  RuntimeState &state,
                                  std::uint32_t class_index,
                                  std::uint32_t mixin_index, bool class_side) {
  if (class_index >= module.classes.size()) {
    return {Value::null(),
            Fault{"VMError", "mixin owner index is out of range", 0, 0}};
  }
  if (mixin_index >= module.classes.size()) {
    return {Value::null(),
            Fault{"VMError", "mixin target index is out of range", 0, 0}};
  }
  if (class_side &&
      (module.classes[class_index].flags & bytecode::kClassFlagMixin) != 0U) {
    return {Value::null(),
            Fault{"TypeError", "extend owner must be a class object", 0, 0}};
  }
  if ((module.classes[mixin_index].flags & bytecode::kClassFlagMixin) == 0U) {
    return {Value::null(),
            Fault{"TypeError", "include/extend target is not a mixin", 0, 0}};
  }
  if (state.classes.size() < module.classes.size()) {
    state.classes.resize(module.classes.size());
  }
  state.initialize_for_module(module);
  std::vector<std::uint32_t> &dynamic_mixins =
      class_side ? state.classes[class_index].direct_extend_indices
                 : state.classes[class_index].direct_include_indices;
  if (std::find(dynamic_mixins.begin(), dynamic_mixins.end(), mixin_index) !=
      dynamic_mixins.end()) {
    return {Value::null(), std::nullopt};
  }
  dynamic_mixins.push_back(mixin_index);
  state.invalidate_dispatch_owner(class_index);
  return {Value::null(), std::nullopt};
}

} // namespace

ExecutionResult RuntimeWorld::include_mixin(std::uint32_t class_index,
                                            std::uint32_t mixin_index) {
  if (impl_ == nullptr) {
    return {Value::null(),
            Fault{"VMError", "runtime world is not bound", 0, 0}};
  }
  return add_runtime_mixin(*impl_->module, *impl_->state, class_index,
                           mixin_index, false);
}

ExecutionResult RuntimeWorld::extend_mixin(std::uint32_t class_index,
                                           std::uint32_t mixin_index) {
  if (impl_ == nullptr) {
    return {Value::null(),
            Fault{"VMError", "runtime world is not bound", 0, 0}};
  }
  return add_runtime_mixin(*impl_->module, *impl_->state, class_index,
                           mixin_index, true);
}

std::uint64_t RuntimeWorld::world_epoch() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->world_epoch;
}

std::uint64_t RuntimeWorld::method_version(std::uint32_t class_index) const {
  if (impl_ == nullptr || impl_->state == nullptr ||
      class_index >= impl_->state->classes.size()) {
    return 0;
  }
  return impl_->state->classes[class_index].method_version;
}

std::size_t RuntimeWorld::method_table_size(std::uint32_t class_index,
                                            MethodTableSide side) const {
  if (impl_ == nullptr || impl_->state == nullptr ||
      class_index >= impl_->state->classes.size()) {
    return 0;
  }
  const ClassRuntimeState &owner = impl_->state->classes[class_index];
  return side == MethodTableSide::Class
             ? owner.class_method_table.entries.size()
             : owner.instance_method_table.entries.size();
}

RuntimeHeapStats RuntimeWorld::heap_stats() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  return impl_->state->heap.stats();
}

std::uint64_t RuntimeWorld::drain_remote_frees() {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->heap.drain_remote_frees();
}

std::uint64_t RuntimeWorld::drain_remote_frees(std::uint64_t worker_id) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->heap.drain_remote_frees(worker_id);
}

RuntimeWriteBarrierResult RuntimeWorld::write_barrier(const Value &owner,
                                                      const Value &value) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeWriteBarrierResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.write_barrier(owner, value);
}

RuntimeGcResult RuntimeWorld::collect_garbage(const std::vector<Value> &roots,
                                              RuntimeGcCycle cycle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  std::vector<Value> all_roots = roots;
  for (const ClassRuntimeState &klass : impl_->state->classes) {
    for (const auto &[name, value] : klass.cvars) {
      (void)name;
      if (value_has_heap_payload_tag(value)) {
        all_roots.push_back(value);
      }
    }
  }
  return impl_->state->heap.collect_garbage(all_roots, cycle);
}

void RuntimeWorld::request_garbage_collection(RuntimeGcCycle cycle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return;
  }
  impl_->state->heap.request_garbage_collection(cycle);
}

std::string value_to_debug_string(const Value &value,
                                  const bytecode::BcModule *module) {
  if (value.is_null()) {
    return "null";
  }
  if (value.is_bool()) {
    return value.as_bool() ? "true" : "false";
  }
  if (value.is_integer()) {
    return std::to_string(value.as_integer());
  }
  if (value.is_float()) {
    std::ostringstream out;
    out << value.as_float();
    return out.str();
  }
  if (value.is_symbol()) {
    const SymbolValue symbol = value.as_symbol();
    if (module != nullptr && symbol.symbol_id < module->symbols.size()) {
      return ":" + module->symbols[symbol.symbol_id];
    }
    return ":<invalid>";
  }
  if (value.is_string()) {
    const StringValue string = value.as_string();
    if (module != nullptr && string.string_id < module->strings.size()) {
      return "\"" + module->strings[string.string_id] + "\"";
    }
    return "\"<invalid>\"";
  }
  if (value.is_class_object()) {
    const ClassObjectValue klass = value.as_class_object();
    std::ostringstream out;
    out << "<class";
    if (module != nullptr && klass.class_index < module->classes.size()) {
      const std::uint32_t symbol_id =
          module->classes[klass.class_index].class_name_sym_id;
      if (symbol_id < module->symbols.size()) {
        out << " " << module->symbols[symbol_id];
      } else {
        out << " #" << klass.class_index;
      }
    } else {
      out << " #" << klass.class_index;
    }
    out << ">";
    return out.str();
  }
  if (value.is_closure()) {
    const std::shared_ptr<ClosureValue> closure = value.as_closure();
    std::ostringstream out;
    if (closure == nullptr) {
      out << "<closure null>";
      return out.str();
    }
    const std::string lifecycle = lifecycle_debug_label(closure->header);
    if (!lifecycle.empty()) {
      out << "<" << lifecycle << " closure>";
      return out.str();
    }
    out << "<closure c" << closure->code_id << ">";
    return out.str();
  }
  if (value.is_instance_object()) {
    const std::shared_ptr<InstanceValue> instance = value.as_instance_object();
    std::ostringstream out;
    out << "<instance";
    if (instance == nullptr) {
      out << " null>";
      return out.str();
    }
    const std::string lifecycle = lifecycle_debug_label(instance->header);
    if (!lifecycle.empty()) {
      out << " " << lifecycle << ">";
      return out.str();
    }
    if (module != nullptr && instance->class_index < module->classes.size()) {
      const std::uint32_t symbol_id =
          module->classes[instance->class_index].class_name_sym_id;
      if (symbol_id < module->symbols.size()) {
        out << " " << module->symbols[symbol_id];
      } else {
        out << " #" << instance->class_index;
      }
    } else {
      out << " #" << instance->class_index;
    }
    out << ">";
    return out.str();
  }
  if (value.is_list()) {
    const std::shared_ptr<ListValue> list = value.as_list();
    if (list == nullptr) {
      return "[<null-list>]";
    }
    const std::string lifecycle = lifecycle_debug_label(list->header);
    if (!lifecycle.empty()) {
      return "[<" + lifecycle + "-list>]";
    }
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < list->items.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << value_to_debug_string(list->items[i], module);
    }
    out << "]";
    return out.str();
  }
  if (value.is_tuple()) {
    const std::shared_ptr<TupleValue> tuple = value.as_tuple();
    if (tuple == nullptr) {
      return "(<null-tuple>)";
    }
    const std::string lifecycle = lifecycle_debug_label(tuple->header);
    if (!lifecycle.empty()) {
      return "(<" + lifecycle + "-tuple>)";
    }
    std::ostringstream out;
    out << "(";
    for (std::size_t i = 0; i < tuple->items.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << value_to_debug_string(tuple->items[i], module);
    }
    out << ")";
    return out.str();
  }
  if (value.is_map()) {
    const std::shared_ptr<MapValue> map = value.as_map();
    if (map == nullptr) {
      return "{<null-map>}";
    }
    const std::string lifecycle = lifecycle_debug_label(map->header);
    if (!lifecycle.empty()) {
      return "{<" + lifecycle + "-map>}";
    }
    std::ostringstream out;
    out << "{";
    for (std::size_t i = 0; i < map->entries.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      if (module != nullptr &&
          map->entries[i].symbol_id < module->symbols.size()) {
        out << module->symbols[map->entries[i].symbol_id];
      } else {
        out << "#" << map->entries[i].symbol_id;
      }
      out << ": " << value_to_debug_string(map->entries[i].value, module);
    }
    out << "}";
    return out.str();
  }
  return "<unknown>";
}

} // namespace amber::runtime
