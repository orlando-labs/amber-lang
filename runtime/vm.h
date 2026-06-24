#pragma once

#include "bytecode/format.h"
#include "package/package.h"
#include "profile/capabilities.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
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
struct ResultValue; // value-based Result[T,E] payload; defined after Value

// Intrusive strong-reference smart pointer for the six ObjHeader-bearing heap
// kinds (RESEARCH §7.2). The count lives in the object's ObjHeader, so this is
// a single 8-byte pointer with no separate control block. All element-touching
// logic is out-of-line in vm.cpp (declared here, explicitly instantiated there)
// so IntrusivePtr<T> works with an incomplete T at the many include sites, just
// like std::shared_ptr does.
template <class T> void runtime_heap_add_ref(T *obj) noexcept;
template <class T> void runtime_heap_release(T *obj) noexcept;

template <class T> class IntrusivePtr {
public:
  struct Adopt {};

  IntrusivePtr() noexcept = default;
  IntrusivePtr(std::nullptr_t) noexcept {}
  // Take ownership of an existing +1 reference without adding another.
  IntrusivePtr(T *ptr, Adopt) noexcept : ptr_(ptr) {}
  IntrusivePtr(const IntrusivePtr &other) noexcept : ptr_(other.ptr_) {
    if (ptr_ != nullptr) {
      runtime_heap_add_ref(ptr_);
    }
  }
  IntrusivePtr(IntrusivePtr &&other) noexcept : ptr_(other.ptr_) {
    other.ptr_ = nullptr;
  }
  IntrusivePtr &operator=(const IntrusivePtr &other) noexcept {
    if (other.ptr_ != nullptr) {
      runtime_heap_add_ref(other.ptr_);
    }
    if (ptr_ != nullptr) {
      runtime_heap_release(ptr_);
    }
    ptr_ = other.ptr_;
    return *this;
  }
  IntrusivePtr &operator=(IntrusivePtr &&other) noexcept {
    if (this != &other) {
      if (ptr_ != nullptr) {
        runtime_heap_release(ptr_);
      }
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }
  IntrusivePtr &operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }
  ~IntrusivePtr() {
    if (ptr_ != nullptr) {
      runtime_heap_release(ptr_);
    }
  }

  void reset() noexcept {
    if (ptr_ != nullptr) {
      runtime_heap_release(ptr_);
      ptr_ = nullptr;
    }
  }
  // Relinquish ownership of the managed pointer *without* releasing the
  // reference; the caller adopts the outstanding +1. Used by the tagged-Value
  // factories to move an IntrusivePtr's reference into the union with no
  // atomic.
  T *release() noexcept {
    T *p = ptr_;
    ptr_ = nullptr;
    return p;
  }
  T *get() const noexcept { return ptr_; }
  T *operator->() const noexcept { return ptr_; }
  T &operator*() const noexcept { return *ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  friend bool operator==(const IntrusivePtr &a,
                         const IntrusivePtr &b) noexcept {
    return a.ptr_ == b.ptr_;
  }
  friend bool operator!=(const IntrusivePtr &a,
                         const IntrusivePtr &b) noexcept {
    return a.ptr_ != b.ptr_;
  }
  friend bool operator==(const IntrusivePtr &a, std::nullptr_t) noexcept {
    return a.ptr_ == nullptr;
  }
  friend bool operator!=(const IntrusivePtr &a, std::nullptr_t) noexcept {
    return a.ptr_ != nullptr;
  }
  friend bool operator==(std::nullptr_t, const IntrusivePtr &a) noexcept {
    return a.ptr_ == nullptr;
  }
  friend bool operator!=(std::nullptr_t, const IntrusivePtr &a) noexcept {
    return a.ptr_ != nullptr;
  }

private:
  T *ptr_ = nullptr;
};

// Construct a standalone, heap-unregistered ObjHeader object (ref_count = 1,
// heap = null) wrapped in an IntrusivePtr. For white-box tests that build heap
// objects directly; on drop such an object is plain-deleted (no RuntimeHeap
// bookkeeping), matching how a bare `new`/`make_shared` object behaved before.
// Declared here, defined + explicitly instantiated for the six kinds in vm.cpp.
template <class T> IntrusivePtr<T> make_intrusive();

class RuntimeAtomic;
class RuntimeBarrier;
class RuntimeChannel;
class RuntimeFlowModule;
class RuntimeHeap;
class RuntimeMutex;
class RuntimeTaskHandle;
class RuntimeTaskModule;
class RuntimeLogger;
class RuntimeIoValue;
class RuntimeTextWriter;
class RuntimeThreadedCollection;
class RuntimeWatchCell;
class RuntimeWatchObjectState;
class RuntimeWatchHandle;
struct RuntimeArgParserValue;

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
  // Intrusive strong refcount (RESEARCH §7.2): replaces the per-object
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
  Kernel,
  Math,
  Json,
  Base64,
  Base64Url,
  Hex,
  Digest,
  Url,
  SecureRandom,
  ArgParser,
  Time,
  TimePeriod,
  Io,
  TextBuffer,
  Logger,
  Amber,
  Str,
  Int,
  BigInt,
  Float,
  Bool,
  Symbol,
  Array,
  Tuple,
  Set,
  Map,
  StrictMap,
  Null,
  Object,
  Range,
  Bytes,
  ByteBuffer,
  ByteSlice,
  IoPipe,
  Fs,
  FsPath,
  FsFile,
  Net,
  NetEndpoint,
  NetTcp,
  NetUdp,
  NetHttp,
  NetHttpClient,
  NetHttpRequest,
  Uuid
};

