#pragma once

#include "bytecode/format.h"
#include "package/package.h"
#include "profile/capabilities.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace amber::runtime {

struct ClosureValue;
struct InstanceValue;
struct ListValue;
struct TupleValue;
struct SetValue;
struct MapValue;
struct MapEntry;
class RuntimeAtomic;
class RuntimeBarrier;
class RuntimeChannel;
class RuntimeFlowModule;
class RuntimeHeap;
class RuntimeMutex;
class RuntimeTaskHandle;
class RuntimeTaskModule;
class RuntimeThreadedCollection;
class RuntimeWatchCell;
class RuntimeWatchObjectState;
class RuntimeWatchHandle;

enum class HeapObjectKind { Instance, List, Tuple, Set, Map, Closure };

enum class OwnerTokenKind { Shareable, Confined, Sync };

enum class ObjectLifetimeState { Live, Destroying, Destroyed, Deallocated };

enum class ObjectGeneration { Young, Mature, Shared };

enum class RuntimeGcCycle { Young, Full, Shared };

enum class RuntimePinViewKind { Opaque, ValueBuffer };

enum class RuntimePinPermission { ReadOnly, ReadWrite };

struct OwnerToken {
  OwnerTokenKind kind = OwnerTokenKind::Confined;
  std::uint64_t strand_id = 0;
};

inline constexpr std::uint32_t kObjectFlagFrozen = 0x1U;
inline constexpr std::uint32_t kObjectFlagShareable = 0x2U;
inline constexpr std::uint32_t kObjectFlagDead = 0x4U;
inline constexpr std::uint32_t kObjectFlagDestroyed = 0x8U;
inline constexpr std::uint32_t kObjectFlagDestroying = 0x10U;
inline constexpr std::uint32_t kObjectFlagPinned = 0x20U;

struct ShapeDescriptor {
  std::uint64_t shape_id = 0;
  std::uint64_t shape_version = 0;
  std::unordered_map<std::string, std::uint32_t> ivar_slots;
  std::vector<std::string> slot_names;
  std::shared_ptr<const ShapeDescriptor> parent_shape;
  bool dead = false;
};

struct ObjHeader {
  HeapObjectKind kind = HeapObjectKind::Instance;
  std::uint32_t class_index = 0;
  std::uint32_t flags = 0;
  OwnerToken owner;
  std::shared_ptr<const ShapeDescriptor> shape;
  std::uint64_t allocation_id = 0;
  std::uint64_t arena_worker_id = 0;
  std::size_t allocation_size = 0;
  ObjectLifetimeState lifetime_state = ObjectLifetimeState::Live;
  ObjectGeneration generation = ObjectGeneration::Young;
  std::uint32_t gc_age = 0;
  std::uint64_t gc_mark_epoch = 0;
  std::uint32_t pin_count = 0;
  std::uint64_t pin_epoch = 0;
};

struct SymbolValue {
  std::uint32_t symbol_id = 0;
};

struct StringValue {
  std::uint32_t string_id = 0;
};

struct ClassObjectValue {
  std::uint32_t class_index = 0;
};

enum class RuntimeNativeTypeKind {
  TaskModule,
  Channel,
  Mutex,
  Atomic,
  Barrier,
  Flow,
  ThreadedCollection,
  Amber,
  Str,
  Int,
  Float,
  Bool,
  Symbol,
  Array,
  Tuple,
  Set,
  Map,
  Null,
  Object
};

struct NativeTypeValue {
  RuntimeNativeTypeKind kind = RuntimeNativeTypeKind::TaskModule;
};

struct Value {
  using Payload = std::variant<
      std::monostate, bool, std::int64_t, double, SymbolValue, StringValue,
      ClassObjectValue, std::shared_ptr<ClosureValue>,
      std::shared_ptr<InstanceValue>, std::shared_ptr<ListValue>,
      std::shared_ptr<TupleValue>, std::shared_ptr<SetValue>,
      std::shared_ptr<MapValue>, NativeTypeValue,
      std::shared_ptr<RuntimeTaskModule>, std::shared_ptr<RuntimeTaskHandle>,
      std::shared_ptr<RuntimeChannel>, std::shared_ptr<RuntimeMutex>,
      std::shared_ptr<RuntimeAtomic>, std::shared_ptr<RuntimeBarrier>,
      std::shared_ptr<RuntimeFlowModule>,
      std::shared_ptr<RuntimeThreadedCollection>,
      std::shared_ptr<RuntimeWatchCell>, std::shared_ptr<RuntimeWatchHandle>>;

  Payload payload;

  static Value null();
  static Value boolean(bool value);
  static Value integer(std::int64_t value);
  static Value floating(double value);
  static Value symbol(std::uint32_t symbol_id);
  static Value string(std::uint32_t string_id);
  static Value class_object(std::uint32_t class_index);
  static Value closure(std::shared_ptr<ClosureValue> value);
  static Value instance(std::shared_ptr<InstanceValue> value);
  static Value native_type(RuntimeNativeTypeKind kind);
  static Value task_module(std::shared_ptr<RuntimeTaskModule> value);
  static Value task_handle(std::shared_ptr<RuntimeTaskHandle> value);
  static Value channel(std::shared_ptr<RuntimeChannel> value);
  static Value mutex(std::shared_ptr<RuntimeMutex> value);
  static Value atomic(std::shared_ptr<RuntimeAtomic> value);
  static Value barrier(std::shared_ptr<RuntimeBarrier> value);
  static Value flow_module(std::shared_ptr<RuntimeFlowModule> value);
  static Value
  threaded_collection(std::shared_ptr<RuntimeThreadedCollection> value);
  static Value watch_cell(std::shared_ptr<RuntimeWatchCell> value);
  static Value watch_handle(std::shared_ptr<RuntimeWatchHandle> value);

  bool is_null() const;
  bool is_bool() const;
  bool is_integer() const;
  bool is_float() const;
  bool is_symbol() const;
  bool is_string() const;
  bool is_class_object() const;
  bool is_closure() const;
  bool is_instance_object() const;
  bool is_list() const;
  bool is_tuple() const;
  bool is_set() const;
  bool is_map() const;
  bool is_native_type() const;
  bool is_task_module() const;
  bool is_task_handle() const;
  bool is_channel() const;
  bool is_mutex() const;
  bool is_atomic() const;
  bool is_barrier() const;
  bool is_flow_module() const;
  bool is_threaded_collection() const;
  bool is_watch_cell() const;
  bool is_watch_handle() const;

