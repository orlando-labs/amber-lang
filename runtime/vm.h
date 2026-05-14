#pragma once

#include "bytecode/format.h"

#include <cstddef>
#include <cstdint>
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
struct MapValue;
struct MapEntry;

enum class HeapObjectKind { Instance, List, Tuple, Map, Closure };

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

struct Value {
  using Payload =
      std::variant<std::monostate, bool, std::int64_t, double, SymbolValue,
                   StringValue, ClassObjectValue, std::shared_ptr<ClosureValue>,
                   std::shared_ptr<InstanceValue>, std::shared_ptr<ListValue>,
                   std::shared_ptr<TupleValue>, std::shared_ptr<MapValue>>;

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
  bool is_map() const;

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
  std::shared_ptr<MapValue> as_map() const;
};

struct InstanceValue {
  ObjHeader header;
  std::uint32_t class_index = 0;
  std::vector<Value> ivar_storage;
  std::uint64_t ivar_shape_version = 1;
  std::unordered_map<std::string, Value> ivars;
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

class RuntimeWorkerScope {
public:
  explicit RuntimeWorkerScope(std::uint64_t worker_id);
  RuntimeWorkerScope(const RuntimeWorkerScope &) = delete;
  RuntimeWorkerScope &operator=(const RuntimeWorkerScope &) = delete;
  ~RuntimeWorkerScope();

private:
  std::uint64_t previous_worker_id_ = 0;
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
Value make_symbol_map_value(std::vector<MapEntry> entries, bool frozen = false);

enum class MethodTableSide { Instance, Class };

struct TraceFrame {
  std::uint32_t code_id = 0;
  std::uint32_t pc = 0;
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
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

struct ExecutionResult {
  Value value = Value::null();
  std::optional<Fault> fault;

  bool ok() const { return !fault.has_value(); }
};

class RuntimeWorld {
public:
  explicit RuntimeWorld(const bytecode::BcModule &module);
  ~RuntimeWorld();

  ExecutionResult execute(std::uint32_t code_id,
                          const std::vector<Value> &args = {},
                          Value self = Value::null(),
                          Value block = Value::null());

  ExecutionResult define_instance_method(std::uint32_t class_index,
                                         bytecode::BcMethod method);
  ExecutionResult include_mixin(std::uint32_t class_index,
                                std::uint32_t mixin_index);
  ExecutionResult extend_mixin(std::uint32_t class_index,
                               std::uint32_t mixin_index);

  std::uint64_t world_epoch() const;
  std::uint64_t method_version(std::uint32_t class_index) const;
  std::size_t method_table_size(std::uint32_t class_index,
                                MethodTableSide side) const;
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