struct NativeTypeValue {
  RuntimeNativeTypeKind kind = RuntimeNativeTypeKind::TaskModule;
};

enum class RuntimeNativeFunctionKind {
  Print,
  P,
  Pp,
  Desc,
  ResultOk,
  ResultErr
};

struct NativeFunctionValue {
  RuntimeNativeFunctionKind kind = RuntimeNativeFunctionKind::Print;
};

// Builtin runtime error classes mirror spec/registries/runtime_errors.yaml.
// `error_id` indexes the registry-ordered name table (runtime_error_name).
struct NativeErrorClassValue {
  std::uint16_t error_id = 0;
};

struct ErrorInstanceValue;

// Arbitrary-precision integer per amber.numeric-profile.v1: explicit BigInt
// values only — fixed-width Int arithmetic never promotes into this type.
// Canonical form: little-endian base-2^64 magnitude with no trailing zero
// limbs; zero is an empty magnitude with negative == false.
struct BigIntValue {
  bool negative = false;
  std::vector<std::uint64_t> magnitude;
};

struct RuntimeTimeValue {
  std::int64_t epoch_seconds = 0;
  std::uint32_t nanosecond = 0;
};

struct RuntimeUuidValue {
  std::array<std::uint8_t, 16> bytes{};
};

struct RuntimeTimePeriodValue {
  std::int64_t months = 0;
  std::int64_t days = 0;
  std::int64_t nanoseconds = 0;
};

// Backs a `native class` instance: an opaque host pointer plus the
// deterministic lifetime model from the native-packages design (§7). Teardown
// runs through destroy!/memory.dealloc; the GC never runs an `owned` destructor
// (no implicit finalizer), and only a `collected` handle opts into GC
// reclamation. Held only via shared_ptr (single owner), so copying is disabled
// to prevent double-free.
struct RuntimeForeignHandle {
  enum class Ownership { Owned, Borrowed, Collected };

  std::string tag;     // per-(package,type) dispatch identity
  void *ptr = nullptr; // the wrapped foreign resource
  Ownership ownership = Ownership::Borrowed;
  // Reclaim callback: the context-free reclaim for `collected`, a ctx-bound
  // closure supplied by the ABI layer for `owned`, and empty for `borrowed`.
  // The ctx is the AmberCtx supplied at deterministic destroy!-time,
  // type-erased to void* so this header stays free of the amber_ext.h C ABI
  // types; an `owned` destructor uses it, a `collected` reclaim ignores it. The
  // GC reclaim path passes nullptr (collected only, context-free by
  // construction).
  std::function<void(void *ctx, void *ptr)> teardown;
  bool live = true; // tombstone: cleared once destroyed

  RuntimeForeignHandle() = default;
  RuntimeForeignHandle(const RuntimeForeignHandle &) = delete;
  RuntimeForeignHandle &operator=(const RuntimeForeignHandle &) = delete;

  // Deterministic destroy! / memory.dealloc: runs teardown once for an owning
  // handle, flips the tombstone, and reports whether this call destroyed it.
  // `ctx` is the live AmberCtx the destructor may use (owned); a collected
  // reclaim ignores it. A bytecode-only destroy! with no extension context
  // passes nullptr.
  bool destroy(void *ctx = nullptr) {
    if (!live) {
      return false;
    }
    live = false;
    if (ownership != Ownership::Borrowed && teardown) {
      teardown(ctx, ptr);
    }
    return true;
  }

  // GC reclamation: only a `collected` handle runs teardown here (the opt-in
  // finalizer). An `owned` handle never runs its destructor from the collector
  // (the leak is surfaced by a backstop diagnostic elsewhere); `borrowed` never
  // frees. The collector has no AmberCtx, so it passes nullptr — sound because
  // a `collected` reclaim is context-free by construction (design §7.4).
  ~RuntimeForeignHandle() {
    if (live && ownership == Ownership::Collected && teardown) {
      teardown(nullptr, ptr);
    }
  }
};

// Per-(package,type) descriptor for a `native class`: the dispatch tag plus the
// ownership and reclaim resolved from the manifest [[native.types]] and the
// linked extension symbols. The native binary registers one per type at
// startup; amber_make_handle(cx, tag, ptr) looks it up by tag to build a
// correctly-owned RuntimeForeignHandle. The ctx is type-erased to void* so this
// stays free of the amber_ext.h C ABI types.
struct NativeTypeDescriptor {
  std::string tag;
  RuntimeForeignHandle::Ownership ownership =
      RuntimeForeignHandle::Ownership::Borrowed;
  // ctx-bound destructor for `owned`; context-free reclaim for `collected`;
  // both null for `borrowed`.
  void (*owned_destructor)(void *ctx, void *handle) = nullptr;
  void (*collected_reclaim)(void *handle) = nullptr;
};

// The native binary's tag -> descriptor table. Populated once at startup by
// generated registration calls (one per [[native.types]] entry).
class NativeTagRegistry {
public:
  void register_type(NativeTypeDescriptor descriptor) {
    types_[descriptor.tag] = std::move(descriptor);
  }
  const NativeTypeDescriptor *lookup(const std::string &tag) const {
    const auto found = types_.find(tag);
    return found == types_.end() ? nullptr : &found->second;
  }
  std::size_t size() const { return types_.size(); }

private:
  std::unordered_map<std::string, NativeTypeDescriptor> types_;
};

