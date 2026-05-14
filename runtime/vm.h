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

struct OwnerToken {
  OwnerTokenKind kind = OwnerTokenKind::Confined;
  std::uint64_t strand_id = 0;
};

inline constexpr std::uint32_t kObjectFlagFrozen = 0x1U;
inline constexpr std::uint32_t kObjectFlagShareable = 0x2U;
inline constexpr std::uint32_t kObjectFlagDead = 0x4U;
inline constexpr std::uint32_t kObjectFlagDestroyed = 0x8U;
inline constexpr std::uint32_t kObjectFlagDestroying = 0x10U;

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
  std::vector<RuntimeArenaStats> arenas;
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
  RuntimeHeapStats stats() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
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
