#pragma once

#include "runtime/value.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace amber::runtime {

class RuntimeWatchObjectState;

enum class HeapObjectKind { Instance, List, Tuple, Set, Map, Closure };

enum class OwnerTokenKind { Shareable, Confined, Sync };

enum class ObjectLifetimeState { Live, Destroying, Destroyed, Deallocated };

enum class ObjectGeneration { Young, Mature, Shared };

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
  // Intrusive strong refcount (RESEARCH 7.2): replaces the per-object
  // shared_ptr control block. Managed by IntrusivePtr via runtime_heap_add_ref
  // / runtime_heap_release. An atomic makes ObjHeader non-copyable, which is
  // intended -- these objects are only ever referenced through pointers.
  std::atomic<std::uint32_t> ref_count{0};
  // Type-erased keepalive for the owning RuntimeHeap::Impl (Impl is private to
  // vm.cpp, hence shared_ptr<void>). It locates the heap on the drop path
  // (RuntimeHeap::drop_object) and guarantees the heap outlives its objects --
  // the same lifetime contract the old shared_ptr deleter's Impl capture gave.
  std::shared_ptr<void> heap;
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
  Value key = Value::null();
  Value value = Value::null();

  MapEntry() = default;
  MapEntry(std::uint32_t key_symbol_id, Value entry_value);
  MapEntry(Value entry_key, Value entry_value);
};

struct MapValue {
  ObjHeader header;
  std::vector<MapEntry> entries;
  bool frozen = false;
  // Exact-key (StrictMap / StrictHashMap) vs name-indifferent ordinary Map /
  // HashMap (spec v20.7/v20.8). Ordinary maps treat a Symbol key and a Str key
  // with the same text as the same key for lookup/dedup/pattern matching, while
  // preserving each entry's original key Value for keys()/iteration/display;
  // strict maps keep Symbol and Str keys distinct. See MapEntry::symbol_id,
  // which carries the canonical key identity used by ordinary maps.
  bool strict = false;
};

struct ClosureValue {
  ObjHeader header;
  std::uint32_t code_id = 0;
  std::vector<Value> captures;
  Value self = Value::null();
};

bool value_has_heap_payload_tag(const Value &value);
const ObjHeader *heap_header_from_value(const Value &value);
ObjHeader *mutable_heap_header_from_value(const Value &value);
bool header_is_deallocated(const ObjHeader &header);
bool header_is_destroyed(const ObjHeader &header);
std::optional<std::string> lifecycle_access_error_name(const ObjHeader &header);
std::string lifecycle_access_error_message(const std::string &error_name);
std::string lifecycle_debug_label(const ObjHeader &header);

} // namespace amber::runtime