const char *runtime_error_name(std::uint16_t error_id);
std::optional<std::uint16_t> runtime_error_id(const std::string &name);
bool runtime_error_is_a(std::uint16_t error_id,
                        std::uint16_t ancestor_error_id);
std::string big_int_to_decimal_string(const BigIntValue &value);
std::string runtime_uuid_to_string(const RuntimeUuidValue &value);
std::string runtime_time_to_iso8601(const RuntimeTimeValue &value);
std::string runtime_time_period_to_string(const RuntimeTimePeriodValue &value);

// Overflow policy for fixed-width Int arithmetic (amber.numeric-profile.v1).
// `checked` raises OverflowError; `wrapping` wraps two's-complement;
// `saturating` clamps to the type bounds.
enum class NumericOverflowMode : std::uint8_t { Checked, Wrapping, Saturating };

// Resolved module numeric profile: the selected overflow mode plus the bounds
// of the concrete `Int` width. Defaults describe the default profile
// (`int: Int64`, `overflow: checked`). `min == 0` marks unsigned widths.
struct NumericPolicy {
  NumericOverflowMode mode = NumericOverflowMode::Checked;
  std::int64_t min = std::numeric_limits<std::int64_t>::min();
  std::int64_t max = std::numeric_limits<std::int64_t>::max();
  std::uint32_t bits = 64;
};

// Maps a numeric profile `int` type name (e.g. "Int32", "UInt8") to bounds.
// Returns nullopt for types the reference VM cannot represent (UInt64,
// BigInt-as-Int) or unknown names.
std::optional<NumericPolicy> numeric_policy_for(const std::string &int_type,
                                                const std::string &overflow);

// The runtime `Value` has two interchangeable storage representations selected
// at build time by the VALUE_REPR Makefile flag (PLAN Phase 4 value-repr
// prototype). Both expose an identical public API -- the factories, `is_X()`
// predicates, and `as_X()` accessors below -- so the ~30k-line VM is
// source-compatible across either; only the storage and the method bodies (in
// vm.cpp) differ. The three call sites that previously read the variant
// directly now go through `kind_index()` / `integer_if()`, which both reps
// implement.
//
// X-macros listing every (factory, is_fn, as_fn, ElementType, TagEnumerator)
// tuple so the tagged rep can generate its factory/predicate/accessor bodies in
// lockstep (vm.cpp). Heap kinds (the six ObjHeader-bearing types, stored
// inline) take a ValueTag enumerator; tail kinds (the ~15 cold shared_ptr
// types, boxed) take a ValueTailKind enumerator.
#define AMBER_VALUE_HEAP_KINDS(X)                                              \
  X(closure, is_closure, as_closure, ClosureValue, Closure)                    \
  X(instance, is_instance_object, as_instance_object, InstanceValue, Instance) \
  X(list, is_list, as_list, ListValue, List)                                   \
  X(tuple, is_tuple, as_tuple, TupleValue, Tuple)                              \
  X(set, is_set, as_set, SetValue, Set)                                        \
  X(map, is_map, as_map, MapValue, Map)

#define AMBER_VALUE_TAIL_KINDS(X)                                              \
  X(error_instance, is_error_instance, as_error_instance, ErrorInstanceValue,  \
    ErrorInstance)                                                             \
  X(big_int, is_big_int, as_big_int, BigIntValue, BigInt)                      \
  X(task_module, is_task_module, as_task_module, RuntimeTaskModule,            \
    TaskModule)                                                                \
  X(task_handle, is_task_handle, as_task_handle, RuntimeTaskHandle,            \
    TaskHandle)                                                                \
  X(channel, is_channel, as_channel, RuntimeChannel, Channel)                  \
  X(mutex, is_mutex, as_mutex, RuntimeMutex, Mutex)                            \
  X(atomic, is_atomic, as_atomic, RuntimeAtomic, Atomic)                       \
  X(barrier, is_barrier, as_barrier, RuntimeBarrier, Barrier)                  \
  X(flow_module, is_flow_module, as_flow_module, RuntimeFlowModule, Flow)      \
  X(threaded_collection, is_threaded_collection, as_threaded_collection,       \
    RuntimeThreadedCollection, ThreadedCollection)                             \
  X(text_writer, is_text_writer, as_text_writer, RuntimeTextWriter,            \
    TextWriter)                                                                \
  X(logger, is_logger, as_logger, RuntimeLogger, Logger)                       \
  X(io_value, is_io_value, as_io_value, RuntimeIoValue, Io)                    \
  X(watch_cell, is_watch_cell, as_watch_cell, RuntimeWatchCell, WatchCell)     \
  X(watch_handle, is_watch_handle, as_watch_handle, RuntimeWatchHandle,        \
    WatchHandle)                                                               \
  X(result, is_result, as_result, ResultValue, Result)                         \
  X(arg_parser, is_arg_parser, as_arg_parser, RuntimeArgParserValue,           \
    ArgParser)                                                                 \
  X(time, is_time, as_time, RuntimeTimeValue, Time)                            \
  X(time_period, is_time_period, as_time_period, RuntimeTimePeriodValue,       \
    TimePeriod)                                                                \
  X(uuid, is_uuid, as_uuid, RuntimeUuidValue, Uuid)                            \
  X(foreign_handle, is_foreign_handle, as_foreign_handle,                      \
    RuntimeForeignHandle, ForeignHandle)