  bool as_bool() const;
  std::int64_t as_integer() const;
  double as_float() const;
  SymbolValue as_symbol() const;
  StringValue as_string() const;
  ClassObjectValue as_class_object() const;
  std::shared_ptr<ClosureValue> as_closure() const;
  std::shared_ptr<InstanceValue> as_instance_object() const;
  std::shared_ptr<ListValue> as_list() const;
  std::shared_ptr<TupleValue> as_tuple() const;
  std::shared_ptr<SetValue> as_set() const;
  std::shared_ptr<MapValue> as_map() const;
  NativeTypeValue as_native_type() const;
  std::shared_ptr<RuntimeTaskModule> as_task_module() const;
  std::shared_ptr<RuntimeTaskHandle> as_task_handle() const;
  std::shared_ptr<RuntimeChannel> as_channel() const;
  std::shared_ptr<RuntimeMutex> as_mutex() const;
  std::shared_ptr<RuntimeAtomic> as_atomic() const;
  std::shared_ptr<RuntimeBarrier> as_barrier() const;
  std::shared_ptr<RuntimeFlowModule> as_flow_module() const;
  std::shared_ptr<RuntimeThreadedCollection> as_threaded_collection() const;
  std::shared_ptr<RuntimeWatchCell> as_watch_cell() const;
  std::shared_ptr<RuntimeWatchHandle> as_watch_handle() const;
};

struct RuntimeWatchCellSnapshot {
  std::uint64_t cell_id = 0;
  std::uint64_t revision = 0;
  std::uint64_t subscriber_count = 0;
  std::string target_name;
  bool watched = false;
  Value value = Value::null();
};

struct RuntimeWatchWriteResult {
  bool changed = false;
  Value old_value = Value::null();
  Value new_value = Value::null();
  std::uint64_t old_revision = 0;
  std::uint64_t new_revision = 0;
};

struct RuntimeWatchObjectStateSnapshot {
  std::uint64_t object_id = 0;
  std::uint64_t object_revision = 0;
  std::uint64_t subscriber_count = 0;
  std::unordered_map<std::string, std::uint64_t> field_revisions;
};

struct RuntimeWatchIvarSnapshot {
  std::uint64_t object_id = 0;
  std::uint64_t object_revision = 0;
  std::uint64_t field_revision = 0;
  std::uint64_t subscriber_count = 0;
  std::string field_name;
  bool watched = false;
  Value value = Value::null();
};

struct RuntimeWatchIvarWriteResult {
  bool changed = false;
  std::string field_name;
  Value old_value = Value::null();
  Value new_value = Value::null();
  std::uint64_t old_revision = 0;
  std::uint64_t new_revision = 0;
  std::uint64_t old_object_revision = 0;
  std::uint64_t new_object_revision = 0;
};

struct RuntimeWatchEvent {
  std::string kind;
  std::uint64_t watch_epoch = 0;
  std::uint64_t cell_id = 0;
  std::uint64_t handle_id = 0;
  std::uint64_t object_id = 0;
  std::uint64_t old_object_revision = 0;
  std::uint64_t new_object_revision = 0;
  std::string target_name;
  std::string field_name;
  std::uint64_t old_revision = 0;
  std::uint64_t new_revision = 0;
  Value old_value = Value::null();
  Value new_value = Value::null();
};

enum class RuntimeDependencyKind { Binding, Ivar, Object };

struct RuntimeDependency {
  RuntimeDependencyKind kind = RuntimeDependencyKind::Binding;
  std::uint64_t cell_id = 0;
  std::uint64_t object_id = 0;
  std::string target_name;
  std::string field_name;
  std::uint64_t revision = 0;
  std::uint64_t object_revision = 0;
};

struct RuntimeDependencySet {
  std::uint64_t notebook_cell_id = 0;
  std::vector<RuntimeDependency> dependencies;
};

class RuntimeWatchCell {
public:
  explicit RuntimeWatchCell(Value value = Value::null(),
                            std::uint64_t cell_id = 0,
                            std::string target_name = {});