#ifndef AMBER_VALUE_REPR_TAGGED
// ---- Variant representation (default, 24 bytes) ---------------------------
struct Value {
  using Payload = std::variant<
      std::monostate, bool, std::int64_t, double, SymbolValue, StringValue,
      ClassObjectValue, IntrusivePtr<ClosureValue>, IntrusivePtr<InstanceValue>,
      IntrusivePtr<ListValue>, IntrusivePtr<TupleValue>, IntrusivePtr<SetValue>,
      IntrusivePtr<MapValue>, NativeTypeValue, NativeFunctionValue,
      NativeErrorClassValue, std::shared_ptr<ErrorInstanceValue>,
      std::shared_ptr<BigIntValue>, std::shared_ptr<RuntimeTaskModule>,
      std::shared_ptr<RuntimeTaskHandle>, std::shared_ptr<RuntimeChannel>,
      std::shared_ptr<RuntimeMutex>, std::shared_ptr<RuntimeAtomic>,
      std::shared_ptr<RuntimeBarrier>, std::shared_ptr<RuntimeFlowModule>,
      std::shared_ptr<RuntimeThreadedCollection>,
      std::shared_ptr<RuntimeTextWriter>, std::shared_ptr<RuntimeLogger>,
      std::shared_ptr<RuntimeIoValue>, std::shared_ptr<RuntimeWatchCell>,
      std::shared_ptr<RuntimeWatchHandle>, std::shared_ptr<ResultValue>,
      std::shared_ptr<RuntimeArgParserValue>, std::shared_ptr<RuntimeTimeValue>,
      std::shared_ptr<RuntimeTimePeriodValue>,
      std::shared_ptr<RuntimeUuidValue>, std::shared_ptr<RuntimeForeignHandle>>;

  Payload payload;

  static Value null();
  static Value boolean(bool value);
  static Value integer(std::int64_t value);
  static Value floating(double value);
  static Value symbol(std::uint32_t symbol_id);
  static Value string(std::uint32_t string_id);
  static Value class_object(std::uint32_t class_index);
  static Value closure(IntrusivePtr<ClosureValue> value);
  static Value instance(IntrusivePtr<InstanceValue> value);
  static Value list(IntrusivePtr<ListValue> value);
  static Value tuple(IntrusivePtr<TupleValue> value);
  static Value set(IntrusivePtr<SetValue> value);
  static Value map(IntrusivePtr<MapValue> value);
  static Value native_type(RuntimeNativeTypeKind kind);
  static Value native_function(RuntimeNativeFunctionKind kind);
  static Value native_error_class(std::uint16_t error_id);
  static Value error_instance(std::shared_ptr<ErrorInstanceValue> value);
  static Value big_int(std::shared_ptr<BigIntValue> value);
  static Value task_module(std::shared_ptr<RuntimeTaskModule> value);
  static Value task_handle(std::shared_ptr<RuntimeTaskHandle> value);
  static Value channel(std::shared_ptr<RuntimeChannel> value);
  static Value mutex(std::shared_ptr<RuntimeMutex> value);
  static Value atomic(std::shared_ptr<RuntimeAtomic> value);
  static Value barrier(std::shared_ptr<RuntimeBarrier> value);
  static Value flow_module(std::shared_ptr<RuntimeFlowModule> value);
  static Value
  threaded_collection(std::shared_ptr<RuntimeThreadedCollection> value);
  static Value text_writer(std::shared_ptr<RuntimeTextWriter> value);
  static Value logger(std::shared_ptr<RuntimeLogger> value);
  static Value io_value(std::shared_ptr<RuntimeIoValue> value);
  static Value watch_cell(std::shared_ptr<RuntimeWatchCell> value);
  static Value watch_handle(std::shared_ptr<RuntimeWatchHandle> value);
  static Value result(std::shared_ptr<ResultValue> value);
  static Value arg_parser(std::shared_ptr<RuntimeArgParserValue> value);
  static Value uuid(std::shared_ptr<RuntimeUuidValue> value);
  static Value foreign_handle(std::shared_ptr<RuntimeForeignHandle> value);
  static Value time(std::shared_ptr<RuntimeTimeValue> value);
  static Value time_period(std::shared_ptr<RuntimeTimePeriodValue> value);

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
  bool is_native_function() const;
  bool is_native_error_class() const;
  bool is_error_instance() const;
  bool is_big_int() const;
  bool is_task_module() const;
  bool is_task_handle() const;
  bool is_channel() const;
  bool is_mutex() const;
  bool is_atomic() const;
  bool is_barrier() const;
  bool is_flow_module() const;
  bool is_threaded_collection() const;
  bool is_text_writer() const;
  bool is_logger() const;
  bool is_io_value() const;
  bool is_watch_cell() const;
  bool is_watch_handle() const;
  bool is_result() const;
  bool is_arg_parser() const;
  bool is_uuid() const;
  bool is_foreign_handle() const;
  bool is_time() const;
  bool is_time_period() const;

  bool as_bool() const;
  std::int64_t as_integer() const;
  double as_float() const;
  SymbolValue as_symbol() const;
  StringValue as_string() const;
  ClassObjectValue as_class_object() const;
  IntrusivePtr<ClosureValue> as_closure() const;
  IntrusivePtr<InstanceValue> as_instance_object() const;
  IntrusivePtr<ListValue> as_list() const;
  IntrusivePtr<TupleValue> as_tuple() const;
  IntrusivePtr<SetValue> as_set() const;
  IntrusivePtr<MapValue> as_map() const;
  NativeTypeValue as_native_type() const;
  NativeFunctionValue as_native_function() const;
  NativeErrorClassValue as_native_error_class() const;
  std::shared_ptr<ErrorInstanceValue> as_error_instance() const;
  std::shared_ptr<BigIntValue> as_big_int() const;
  std::shared_ptr<RuntimeTaskModule> as_task_module() const;
  std::shared_ptr<RuntimeTaskHandle> as_task_handle() const;
  std::shared_ptr<RuntimeChannel> as_channel() const;
  std::shared_ptr<RuntimeMutex> as_mutex() const;
  std::shared_ptr<RuntimeAtomic> as_atomic() const;
  std::shared_ptr<RuntimeBarrier> as_barrier() const;
  std::shared_ptr<RuntimeFlowModule> as_flow_module() const;
  std::shared_ptr<RuntimeThreadedCollection> as_threaded_collection() const;
  std::shared_ptr<RuntimeTextWriter> as_text_writer() const;
  std::shared_ptr<RuntimeLogger> as_logger() const;
  std::shared_ptr<RuntimeIoValue> as_io_value() const;
  std::shared_ptr<RuntimeWatchCell> as_watch_cell() const;
  std::shared_ptr<RuntimeWatchHandle> as_watch_handle() const;
  std::shared_ptr<ResultValue> as_result() const;
  std::shared_ptr<RuntimeArgParserValue> as_arg_parser() const;
  std::shared_ptr<RuntimeUuidValue> as_uuid() const;
  std::shared_ptr<RuntimeForeignHandle> as_foreign_handle() const;
  std::shared_ptr<RuntimeTimeValue> as_time() const;
  std::shared_ptr<RuntimeTimePeriodValue> as_time_period() const;

  // Representation-agnostic helpers (see the doc comment above): a distinct
  // value per active alternative, and a pointer to the inline integer payload
  // (or nullptr). Replace the three former direct-variant call sites.
  std::uint32_t kind_index() const;
  const std::int64_t *integer_if() const;
};
static_assert(sizeof(Value) == 24,
              "variant Value is expected to be 24 bytes on this platform");
#else
// ---- Tagged representation (PLAN Phase 4 prototype, 16 bytes) -------------
// Immediates and the six ObjHeader heap kinds live inline in an 8-byte union;
// the ~15 cold tail kinds are boxed behind a refcounted ValueTailBox (one extra
// allocation + indirection per tail value, all cold paths -- BigInt/error/io/
// task/sync). Copy/move/destroy manage the intrusive refcount of the heap
// kinds and the box refcount manually.
enum class ValueTag : std::uint8_t {
  Null,
  Bool,
  Int,
  Float,
  Symbol,
  String,
  ClassObject,
  NativeType,
  NativeFunction,
  NativeErrorClass,
  // ObjHeader-bearing heap kinds; the union holds an ObjHeader* and the tag
  // names the concrete type. MUST stay contiguous Closure..Map (range-checked).
  Closure,
  Instance,
  List,
  Tuple,
  Set,
  Map,
  // Boxed tail kinds; the union holds a ValueTailBox* whose ValueTailKind names
  // the concrete shared_ptr type.
  Tail,
};

enum class ValueTailKind : std::uint8_t {
  ErrorInstance,
  BigInt,
  TaskModule,
  TaskHandle,
  Channel,
  Mutex,
  Atomic,
  Barrier,
  Flow,
  ThreadedCollection,
  TextWriter,
  Logger,
  Io,
  WatchCell,
  WatchHandle,
  Result,
  ArgParser,
  Time,
  TimePeriod,
  Uuid,
  ForeignHandle,
};

struct ValueTailBox; // refcounted tail box; defined in vm.cpp

struct Value {
  Value() noexcept : tag_(ValueTag::Null) { u_.i = 0; }
  Value(const Value &other) noexcept;
  Value(Value &&other) noexcept;
  Value &operator=(const Value &other) noexcept;
  Value &operator=(Value &&other) noexcept;
  ~Value();

  static Value null();
  static Value boolean(bool value);
  static Value integer(std::int64_t value);
  static Value floating(double value);
  static Value symbol(std::uint32_t symbol_id);
  static Value string(std::uint32_t string_id);
  static Value class_object(std::uint32_t class_index);
  static Value closure(IntrusivePtr<ClosureValue> value);
  static Value instance(IntrusivePtr<InstanceValue> value);
  static Value list(IntrusivePtr<ListValue> value);
  static Value tuple(IntrusivePtr<TupleValue> value);
  static Value set(IntrusivePtr<SetValue> value);
  static Value map(IntrusivePtr<MapValue> value);
  static Value native_type(RuntimeNativeTypeKind kind);
  static Value native_function(RuntimeNativeFunctionKind kind);
  static Value native_error_class(std::uint16_t error_id);
  static Value error_instance(std::shared_ptr<ErrorInstanceValue> value);
  static Value big_int(std::shared_ptr<BigIntValue> value);
  static Value task_module(std::shared_ptr<RuntimeTaskModule> value);
  static Value task_handle(std::shared_ptr<RuntimeTaskHandle> value);
  static Value channel(std::shared_ptr<RuntimeChannel> value);
  static Value mutex(std::shared_ptr<RuntimeMutex> value);
  static Value atomic(std::shared_ptr<RuntimeAtomic> value);
  static Value barrier(std::shared_ptr<RuntimeBarrier> value);
  static Value flow_module(std::shared_ptr<RuntimeFlowModule> value);
  static Value
  threaded_collection(std::shared_ptr<RuntimeThreadedCollection> value);
  static Value text_writer(std::shared_ptr<RuntimeTextWriter> value);
  static Value logger(std::shared_ptr<RuntimeLogger> value);
  static Value io_value(std::shared_ptr<RuntimeIoValue> value);
  static Value watch_cell(std::shared_ptr<RuntimeWatchCell> value);
  static Value watch_handle(std::shared_ptr<RuntimeWatchHandle> value);
  static Value result(std::shared_ptr<ResultValue> value);
  static Value arg_parser(std::shared_ptr<RuntimeArgParserValue> value);
  static Value uuid(std::shared_ptr<RuntimeUuidValue> value);
  static Value foreign_handle(std::shared_ptr<RuntimeForeignHandle> value);
  static Value time(std::shared_ptr<RuntimeTimeValue> value);
  static Value time_period(std::shared_ptr<RuntimeTimePeriodValue> value);

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
  bool is_native_function() const;
  bool is_native_error_class() const;
  bool is_error_instance() const;
  bool is_big_int() const;
  bool is_task_module() const;
  bool is_task_handle() const;
  bool is_channel() const;
  bool is_mutex() const;
  bool is_atomic() const;
  bool is_barrier() const;
  bool is_flow_module() const;
  bool is_threaded_collection() const;
  bool is_text_writer() const;
  bool is_logger() const;
  bool is_io_value() const;
  bool is_watch_cell() const;
  bool is_watch_handle() const;
  bool is_result() const;
  bool is_arg_parser() const;
  bool is_uuid() const;
  bool is_foreign_handle() const;
  bool is_time() const;
  bool is_time_period() const;