  Value read() const;
  RuntimeWatchWriteResult write(Value value);
  RuntimeWatchCellSnapshot snapshot() const;
  bool watched() const;
  void enable_watch(std::string target_name = {});
  void subscribe();
  void unsubscribe();
  std::uint64_t cell_id() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimeWatchObjectState {
public:
  explicit RuntimeWatchObjectState(std::uint64_t object_id = 0);

  RuntimeWatchObjectStateSnapshot snapshot() const;
  RuntimeWatchIvarSnapshot snapshot_field(const std::string &field_name,
                                          Value current_value) const;
  RuntimeWatchIvarWriteResult write_field(std::string field_name,
                                          Value old_value, Value new_value);
  void subscribe_field(std::string field_name);
  void unsubscribe_field(const std::string &field_name);
  bool field_watched(const std::string &field_name) const;
  std::uint64_t object_id() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimeWatchHandle {
public:
  RuntimeWatchHandle();
  RuntimeWatchHandle(std::shared_ptr<RuntimeWatchCell> cell,
                     std::uint64_t handle_id, std::string target_name);
  RuntimeWatchHandle(std::shared_ptr<RuntimeWatchObjectState> object_state,
                     std::uint64_t handle_id, std::string target_name,
                     std::string field_name);
  RuntimeWatchHandle(const RuntimeWatchHandle &) = delete;
  RuntimeWatchHandle &operator=(const RuntimeWatchHandle &) = delete;
  RuntimeWatchHandle(RuntimeWatchHandle &&) noexcept;
  RuntimeWatchHandle &operator=(RuntimeWatchHandle &&) noexcept;
  ~RuntimeWatchHandle();

  bool active() const;
  bool unwatch();
  std::uint64_t handle_id() const;
  std::shared_ptr<RuntimeWatchCell> cell() const;
  RuntimeWatchCellSnapshot snapshot() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

struct InstanceValue {
  ObjHeader header;
  std::uint32_t class_index = 0;
  std::vector<Value> ivar_storage;
  std::uint64_t ivar_shape_version = 1;
  std::unordered_map<std::string, Value> ivars;
  std::shared_ptr<RuntimeWatchObjectState> watch_state;
};

struct ListValue {
  ObjHeader header;
  std::vector<Value> items;
  bool frozen = false;
};

struct TupleValue {
  ObjHeader header;
  std::vector<Value> items;
};

struct SetValue {
  ObjHeader header;
  std::vector<Value> items;
  bool frozen = false;
};

struct MapEntry {
  std::uint32_t symbol_id = 0;
  Value value = Value::null();
};

struct MapValue {
  ObjHeader header;
  std::vector<MapEntry> entries;
  bool frozen = false;
};

struct ClosureValue {
  ObjHeader header;
  std::uint32_t code_id = 0;
  std::vector<Value> captures;
  Value self = Value::null();
};

struct RuntimeArenaStats {
  std::uint64_t worker_id = 0;
  std::uint64_t allocations = 0;
  std::uint64_t live_objects = 0;
  std::uint64_t remote_queue_depth = 0;
};

struct RuntimeHeapStats {
  std::uint64_t allocations = 0;
  std::uint64_t live_objects = 0;
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

std::uint64_t current_runtime_worker_id();
std::uint64_t current_runtime_strand_id();
std::uint64_t current_runtime_task_id();
bool current_runtime_task_cancel_requested();
bool current_runtime_task_sync_active();

class RuntimeTaskFailure : public std::exception {
public:
  RuntimeTaskFailure(std::string error_name, std::string message);
  const char *what() const noexcept override;
  const std::string &error_name() const;
  const std::string &message() const;

private:
  std::string error_name_;
  std::string message_;
  std::string what_;
};

class RuntimeTaskCancelled : public std::exception {
public:
  RuntimeTaskCancelled();
  const char *what() const noexcept override;
};

void throw_if_runtime_task_cancelled();

class RuntimeWorkerScope {
public:
  explicit RuntimeWorkerScope(std::uint64_t worker_id);
  RuntimeWorkerScope(const RuntimeWorkerScope &) = delete;
  RuntimeWorkerScope &operator=(const RuntimeWorkerScope &) = delete;
  ~RuntimeWorkerScope();

private:
  std::uint64_t previous_worker_id_ = 0;
};

class RuntimeStrandScope {
public:
  explicit RuntimeStrandScope(std::uint64_t strand_id);
  RuntimeStrandScope(const RuntimeStrandScope &) = delete;
  RuntimeStrandScope &operator=(const RuntimeStrandScope &) = delete;
  ~RuntimeStrandScope();

private:
  std::uint64_t previous_strand_id_ = 0;
};

enum class RuntimeStrandState {
  New,
  Runnable,
  Running,
  Sleeping,
  Waiting,
  Done,
  Finished = Done,
  Failed,
  Cancelled
};

struct RuntimeSchedulerConfig {
  std::size_t worker_count = 0;
  std::uint64_t first_worker_id = 1;
};

struct RuntimeSchedulerStats {
  std::uint64_t worker_count = 0;
  std::uint64_t strands_created = 0;
  std::uint64_t strands_completed = 0;
  std::uint64_t strands_failed = 0;
  std::uint64_t global_queue_enqueues = 0;
  std::uint64_t local_queue_enqueues = 0;
  std::uint64_t worker_dequeues = 0;
  std::uint64_t explicit_wakes = 0;
  std::uint64_t timer_wakes = 0;
  std::uint64_t coalesced_wakes = 0;
  std::uint64_t stale_timer_wakes = 0;
  std::uint64_t max_parallel_running = 0;
  std::uint64_t runnable_queue_depth = 0;
  std::uint64_t timer_queue_depth = 0;
  std::uint64_t sleeping_strands = 0;
  std::uint64_t tasks_created = 0;
  std::uint64_t tasks_completed = 0;
  std::uint64_t tasks_failed = 0;
  std::uint64_t tasks_cancelled = 0;
  std::uint64_t task_joins = 0;
  std::uint64_t task_join_timeouts = 0;
  std::uint64_t task_cancellation_requests = 0;
  std::uint64_t task_wait_state_entries = 0;
  std::uint64_t structured_child_tasks = 0;
  std::uint64_t first_failure_cancellations = 0;
  std::uint64_t supervisor_one_for_one_failures = 0;
  std::uint64_t supervisor_one_for_all_cancellations = 0;
  std::uint64_t supervisor_rest_for_one_cancellations = 0;
};

struct RuntimeStrandSnapshot {
  std::uint64_t strand_id = 0;
  RuntimeStrandState state = RuntimeStrandState::New;
  std::uint64_t worker_id = 0;
  std::uint64_t wake_generation = 0;
  bool wake_pending = false;
  std::uint64_t explicit_wakes = 0;
  std::uint64_t timer_wakes = 0;
};

struct RuntimeTaskSnapshot {
  std::uint64_t task_id = 0;
  std::uint64_t parent_task_id = 0;
  RuntimeStrandState state = RuntimeStrandState::New;
  std::uint64_t worker_id = 0;
  bool cancellation_requested = false;
  std::uint64_t total_children = 0;
  std::uint64_t active_children = 0;
  std::string error_name;
  std::string message;
};

struct RuntimeTaskJoinResult {
  bool ok = false;
  bool joined = false;
  bool timed_out = false;
  bool cancelled = false;
  std::uint64_t task_id = 0;
  RuntimeStrandState state = RuntimeStrandState::New;
  std::string error_name;
  std::string message;
};

enum class RuntimeSupervisorPolicy {
  CancelScope,
  OneForOne,
  OneForAll,
  RestForOne
};

struct RuntimeTaskOptions {
  RuntimeSupervisorPolicy policy = RuntimeSupervisorPolicy::CancelScope;
};

struct RuntimeChannelResult {
  bool ok = false;
  bool sent = false;
  bool received = false;
  bool closed = false;
  bool timed_out = false;
  bool cancelled = false;
  Value value = Value::null();
  std::string error_name;
  std::string message;
};

struct RuntimeChannelStats {
  std::uint64_t capacity = 0;
  std::uint64_t buffered_values = 0;
  std::uint64_t pending_senders = 0;
  std::uint64_t pending_receivers = 0;
  std::uint64_t sends = 0;
  std::uint64_t receives = 0;
  std::uint64_t closes = 0;
  std::uint64_t send_timeouts = 0;
  std::uint64_t receive_timeouts = 0;
  std::uint64_t send_cancellations = 0;
  std::uint64_t receive_cancellations = 0;
  std::uint64_t isolation_rejections = 0;
  bool closed = false;
};

enum class RuntimeAwaitableState { Pending, Ready, Failed, Cancelled };

struct RuntimeAwaitableResult {
  bool ok = false;
  bool ready = false;
  bool timed_out = false;
  bool cancelled = false;
  bool failed = false;
  RuntimeAwaitableState state = RuntimeAwaitableState::Pending;
  Value value = Value::null();
  std::string error_name;
  std::string message;
};

struct RuntimeAwaitableStats {
  std::uint64_t waits = 0;
  std::uint64_t polls = 0;
  std::uint64_t completions = 0;
  std::uint64_t failures = 0;
  std::uint64_t cancellations = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t native_polls = 0;
  std::uint64_t native_cancellations = 0;
  std::uint64_t native_finishes = 0;
  RuntimeAwaitableState state = RuntimeAwaitableState::Pending;
  bool native_backed = false;
};

class RuntimeAwaitable {
public:
  RuntimeAwaitable();
  RuntimeAwaitable(const RuntimeAwaitable &) = delete;
  RuntimeAwaitable &operator=(const RuntimeAwaitable &) = delete;
  RuntimeAwaitable(RuntimeAwaitable &&) noexcept;
  RuntimeAwaitable &operator=(RuntimeAwaitable &&) noexcept;
  ~RuntimeAwaitable();

  static RuntimeAwaitable ready(Value value = Value::null());
  static RuntimeAwaitable from_native_wait(RuntimeHeap &heap,
                                           const RuntimePinToken &token);

  bool complete(Value value = Value::null());
  bool fail(std::string error_name, std::string message);
  bool cancel();
  RuntimeAwaitableResult
  await(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeAwaitableResult poll();
  RuntimeAwaitableState state() const;
  RuntimeAwaitableStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

enum class RuntimeMoveSlotState { Ready, Reserved, Moved };

struct RuntimeMoveReservation {
  std::uint64_t reservation_id = 0;
  Value value = Value::null();
  bool active = false;
};

struct RuntimeMoveResult {
  bool ok = false;
  bool moved = false;
  bool reserved = false;
  RuntimeMoveSlotState state = RuntimeMoveSlotState::Ready;
  RuntimeMoveReservation reservation;
  Value value = Value::null();
  std::string error_name;
  std::string message;
};

class RuntimeMoveSlot {
public:
  explicit RuntimeMoveSlot(Value value = Value::null());
  RuntimeMoveSlot(const RuntimeMoveSlot &) = delete;
  RuntimeMoveSlot &operator=(const RuntimeMoveSlot &) = delete;
  RuntimeMoveSlot(RuntimeMoveSlot &&) noexcept;
  RuntimeMoveSlot &operator=(RuntimeMoveSlot &&) noexcept;
  ~RuntimeMoveSlot();

  RuntimeMoveResult read() const;
  RuntimeMoveResult reserve_move();
  RuntimeMoveResult commit_move(RuntimeMoveReservation *reservation);
  RuntimeMoveResult release_move(RuntimeMoveReservation *reservation);
  void reset(Value value);
  RuntimeMoveSlotState state() const;
  bool moved() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimeChannel {
public:
  explicit RuntimeChannel(std::size_t capacity = 0);
  RuntimeChannel(const RuntimeChannel &) = delete;
  RuntimeChannel &operator=(const RuntimeChannel &) = delete;
  RuntimeChannel(RuntimeChannel &&) noexcept;
  RuntimeChannel &operator=(RuntimeChannel &&) noexcept;
  ~RuntimeChannel();

  RuntimeChannelResult
  send(const Value &value,
       std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeChannelResult
  send(RuntimeMoveSlot &slot,
       std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeChannelResult
  recv(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  bool close();
  bool closed() const;
  RuntimeChannelStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

enum class RuntimeSelectArmKind { Recv, Send, Await };

struct RuntimeSelectArm {
  RuntimeSelectArmKind kind = RuntimeSelectArmKind::Recv;
  RuntimeChannel *channel = nullptr;
  RuntimeAwaitable *awaitable = nullptr;
  Value value = Value::null();
  RuntimeMoveSlot *move_slot = nullptr;

  static RuntimeSelectArm recv(RuntimeChannel &channel);
  static RuntimeSelectArm send(RuntimeChannel &channel, Value value);
  static RuntimeSelectArm send_moved(RuntimeChannel &channel,
                                     RuntimeMoveSlot &slot);
  static RuntimeSelectArm awaitable_arm(RuntimeAwaitable &awaitable);
};

struct RuntimeSelectResult {
  bool ok = false;
  bool selected = false;
  bool else_selected = false;
  bool timed_out = false;
  bool cancelled = false;
  std::size_t arm_index = 0;
  RuntimeSelectArmKind kind = RuntimeSelectArmKind::Recv;
  RuntimeChannelResult channel_result;
  RuntimeAwaitableResult awaitable_result;
  std::string error_name;
  std::string message;
};

RuntimeSelectResult runtime_select(
    const std::vector<RuntimeSelectArm> &arms,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max(),
    bool has_else = false);

struct RuntimeMutexResult {
  bool ok = false;
  bool locked = false;
  bool unlocked = false;
  bool timed_out = false;
  bool cancelled = false;
  Value value = Value::null();
  std::string error_name;
  std::string message;
};

struct RuntimeMutexStats {
  std::uint64_t lock_attempts = 0;
  std::uint64_t locks = 0;
  std::uint64_t unlocks = 0;
  std::uint64_t contentions = 0;
  std::uint64_t reentrant_failures = 0;
  std::uint64_t lock_timeouts = 0;
  std::uint64_t lock_cancellations = 0;
  std::uint64_t waiting_lockers = 0;
  std::uint64_t owner_id = 0;
  bool locked = false;
};

class RuntimeMutex {
public:
  RuntimeMutex();
  RuntimeMutex(const RuntimeMutex &) = delete;
  RuntimeMutex &operator=(const RuntimeMutex &) = delete;
  RuntimeMutex(RuntimeMutex &&) noexcept;
  RuntimeMutex &operator=(RuntimeMutex &&) noexcept;
  ~RuntimeMutex();

  RuntimeMutexResult
  lock(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeMutexResult unlock();
  RuntimeMutexResult synchronize(
      std::function<Value()> function,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  bool locked() const;
  bool owned() const;
  RuntimeMutexStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimeAtomic {
public:
  explicit RuntimeAtomic(std::int64_t value = 0);
  explicit RuntimeAtomic(Value value);
  RuntimeAtomic(const RuntimeAtomic &) = delete;
  RuntimeAtomic &operator=(const RuntimeAtomic &) = delete;
  RuntimeAtomic(RuntimeAtomic &&) noexcept;
  RuntimeAtomic &operator=(RuntimeAtomic &&) noexcept;
  ~RuntimeAtomic();

  struct Result {
    bool ok = false;
    bool matched = false;
    bool updated = false;
    Value value = Value::null();
    std::uint64_t attempts = 0;
    std::string error_name;
    std::string message;
  };

  std::int64_t get() const;
  void set(std::int64_t value);
  bool compare_and_set(std::int64_t expected, std::int64_t desired);
  Value get_value() const;
  Result set_value(Value value);
  Result compare_and_set_value(const Value &expected, Value desired);
  Result update(std::function<Value(const Value &)> function);

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

struct RuntimeBarrierResult {
  bool ok = false;
  bool passed = false;
  bool last = false;
  bool timed_out = false;
  bool cancelled = false;
  std::uint64_t generation = 0;
  std::uint64_t parties = 0;
  std::uint64_t waiting = 0;
  std::string error_name;
  std::string message;
};

struct RuntimeBarrierStats {
  std::uint64_t parties = 0;
  std::uint64_t waiting = 0;
  std::uint64_t generation = 0;
  std::uint64_t arrivals = 0;
  std::uint64_t passes = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t cancellations = 0;
};

class RuntimeBarrier {
public:
  explicit RuntimeBarrier(std::size_t parties);
  RuntimeBarrier(const RuntimeBarrier &) = delete;
  RuntimeBarrier &operator=(const RuntimeBarrier &) = delete;
  RuntimeBarrier(RuntimeBarrier &&) noexcept;
  RuntimeBarrier &operator=(RuntimeBarrier &&) noexcept;
  ~RuntimeBarrier();

  RuntimeBarrierResult
  wait(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeBarrierStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

bool runtime_value_is_shareable(const Value &value);

class RuntimeScheduler {
public:
  using StrandFunction = std::function<void()>;

  explicit RuntimeScheduler(std::size_t worker_count = 0);
  explicit RuntimeScheduler(RuntimeSchedulerConfig config);
  RuntimeScheduler(const RuntimeScheduler &) = delete;
  RuntimeScheduler &operator=(const RuntimeScheduler &) = delete;
  RuntimeScheduler(RuntimeScheduler &&) noexcept;
  RuntimeScheduler &operator=(RuntimeScheduler &&) noexcept;
  ~RuntimeScheduler();

  void start();
  void shutdown();
  std::uint64_t spawn_strand(StrandFunction function);
  std::uint64_t spawn_sleeping_strand(std::chrono::milliseconds delay,
                                      StrandFunction function);
  std::uint64_t spawn_task(StrandFunction function);
  std::uint64_t spawn_task(RuntimeTaskOptions options, StrandFunction function);
  std::uint64_t spawn_sleeping_task(std::chrono::milliseconds delay,
                                    StrandFunction function);
  std::uint64_t spawn_sleeping_task(std::chrono::milliseconds delay,
                                    RuntimeTaskOptions options,
                                    StrandFunction function);
  bool wake_strand(std::uint64_t strand_id);
  bool cancel_task(std::uint64_t task_id);
  bool task_cancel_requested(std::uint64_t task_id) const;
  RuntimeTaskJoinResult join_task(
      std::uint64_t task_id,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  bool wait_until_idle(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
  RuntimeSchedulerStats stats() const;
  std::optional<RuntimeStrandSnapshot>
  strand_snapshot(std::uint64_t strand_id) const;
  std::optional<RuntimeTaskSnapshot> task_snapshot(std::uint64_t task_id) const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

enum class RuntimeTaskHandleState {
  Inactive,
  New,
  Runnable,
  Running,
  Sleeping,
  Waiting,
  Done,
  Failed,
  Cancelled
};

struct RuntimeTaskHandleSnapshot {
  bool active = false;
  std::uint64_t task_id = 0;
  std::uint64_t strand_id = 0;
  RuntimeTaskHandleState state = RuntimeTaskHandleState::Inactive;
  bool ready = false;
  bool succeeded = false;
  bool cancelled = false;
  bool failed = false;
  bool running = false;
  bool cancellation_requested = false;
  std::string error_name;
  std::string message;
};

struct RuntimeTaskPublicResult {
  bool ok = false;
  bool ready = false;
  bool timed_out = false;
  bool cancelled = false;
  bool failed = false;
  RuntimeTaskHandleState state = RuntimeTaskHandleState::Inactive;
  Value value = Value::null();
  std::string error_name;
  std::string message;
};

struct RuntimeTaskFailureInfo {
  bool ready = false;
  bool failed = false;
  bool cancelled = false;
  RuntimeTaskHandleState state = RuntimeTaskHandleState::Inactive;
  std::string error_name;
  std::string message;
};

class RuntimeTaskHandle {
public:
  RuntimeTaskHandle();

  bool active() const;
  std::uint64_t task_id() const;
  std::uint64_t strand_id() const;
  RuntimeTaskHandleState state() const;
  RuntimeTaskHandleSnapshot snapshot() const;

  RuntimeTaskPublicResult wait(std::chrono::milliseconds timeout =
                                   std::chrono::milliseconds::max()) const;
  RuntimeTaskPublicResult result() const;
  RuntimeTaskFailureInfo failure() const;

  bool cancel() const;
  bool resume() const;
  bool cancelled() const;
  bool done() const;
  bool running() const;
  bool failed() const;

private:
  struct State;

  RuntimeTaskHandle(std::shared_ptr<RuntimeScheduler> scheduler,
                    std::uint64_t task_id, std::shared_ptr<State> state);

  std::shared_ptr<RuntimeScheduler> scheduler_;
  std::uint64_t task_id_ = 0;
  std::shared_ptr<State> state_;

  friend class RuntimeTaskModule;
};

class RuntimeTaskModule {
public:
  using TaskFunction = std::function<Value()>;

  explicit RuntimeTaskModule(std::size_t worker_count = 0);
  explicit RuntimeTaskModule(RuntimeSchedulerConfig config);

  RuntimeTaskHandle async(TaskFunction function);
  RuntimeTaskHandle spawn(TaskFunction function);

  Value sync(TaskFunction function) const;
  bool sync_active() const;
  void sleep(std::chrono::milliseconds duration) const;
  void yield_current() const;

  RuntimeScheduler &scheduler();
  const RuntimeScheduler &scheduler() const;

private:
  enum class SpawnKind { SameStrand, NewStrand };

  RuntimeTaskHandle spawn_with_kind(SpawnKind kind, TaskFunction function);

  std::shared_ptr<RuntimeScheduler> scheduler_;
};

enum class RuntimeFlowFailurePolicy { First, Collect, Ignore };

enum class RuntimeFlowIsolationMode { Checked, Unchecked };

enum class RuntimeFlowPartitionPolicy { Items, Chunks, Stride };

struct RuntimeFlowOptions {
  std::size_t workers = 0;
  bool ordered = true;
  RuntimeFlowFailurePolicy failure_policy = RuntimeFlowFailurePolicy::First;
  RuntimeFlowIsolationMode isolation = RuntimeFlowIsolationMode::Checked;
  RuntimeFlowPartitionPolicy partition_policy =
      RuntimeFlowPartitionPolicy::Items;
  std::chrono::milliseconds timeout = std::chrono::milliseconds::max();
};

struct RuntimeFlowFailure {
  std::size_t index = 0;
  std::string error_name;
  std::string message;
};

struct RuntimeFlowGatherResult {
  bool ok = false;
  bool failed = false;
  bool timed_out = false;
  bool cancelled = false;
  std::vector<Value> values;
  std::vector<RuntimeFlowFailure> failures;
  std::uint64_t completed_count = 0;
  std::uint64_t failed_count = 0;
  std::uint64_t cancelled_count = 0;
  std::string error_name;
  std::string message;
};

struct RuntimeFlowReduceResult {
  bool ok = false;
  bool failed = false;
  Value value = Value::null();
  RuntimeFlowGatherResult gather;
  std::string error_name;
  std::string message;
};

struct RuntimeFlowStats {
  std::uint64_t flows = 0;
  std::uint64_t gathers = 0;
  std::uint64_t worker_tasks = 0;
  std::uint64_t completed_workers = 0;
  std::uint64_t failed_workers = 0;
  std::uint64_t cancelled_workers = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t reductions = 0;
  std::uint64_t broadcasts = 0;
  std::uint64_t isolation_rejections = 0;
};

class RuntimeFlowModule {
public:
  using MapFunction = std::function<Value(const Value &, std::size_t)>;
  using ReduceFunction = std::function<Value(const Value &, const Value &)>;
  using BroadcastFunction = std::function<Value(const Value &, std::size_t)>;

  explicit RuntimeFlowModule(std::size_t worker_count = 0);
  explicit RuntimeFlowModule(RuntimeSchedulerConfig config);
  RuntimeFlowModule(const RuntimeFlowModule &) = delete;
  RuntimeFlowModule &operator=(const RuntimeFlowModule &) = delete;
  RuntimeFlowModule(RuntimeFlowModule &&) noexcept;
  RuntimeFlowModule &operator=(RuntimeFlowModule &&) noexcept;
  ~RuntimeFlowModule();

  RuntimeFlowGatherResult gather(std::vector<RuntimeTaskHandle> handles,
                                 RuntimeFlowOptions options = {});
  RuntimeFlowGatherResult scatter(std::vector<Value> partitions,
                                  MapFunction function,
                                  RuntimeFlowOptions options = {});
  RuntimeFlowGatherResult scatter_map(std::vector<Value> items,
                                      MapFunction function,
                                      RuntimeFlowOptions options = {});
  RuntimeFlowReduceResult scatter_reduce(std::vector<Value> items, Value init,
                                         MapFunction map, ReduceFunction reduce,
                                         RuntimeFlowOptions options = {});
  RuntimeFlowGatherResult broadcast(Value value, std::size_t workers,
                                    BroadcastFunction function,
                                    RuntimeFlowOptions options = {});
  RuntimeFlowStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

struct RuntimeThreadedCollectionStats {
  std::uint64_t operations = 0;
  std::uint64_t each_operations = 0;
  std::uint64_t map_operations = 0;
  std::uint64_t filter_operations = 0;
  std::uint64_t flat_map_operations = 0;
  std::uint64_t combination_operations = 0;
  std::uint64_t permutation_operations = 0;
  std::uint64_t generated_values = 0;
  RuntimeFlowStats flow;
};

class RuntimeThreadedCollection {
public:
  using EachFunction = std::function<void(const Value &, std::size_t)>;
  using MapFunction = std::function<Value(const Value &, std::size_t)>;
  using PredicateFunction = std::function<bool(const Value &, std::size_t)>;
  using FlatMapFunction =
      std::function<std::vector<Value>(const Value &, std::size_t)>;

  explicit RuntimeThreadedCollection(std::vector<Value> items,
                                     std::size_t workers = 0,
                                     RuntimeFlowOptions options = {});
  RuntimeThreadedCollection(const RuntimeThreadedCollection &) = delete;
  RuntimeThreadedCollection &
  operator=(const RuntimeThreadedCollection &) = delete;
  RuntimeThreadedCollection(RuntimeThreadedCollection &&) noexcept;
  RuntimeThreadedCollection &operator=(RuntimeThreadedCollection &&) noexcept;
  ~RuntimeThreadedCollection();

  RuntimeFlowGatherResult each(EachFunction function);
  RuntimeFlowGatherResult map(MapFunction function);
  RuntimeFlowGatherResult select(PredicateFunction function);
  RuntimeFlowGatherResult reject(PredicateFunction function);
  RuntimeFlowGatherResult flat_map(FlatMapFunction function);
  RuntimeFlowGatherResult combination(std::size_t count);
  RuntimeFlowGatherResult permutation(std::size_t count);
  RuntimeThreadedCollectionStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimeHeap {
public:
  RuntimeHeap();
  ~RuntimeHeap();

  std::shared_ptr<InstanceValue>
  make_instance_value(std::uint32_t class_index = 0);
  std::shared_ptr<ClosureValue> make_closure_value();
  Value make_list_value(std::vector<Value> items, bool frozen = false);
  Value make_tuple_value(std::vector<Value> items);
  Value make_set_value(std::vector<Value> items, bool frozen = false);
  Value make_symbol_map_value(std::vector<MapEntry> entries,
                              bool frozen = false);

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
Value make_symbol_map_value(std::vector<MapEntry> entries, bool frozen = false);

enum class MethodTableSide { Instance, Class };
enum class RuntimeWorldState { Open, Frozen };
enum class RuntimeOwnerKind { Class, Mixin };

struct RuntimeWorldTransaction {
  RuntimeOwnerKind target_kind = RuntimeOwnerKind::Class;
  std::uint32_t target_index = 0;
  bool has_superclass_ref = false;
  std::uint32_t superclass_ref = 0;
  std::vector<bytecode::BcMethod> instance_methods;
  std::vector<bytecode::BcMethod> class_methods;
  std::vector<std::uint32_t> include_indices;
  std::vector<std::uint32_t> extend_indices;
};

struct RuntimeMirrorSourceLocation {
  bool present = false;
  std::uint32_t code_id = 0;
  std::uint32_t pc = 0;
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

struct RuntimeMethodMirror {
  bool read_only = true;
  std::uint32_t selector_symbol_id = 0;
  std::string selector;
  std::uint32_t owner_index = 0;
  std::string owner_name;
  RuntimeOwnerKind owner_kind = RuntimeOwnerKind::Class;
  MethodTableSide side = MethodTableSide::Instance;
  std::uint32_t signature_blob_id = 0;
  std::uint32_t entry_code_id = 0;
  std::uint32_t flags = 0;
  std::size_t parameter_count = 0;
  std::size_t default_count = 0;
  std::size_t type_hook_count = 0;
  std::size_t clause_count = 0;
  RuntimeMirrorSourceLocation source_location;
};

struct RuntimeDirectMixinMirror {
  std::uint32_t index = 0;
  std::string name;
  bool dynamic = false;
};

struct RuntimePackageDependencyMirror {
  std::string module_name;
  bytecode::Version required_format;
  bytecode::Version min_language_version;
  bool has_max_language_version = false;
  bytecode::Version max_language_version;
  bool has_abi_requirement = false;
};

struct RuntimePackageExportMirror {
  std::string public_name;
  std::string target_kind;
  std::uint32_t target_index = 0;
  std::uint32_t visibility_flags = 0;
  bool has_reexport = false;
  std::string reexport_module_name;
};

struct RuntimePackageAttrMirror {
  std::string key;
  std::string value;
};

using RuntimeCapabilityGrant = capability::CapabilityRequest;
using RuntimeCapabilityResolution = capability::CapabilityResolutionResult;
using RuntimeEffectSummary = effect::EffectSummary;
using RuntimeEffectValidation = effect::EffectValidationResult;
using RuntimeObservabilitySite = replay::ObservabilitySite;
using RuntimeReplayMetadata = replay::ReplayMetadata;
using RuntimeTraceEvent = replay::TraceEvent;
using RuntimeReplayTrace = replay::ReplayTrace;
using RuntimeReplayValidation = replay::ReplayValidationResult;
using RuntimeSchemaDefinition = data::SchemaDefinition;
using RuntimeSchemaMigration = data::SchemaMigration;
using RuntimeTablePlan = data::TablePlan;
using RuntimeSchemaValidation = data::SchemaValidationResult;
using RuntimeTablePlanValidation = data::TablePlanValidationResult;
using RuntimeWasmComponent = wasm_accel::WasmComponent;
using RuntimeAcceleratorKernel = wasm_accel::AcceleratorKernel;
using RuntimeWasmValidation = wasm_accel::WasmComponentValidationResult;
using RuntimeAcceleratorValidation = wasm_accel::AcceleratorValidationResult;
using RuntimeAgentSymbol = modern::AgentSymbol;
using RuntimeAgentValidation = modern::AgentValidationResult;
using RuntimeContractSpec = modern::ContractSpec;
using RuntimeContractValidation = modern::ContractValidationResult;
using RuntimePrivacyLabel = modern::PrivacyLabel;
using RuntimePrivacyValidation = modern::PrivacyValidationResult;
using RuntimeWorkflowStep = modern::WorkflowStep;
using RuntimeWorkflowValidation = modern::WorkflowValidationResult;

struct RuntimePackageMirror {
  bool read_only = true;
  std::string name;
  bytecode::Version format_version;
  bytecode::Version language_version;
  std::uint32_t profile_flags = 0;
  std::uint32_t file_flags = 0;
  bool has_init = false;
  std::uint32_t init_code_id = 0;
  std::vector<RuntimePackageDependencyMirror> dependencies;
  std::vector<RuntimePackageExportMirror> exports;
  std::vector<RuntimePackageAttrMirror> attrs;
  std::vector<RuntimeCapabilityGrant> capabilities;
  std::vector<RuntimeEffectSummary> effects;
  std::vector<RuntimeObservabilitySite> observability_sites;
  RuntimeReplayMetadata replay_metadata;
  std::vector<RuntimeSchemaDefinition> schemas;
  std::vector<RuntimeSchemaMigration> schema_migrations;
  std::vector<RuntimeTablePlan> table_plans;
  std::vector<RuntimeWasmComponent> wasm_components;
  std::vector<RuntimeAcceleratorKernel> accelerator_kernels;
  std::vector<RuntimeAgentSymbol> agent_symbols;
  std::vector<RuntimeContractSpec> contracts;
  std::vector<RuntimePrivacyLabel> privacy_labels;
  std::vector<RuntimeWorkflowStep> workflow_steps;
};

struct RuntimeOwnerMirror {
  bool read_only = true;
  std::uint32_t index = 0;
  std::string name;
  RuntimeOwnerKind kind = RuntimeOwnerKind::Class;
  std::uint32_t owner_flags = 0;
  std::uint32_t ivar_schema_id = 0;
  bool has_superclass = false;
  std::uint32_t superclass_index = 0;
  std::string superclass_name;
  std::uint64_t method_version = 0;
  std::uint64_t world_epoch = 0;
  RuntimeMirrorSourceLocation source_location;
  std::vector<RuntimeDirectMixinMirror> direct_includes;
  std::vector<RuntimeDirectMixinMirror> direct_extends;
  std::vector<RuntimeMethodMirror> instance_methods;
  std::vector<RuntimeMethodMirror> class_methods;
};

struct RuntimeWorldMirror {
  bool read_only = true;
  RuntimeWorldState state = RuntimeWorldState::Open;
  std::uint64_t world_epoch = 0;
  std::uint64_t watch_epoch = 0;
  RuntimePackageMirror package;
  std::vector<RuntimeOwnerMirror> owners;
};

struct RuntimeDispatchCacheStats {
  std::uint64_t call_cache_entries = 0;
  std::uint64_t call_cache_hits = 0;
  std::uint64_t call_cache_misses = 0;
  std::uint64_t call_cache_updates = 0;
};

struct RuntimePackageReloadDiagnostic {
  std::string error_name;
  std::string message;
  std::string module_name;
};

struct RuntimePackageReloadResult {
  bool ok = false;
  bool swapped = false;
  std::string package_name;
  std::string previous_version;
  std::string new_version;
  std::string root_module;
  std::uint64_t previous_world_epoch = 0;
  std::uint64_t new_world_epoch = 0;
  std::vector<RuntimePackageReloadDiagnostic> diagnostics;
};

struct RuntimeCapabilityCheckResult {
  bool ok = false;
  std::string error_name;
  std::string message;
  std::string capability;
  std::string target;
};

struct RuntimeEffectCheckResult {
  bool ok = false;
  std::string error_name;
  std::string message;
  std::vector<std::string> effects;
};

struct RuntimeWorldOptions {
  std::vector<RuntimeCapabilityGrant> capability_grants;
  std::vector<std::string> allowed_effects;
  bool enforce_effects = false;
  bool record_replay_trace = false;
  bool enforce_replay = false;
  std::string trace_id;
  std::uint64_t virtual_time_start = 1;
  std::uint64_t virtual_time_step = 1;
  RuntimeReplayTrace expected_replay;
};

struct TraceFrame {
  std::string module_id;
  std::uint32_t code_id = 0;
  std::uint32_t pc = 0;
  std::string file;
  std::uint32_t byte_start = 0;
  std::uint32_t byte_end = 0;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::uint32_t line_end = 0;
  std::uint32_t column_end = 0;
  std::string generated_kind;
};

struct Fault {
  Fault() = default;
  Fault(std::string error_name, std::string message, std::uint32_t code_id,
        std::uint32_t pc)
      : error_name(std::move(error_name)), message(std::move(message)),
        code_id(code_id), pc(pc) {}

  std::string error_name;
  std::string message;
  std::uint32_t code_id = 0;
  std::uint32_t pc = 0;
  std::vector<TraceFrame> trace;
  std::string trace_text;
};

struct ExecutionLocal {
  std::uint32_t slot = 0;
  std::string name;
  std::string role;
  std::string binding_kind;
  bool initialized = false;
  bool watched = false;
  std::uint64_t watch_cell_id = 0;
  std::uint64_t watch_revision = 0;
  Value value = Value::null();
};

struct ExecutionResult {
  ExecutionResult() = default;
  ExecutionResult(Value result_value, std::optional<Fault> result_fault,
                  std::vector<ExecutionLocal> result_locals = {},
                  std::vector<RuntimeWatchEvent> result_watch_events = {},
                  std::uint64_t result_watch_epoch = 0)
      : value(std::move(result_value)), fault(std::move(result_fault)),
        locals(std::move(result_locals)),
        watch_events(std::move(result_watch_events)),
        watch_epoch(result_watch_epoch) {}

  Value value = Value::null();
  std::optional<Fault> fault;
  std::vector<ExecutionLocal> locals;
  std::vector<RuntimeWatchEvent> watch_events;
  std::uint64_t watch_epoch = 0;

  bool ok() const { return !fault.has_value(); }
};

class RuntimeWorld {
public:
  explicit RuntimeWorld(const bytecode::BcModule &module);
  RuntimeWorld(const bytecode::BcModule &module, RuntimeWorldOptions options);
  explicit RuntimeWorld(const pkg::PackageArtifact &artifact);
  RuntimeWorld(const pkg::PackageArtifact &artifact,
               RuntimeWorldOptions options);
  ~RuntimeWorld();

  ExecutionResult execute(std::uint32_t code_id,
                          const std::vector<Value> &args = {},
                          Value self = Value::null(),
                          Value block = Value::null());

  ExecutionResult define_instance_method(std::uint32_t class_index,
                                         bytecode::BcMethod method);
  ExecutionResult define_class_method(std::uint32_t class_index,
                                      bytecode::BcMethod method);
  ExecutionResult include_mixin(std::uint32_t class_index,
                                std::uint32_t mixin_index);
  ExecutionResult extend_mixin(std::uint32_t class_index,
                               std::uint32_t mixin_index);
  ExecutionResult commit_transaction(const RuntimeWorldTransaction &tx);
  ExecutionResult freeze_world();
  RuntimePackageReloadResult
  reload_package_artifact(const pkg::PackageArtifact &artifact);
  RuntimeCapabilityCheckResult
  check_capability(const std::string &capability,
                   const std::string &target = {}) const;
  RuntimeCapabilityResolution capability_resolution() const;
  RuntimeEffectCheckResult
  check_effects(const std::vector<std::string> &effects) const;
  RuntimeEffectValidation effect_validation() const;
  RuntimeSchemaValidation schema_validation() const;
  RuntimeTablePlanValidation table_plan_validation() const;
  RuntimeWasmValidation wasm_validation() const;
  RuntimeAcceleratorValidation accelerator_validation() const;
  RuntimeAgentValidation agent_validation() const;
  RuntimeContractValidation contract_validation() const;
  RuntimePrivacyValidation privacy_validation() const;
  RuntimeWorkflowValidation workflow_validation() const;
  RuntimeTraceEvent record_trace_event(RuntimeTraceEvent event);
  RuntimeReplayTrace replay_trace() const;
  RuntimeReplayValidation replay_validation() const;

  std::uint64_t world_epoch() const;
  std::uint64_t watch_epoch() const;
  std::vector<RuntimeWatchEvent> watch_events() const;
  void begin_dependency_capture(std::uint64_t notebook_cell_id);
  RuntimeDependencySet end_dependency_capture();
  RuntimeDependencySet dependency_capture_snapshot() const;
  RuntimeWorldState world_state() const;
  bool is_world_frozen() const;
  std::uint64_t method_version(std::uint32_t class_index) const;
  std::size_t method_table_size(std::uint32_t class_index,
                                MethodTableSide side) const;
  RuntimePackageMirror package_mirror() const;
  std::optional<RuntimeOwnerMirror>
  owner_mirror(std::uint32_t owner_index) const;
  std::optional<RuntimeOwnerMirror>
  class_mirror(std::uint32_t class_index) const;
  std::optional<RuntimeOwnerMirror>
  mixin_mirror(std::uint32_t mixin_index) const;
  RuntimeWorldMirror world_mirror() const;
  RuntimeDispatchCacheStats dispatch_cache_stats() const;
  RuntimeHeapStats heap_stats() const;
  std::uint64_t drain_remote_frees();
  std::uint64_t drain_remote_frees(std::uint64_t worker_id);
  RuntimeWriteBarrierResult write_barrier(const Value &owner,
                                          const Value &value);
  RuntimeGcResult collect_garbage(const std::vector<Value> &roots = {},
                                  RuntimeGcCycle cycle = RuntimeGcCycle::Full);
  void request_garbage_collection(RuntimeGcCycle cycle = RuntimeGcCycle::Full);
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

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

ExecutionResult execute_code(const bytecode::BcModule &module,
                             std::uint32_t code_id,
                             const std::vector<Value> &args = {},
                             Value self = Value::null(),
                             Value block = Value::null());

std::string value_to_debug_string(const Value &value,
                                  const bytecode::BcModule *module = nullptr);

} // namespace amber::runtime