  bool as_bool() const;
  std::int64_t as_integer() const;
  double as_float() const;
  SymbolValue as_symbol() const;
  StringValue as_string() const;
  ClassObjectValue as_class_object() const;
  IntrusivePtr<ClosureValue> as_closure() const;
  IntrusivePtr<InstanceValue> as_instance_object() const;
  IntrusivePtr<ListValue> as_list() const;
  IntrusivePtr<TupleValue> as_tuple() const;
  IntrusivePtr<SetValue> as_set() const;
  IntrusivePtr<MapValue> as_map() const;
  NativeTypeValue as_native_type() const;
  NativeFunctionValue as_native_function() const;
  NativeErrorClassValue as_native_error_class() const;
  std::shared_ptr<ErrorInstanceValue> as_error_instance() const;
  std::shared_ptr<BigIntValue> as_big_int() const;
  std::shared_ptr<RuntimeTaskModule> as_task_module() const;
  std::shared_ptr<RuntimeTaskHandle> as_task_handle() const;
  std::shared_ptr<RuntimeChannel> as_channel() const;
  std::shared_ptr<RuntimeMutex> as_mutex() const;
  std::shared_ptr<RuntimeAtomic> as_atomic() const;
  std::shared_ptr<RuntimeBarrier> as_barrier() const;
  std::shared_ptr<RuntimeFlowModule> as_flow_module() const;
  std::shared_ptr<RuntimeThreadedCollection> as_threaded_collection() const;
  std::shared_ptr<RuntimeTextWriter> as_text_writer() const;
  std::shared_ptr<RuntimeLogger> as_logger() const;
  std::shared_ptr<RuntimeIoValue> as_io_value() const;
  std::shared_ptr<RuntimeWatchCell> as_watch_cell() const;
  std::shared_ptr<RuntimeWatchHandle> as_watch_handle() const;
  std::shared_ptr<ResultValue> as_result() const;
  std::shared_ptr<RuntimeArgParserValue> as_arg_parser() const;
  std::shared_ptr<RuntimeUuidValue> as_uuid() const;
  std::shared_ptr<RuntimeForeignHandle> as_foreign_handle() const;
  std::shared_ptr<RuntimeTimeValue> as_time() const;
  std::shared_ptr<RuntimeTimePeriodValue> as_time_period() const;

  std::uint32_t kind_index() const;
  const std::int64_t *integer_if() const;

private:
  union Storage {
    bool b;
    std::int64_t i;
    double d;
    std::uint32_t u32; // symbol_id / string_id / class_index
    std::uint16_t u16; // native error_id
    RuntimeNativeTypeKind ntype;
    RuntimeNativeFunctionKind nfn;
    ObjHeader *obj;     // Closure..Map (points at the embedded header)
    ValueTailBox *tail; // Tail
  };

  ValueTag tag_;
  Storage u_;

  // Build a boxed tail Value (one heap allocation for the ValueTailBox).
  static Value make_tail(ValueTailKind kind, std::shared_ptr<void> ptr);
  // Build an inline heap-kind Value, transferring `value`'s +1 reference.
  template <class T>
  static Value make_heap(ValueTag tag, IntrusivePtr<T> value);

  static void retain_payload(ValueTag tag, const Storage &storage) noexcept;
  static void release_payload(ValueTag tag, Storage &storage) noexcept;
};
static_assert(sizeof(Value) <= 16, "tagged Value must fit in 16 bytes");
#endif // AMBER_VALUE_REPR_TAGGED

// Value-based Result[T,E] (Ok/Err). A cold tail kind: a single payload slot
// holds either the Ok value or the Err value, discriminated by `is_ok`. Defined
// after Value because it embeds a Value member.
struct ResultValue {
  bool is_ok = false;
  Value payload = Value::null();
};

struct ErrorInstanceValue {
  std::uint16_t error_id = 0;
  std::string message;
  std::vector<std::pair<std::string, Value>> fields;
};

struct RuntimeArgParserValue {
  enum class SpecKind { Option, Flag, Positional, Rest };

  struct Spec {
    SpecKind kind = SpecKind::Option;
    std::vector<std::string> spellings;
    std::string name;
    RuntimeNativeTypeKind type = RuntimeNativeTypeKind::Str;
    bool required = false;
    bool multiple = false;
    bool negatable = false;
    bool has_default = false;
    Value default_value = Value::null();
    bool has_choices = false;
    std::vector<Value> choices;
    std::string env;
    Value block = Value::null();
  };

  std::string name;
  std::string about;
  std::vector<std::string> cmdline;
  std::vector<std::pair<std::string, std::string>> env;
  bool add_help = true;
  std::vector<Spec> specs;
};

struct RuntimeTextWriteResult {
  bool ok = true;
  std::string error_name;
  std::string message;
};

struct RuntimeTextSourceLocation {
  bool present = false;
  std::uint32_t code_id = 0;
  std::uint32_t pc = 0;
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

struct RuntimeTextOutputEvent {
  std::string stream;
  std::string text;
  std::uint64_t order = 0;
  RuntimeTextSourceLocation source;
};

class RuntimeTextWriter {
public:
  RuntimeTextWriter();
  RuntimeTextWriter(const RuntimeTextWriter &) = delete;
  RuntimeTextWriter &operator=(const RuntimeTextWriter &) = delete;
  RuntimeTextWriter(RuntimeTextWriter &&) noexcept;
  RuntimeTextWriter &operator=(RuntimeTextWriter &&) noexcept;
  ~RuntimeTextWriter();

  static std::shared_ptr<RuntimeTextWriter> host_stdout();
  static std::shared_ptr<RuntimeTextWriter> host_stderr();
  static std::shared_ptr<RuntimeTextWriter> buffer();
  static std::shared_ptr<RuntimeTextWriter>
  cell_stream(std::string stream_name);

  RuntimeTextWriteResult write_str(const std::string &text);
  RuntimeTextWriteResult write_line(const std::string &text = {});
  RuntimeTextWriteResult flush();
  RuntimeTextWriteResult close();
  bool closed() const;
  bool buffered() const;
  bool xterm_color_available() const;
  std::string to_string() const;
  std::vector<RuntimeTextOutputEvent> events() const;
  std::string stream_name() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

std::shared_ptr<RuntimeTextWriter> current_runtime_stdout();
std::shared_ptr<RuntimeTextWriter> current_runtime_stderr();

class RuntimeOutputScope {
public:
  RuntimeOutputScope(std::shared_ptr<RuntimeTextWriter> stdout_writer = {},
                     std::shared_ptr<RuntimeTextWriter> stderr_writer = {});
  RuntimeOutputScope(const RuntimeOutputScope &) = delete;
  RuntimeOutputScope &operator=(const RuntimeOutputScope &) = delete;
  ~RuntimeOutputScope();

private:
  std::shared_ptr<RuntimeTextWriter> previous_stdout_;
  std::shared_ptr<RuntimeTextWriter> previous_stderr_;
};

enum class RuntimeLogLevel {
  Fatal = 0,
  Error = 1,
  Warn = 2,
  Info = 3,
  Debug = 4
};

enum class RuntimeLogColorMode { Auto, Always, Never };

std::uint64_t current_runtime_native_thread_id();
std::string current_runtime_task_annotation();

class RuntimeTaskAnnotationScope {
public:
  explicit RuntimeTaskAnnotationScope(std::string annotation);
  RuntimeTaskAnnotationScope(const RuntimeTaskAnnotationScope &) = delete;
  RuntimeTaskAnnotationScope &
  operator=(const RuntimeTaskAnnotationScope &) = delete;
  ~RuntimeTaskAnnotationScope();

private:
  std::string previous_annotation_;
};

class RuntimeLogger {
public:
  explicit RuntimeLogger(
      std::shared_ptr<RuntimeTextWriter> writer = {},
      RuntimeLogLevel level = RuntimeLogLevel::Info,
      RuntimeLogColorMode color_mode = RuntimeLogColorMode::Auto);
  RuntimeLogger(const RuntimeLogger &) = delete;
  RuntimeLogger &operator=(const RuntimeLogger &) = delete;
  RuntimeLogger(RuntimeLogger &&) noexcept;
  RuntimeLogger &operator=(RuntimeLogger &&) noexcept;
  ~RuntimeLogger();

  RuntimeTextWriteResult log(RuntimeLogLevel level, const std::string &message);
  RuntimeTextWriteResult fatal(const std::string &message);
  RuntimeTextWriteResult error(const std::string &message);
  RuntimeTextWriteResult warn(const std::string &message);
  RuntimeTextWriteResult info(const std::string &message);
  RuntimeTextWriteResult debug(const std::string &message);
  RuntimeTextWriteResult flush();
  RuntimeTextWriteResult close();
  bool closed() const;
  RuntimeLogLevel level() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

enum class RuntimeStringifyMode { Display, Inspect, Pretty };

struct RuntimePrettyPrintOptions {
  std::size_t max_width = 80;
  std::size_t max_depth = 20;
  std::size_t max_items = 100;
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
  // §9 fragmentation ratio (RSS / live heap bytes), not an exact live-byte
  // total.
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

enum class RuntimeIoWaitInterest {
  Other,
  Read,
  Write,
  Accept,
  Connect,
  Flush,
  Close,
  Metadata,
  Open
};

struct RuntimeIoWaitRecord {
  std::uint64_t wait_id = 0;
  std::uint64_t task_id = 0;
  std::uint64_t strand_id = 0;
  std::uint64_t worker_id = 0;
  std::uint64_t resource_id = 0;
  RuntimeIoWaitInterest interest = RuntimeIoWaitInterest::Other;
  std::string operation;
  std::string resource;
  bool has_timeout = false;
  std::int64_t timeout_millis = 0;
};

using RuntimeIoWaitObserver =
    std::function<void(const RuntimeIoWaitRecord &, bool entering)>;

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
  // Layer B cooperative suspension: a running strand parked itself (releasing
  // its worker) rather than blocking it. park_resumes counts re-dispatches of a
  // previously parked strand.
  std::uint64_t parks = 0;
  std::uint64_t park_resumes = 0;
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

  // Layer B cooperative suspension. Called by the currently-running strand
  // (from within its strand function) to request that, when the function
  // returns, the strand be parked and its worker released to run other strands,
  // rather than finalized. The same strand function is re-invoked when the
  // strand is woken, so it must resume its work (e.g. continue a persisted VM)
  // rather than restart it. With `wake_after` a timer wake is scheduled;
  // otherwise the strand stays parked until an explicit wake_strand(). Returns
  // false when there is no current scheduler strand, in which case the caller
  // must fall back to blocking.
  bool park_current(
      std::optional<std::chrono::milliseconds> wake_after = std::nullopt);

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

enum class RuntimeFlowPartitionPolicy { Items, Chunks, Stride, Atomic };

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
  std::uint64_t filter_map_operations = 0;
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
                                     RuntimeFlowOptions options = {},
                                     RuntimeFlowPartitionPolicy scatter_policy =
                                         RuntimeFlowPartitionPolicy::Atomic);
  RuntimeThreadedCollection(const RuntimeThreadedCollection &) = delete;
  RuntimeThreadedCollection &
  operator=(const RuntimeThreadedCollection &) = delete;
  RuntimeThreadedCollection(RuntimeThreadedCollection &&) noexcept;
  RuntimeThreadedCollection &operator=(RuntimeThreadedCollection &&) noexcept;
  ~RuntimeThreadedCollection();

  RuntimeFlowGatherResult each(EachFunction function);
  RuntimeFlowGatherResult map(MapFunction function);
  RuntimeFlowGatherResult filter_map(MapFunction function);
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
  // zero (RESEARCH §7.2). Public only so the out-of-line runtime_heap_release
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
// Wrap a payload into an Ok (is_ok=true) or Err (is_ok=false) Result value.
Value make_result_value(bool is_ok, Value payload);

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

struct RuntimeIoProviderStatus {
  bool handled = false;
  bool ok = false;
  bool boolean = false;
  std::uint64_t size = 0;
  bool file = false;
  bool directory = false;
  bool symlink = false;
  std::size_t count = 0;
  std::string bytes;
  std::string error_name;
  std::string message;
};

class RuntimeIoProvider {
public:
  virtual ~RuntimeIoProvider() = default;

  virtual RuntimeIoProviderStatus fs_exists(const std::string &path);
  virtual RuntimeIoProviderStatus fs_file(const std::string &path);
  virtual RuntimeIoProviderStatus fs_dir(const std::string &path);
  virtual RuntimeIoProviderStatus fs_metadata(const std::string &path);
  virtual RuntimeIoProviderStatus
  fs_read_bytes(const std::string &path, std::optional<std::size_t> limit);
  virtual RuntimeIoProviderStatus fs_write_bytes(const std::string &path,
                                                 const std::string &bytes,
                                                 bool create, bool truncate,
                                                 bool append = false);
  virtual RuntimeIoProviderStatus fs_mkdir(const std::string &path);
  virtual RuntimeIoProviderStatus fs_mkdir_p(const std::string &path);
  virtual RuntimeIoProviderStatus fs_remove(const std::string &path);
  virtual RuntimeIoProviderStatus fs_rename(const std::string &from,
                                            const std::string &to);
  virtual RuntimeIoProviderStatus fs_copy(const std::string &from,
                                          const std::string &to);
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
  std::shared_ptr<RuntimeIoProvider> io_provider;
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
                  std::uint64_t result_watch_epoch = 0,
                  std::vector<std::string> result_runtime_strings = {},
                  std::vector<std::string> result_runtime_symbols = {})
      : value(std::move(result_value)), fault(std::move(result_fault)),
        locals(std::move(result_locals)),
        watch_events(std::move(result_watch_events)),
        watch_epoch(result_watch_epoch),
        runtime_strings(std::move(result_runtime_strings)),
        runtime_symbols(std::move(result_runtime_symbols)) {}

  Value value = Value::null();
  std::optional<Fault> fault;
  std::vector<ExecutionLocal> locals;
  std::vector<RuntimeWatchEvent> watch_events;
  std::uint64_t watch_epoch = 0;
  std::vector<std::string> runtime_strings;
  std::vector<std::string> runtime_symbols;

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

// Layer B observability/test hook: process-wide count of cooperative task
// parks (a task body suspended at a suspension point such as task.sleep,
// releasing its worker, rather than blocking it). Monotonic; snapshot it before
// and after a run to assert the cooperative path was taken.
std::uint64_t runtime_cooperative_task_park_count();

std::string value_to_debug_string(
    const Value &value, const bytecode::BcModule *module = nullptr,
    const std::vector<std::string> *runtime_strings = nullptr,
    const std::vector<std::string> *runtime_symbols = nullptr);

} // namespace amber::runtime
