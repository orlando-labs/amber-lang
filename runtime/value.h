#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace amber::ast {
struct Expr; // homoiconic AST node reused as the macro `Ast` value model
} // namespace amber::ast

namespace amber::runtime {

struct ClosureValue;
struct InstanceValue;
struct ListValue;
struct TupleValue;
struct SetValue;
struct MapValue;
struct MapEntry;
struct ObjHeader;
struct ResultValue; // value-based Result[T,E] payload; defined after Value
struct ErrorInstanceValue;
struct BigIntValue;
struct RuntimeTimeValue;
struct RuntimeTimeZoneValue;
struct RuntimeTimePeriodValue;
struct RuntimeUuidValue;
struct RuntimeRegexpPatternValue;
struct RuntimeRegexpMatchValue;
struct RuntimeForeignHandle;
struct RuntimeArgParserValue;

// First-class macro `Ast` value (macro.v1 profile, DESIGN-macro-system §4).
// Immutable wrapper over a parsed/expanded `amber.ast.v1` node. `node` is an
// aliasing shared_ptr into a tree owned by `root`, so subtrees are shareable
// without copying; `source` (may be null) is the original module text so
// `.source` / `.to_source` can return the verbatim span slice.
struct RuntimeAstNode {
  std::shared_ptr<const ast::Expr> root;
  const ast::Expr *node = nullptr;
  std::shared_ptr<const std::string> source;
};

// Accessors implemented in value.cpp (the one runtime TU that includes the AST
// header), so other runtime files can render/introspect an Ast value without
// depending on frontend/ast. `kind` returns the node's `amber.ast.v1` kind
// string; `source` returns the verbatim source-text slice for the node's span
// (empty when no source is retained).
std::string runtime_ast_node_kind(const RuntimeAstNode &node);
std::string runtime_ast_node_source(const RuntimeAstNode &node);

class RuntimeAtomic;
class RuntimeBarrier;
class RuntimeChannel;
class RuntimeFlowModule;
class RuntimeIoValue;
class RuntimeLogger;
class RuntimeMutex;
class RuntimeTaskHandle;
class RuntimeTaskModule;
class RuntimeTextWriter;
class RuntimeThreadedCollection;
class RuntimeWatchCell;
class RuntimeWatchHandle;

// Intrusive strong-reference smart pointer for the six ObjHeader-bearing heap
// kinds (RESEARCH 7.2). The count lives in the object's ObjHeader, so this is
// a single 8-byte pointer with no separate control block. All element-touching
// logic is out-of-line in heap.cpp (declared here, explicitly instantiated
// there) so IntrusivePtr<T> works with an incomplete T at the many include
// sites, just like std::shared_ptr does.
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
  // Relinquish ownership of the managed pointer without releasing the
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
// Declared here, defined + explicitly instantiated for the six kinds in
// heap.cpp.
template <class T> IntrusivePtr<T> make_intrusive();

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
  Benchmark,
  Url,
  SecureRandom,
  ArgParser,
  Regexp,
  Time,
  TimePeriod,
  Io,
  TextBuffer,
  Logger,
  Amber,
  Ast,
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
  NetHttpRequestBody,
  NetHttpHeaders,
  NetHttpServer,
  NetHttpServerRequest,
  NetHttpServerResponse,
  NetHttpJson,
  NetHttpJsonGetJson,
  NetHttpJsonPostJson,
  NetHttpForm,
  NetHttpFormBody,
  Uuid,
  TimeZone,
  Yaml
};

struct NativeTypeValue {
  RuntimeNativeTypeKind kind = RuntimeNativeTypeKind::TaskModule;
};

const char *native_type_name(RuntimeNativeTypeKind kind);

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

const char *native_function_name(RuntimeNativeFunctionKind kind);

// Builtin runtime error classes mirror spec/registries/runtime_errors.yaml.
// `error_id` indexes the registry-ordered name table (runtime_error_name).
struct NativeErrorClassValue {
  std::uint16_t error_id = 0;
};

struct NativeErrorNamespaceValue {
  std::string path;
};

// Arbitrary-precision integer per amber.numeric-profile.v1: explicit BigInt
// values only; fixed-width Int arithmetic never promotes into this type.
// Canonical form: little-endian base-2^64 magnitude with no trailing zero
// limbs; zero is an empty magnitude with negative == false.
struct BigIntValue {
  bool negative = false;
  std::vector<std::uint64_t> magnitude;
};

std::string big_int_to_decimal_string(const BigIntValue &value);

struct RuntimeTimeValue {
  std::int64_t epoch_seconds = 0;
  std::uint32_t nanosecond = 0;
  std::int32_t zone_offset_seconds = 0;
  std::string zone_name = "UTC";
  bool zone_fixed_offset = true;
};

struct RuntimeTimeZoneValue {
  std::int32_t offset_seconds = 0;
  std::string name = "UTC";
  bool fixed_offset = true;
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
  // frees. The collector has no AmberCtx, so it passes nullptr -- sound because
  // a `collected` reclaim is context-free by construction (design §7.4).
  ~RuntimeForeignHandle() {
    if (live && ownership == Ownership::Collected && teardown) {
      teardown(nullptr, ptr);
    }
  }
};

// The runtime `Value` has two interchangeable storage representations selected
// at build time by the VALUE_REPR Makefile flag (PLAN Phase 4 value-repr
// prototype). Both expose an identical public API -- the factories, `is_X()`
// predicates, and `as_X()` accessors below -- so the VM is source-compatible
// across either; only the storage and the method bodies (in value.cpp) differ.
// The three call sites that previously read the variant directly now go through
// `kind_index()` / `integer_if()`, which both reps implement.
//
// X-macros listing every (factory, is_fn, as_fn, ElementType, TagEnumerator)
// tuple so the tagged rep can generate its factory/predicate/accessor bodies in
// lockstep (value.cpp). Heap kinds (the six ObjHeader-bearing types, stored
// inline) take a ValueTag enumerator; tail kinds (the cold shared_ptr types,
// boxed) take a ValueTailKind enumerator.
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
  X(native_error_namespace, is_native_error_namespace,                         \
    as_native_error_namespace, NativeErrorNamespaceValue,                      \
    NativeErrorNamespace)                                                      \
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
  X(regexp_pattern, is_regexp_pattern, as_regexp_pattern,                      \
    RuntimeRegexpPatternValue, RegexpPattern)                                  \
  X(regexp_match, is_regexp_match, as_regexp_match, RuntimeRegexpMatchValue,   \
    RegexpMatch)                                                               \
  X(foreign_handle, is_foreign_handle, as_foreign_handle,                      \
    RuntimeForeignHandle, ForeignHandle)                                       \
  X(ast_node, is_ast_node, as_ast_node, RuntimeAstNode, AstNode)               \
  X(time_zone, is_time_zone, as_time_zone, RuntimeTimeZoneValue, TimeZone)

#ifndef AMBER_VALUE_REPR_TAGGED
// ---- Variant representation (default, 24 bytes) ---------------------------
struct Value {
  using Payload = std::variant<
      std::monostate, bool, std::int64_t, double, SymbolValue, StringValue,
      ClassObjectValue, IntrusivePtr<ClosureValue>, IntrusivePtr<InstanceValue>,
      IntrusivePtr<ListValue>, IntrusivePtr<TupleValue>, IntrusivePtr<SetValue>,
      IntrusivePtr<MapValue>, NativeTypeValue, NativeFunctionValue,
      NativeErrorClassValue, std::shared_ptr<ErrorInstanceValue>,
      std::shared_ptr<NativeErrorNamespaceValue>, std::shared_ptr<BigIntValue>,
      std::shared_ptr<RuntimeTaskModule>, std::shared_ptr<RuntimeTaskHandle>,
      std::shared_ptr<RuntimeChannel>, std::shared_ptr<RuntimeMutex>,
      std::shared_ptr<RuntimeAtomic>, std::shared_ptr<RuntimeBarrier>,
      std::shared_ptr<RuntimeFlowModule>,
      std::shared_ptr<RuntimeThreadedCollection>,
      std::shared_ptr<RuntimeTextWriter>, std::shared_ptr<RuntimeLogger>,
      std::shared_ptr<RuntimeIoValue>, std::shared_ptr<RuntimeWatchCell>,
      std::shared_ptr<RuntimeWatchHandle>, std::shared_ptr<ResultValue>,
      std::shared_ptr<RuntimeArgParserValue>, std::shared_ptr<RuntimeTimeValue>,
      std::shared_ptr<RuntimeTimePeriodValue>,
      std::shared_ptr<RuntimeUuidValue>,
      std::shared_ptr<RuntimeRegexpPatternValue>,
      std::shared_ptr<RuntimeRegexpMatchValue>,
      std::shared_ptr<RuntimeForeignHandle>, std::shared_ptr<RuntimeAstNode>,
      std::shared_ptr<RuntimeTimeZoneValue>>;

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
  static Value
  native_error_namespace(std::shared_ptr<NativeErrorNamespaceValue> value);
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
  static Value regexp_pattern(std::shared_ptr<RuntimeRegexpPatternValue> value);
  static Value regexp_match(std::shared_ptr<RuntimeRegexpMatchValue> value);
  static Value foreign_handle(std::shared_ptr<RuntimeForeignHandle> value);
  static Value ast_node(std::shared_ptr<RuntimeAstNode> value);
  static Value time(std::shared_ptr<RuntimeTimeValue> value);
  static Value time_zone(std::shared_ptr<RuntimeTimeZoneValue> value);
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
  bool is_native_error_namespace() const;
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
  bool is_regexp_pattern() const;
  bool is_regexp_match() const;
  bool is_foreign_handle() const;
  bool is_ast_node() const;
  bool is_time() const;
  bool is_time_zone() const;
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
  std::shared_ptr<NativeErrorNamespaceValue> as_native_error_namespace() const;
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
  std::shared_ptr<RuntimeRegexpPatternValue> as_regexp_pattern() const;
  std::shared_ptr<RuntimeRegexpMatchValue> as_regexp_match() const;
  std::shared_ptr<RuntimeForeignHandle> as_foreign_handle() const;
  std::shared_ptr<RuntimeAstNode> as_ast_node() const;
  std::shared_ptr<RuntimeTimeValue> as_time() const;
  std::shared_ptr<RuntimeTimeZoneValue> as_time_zone() const;
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
// the cold tail kinds are boxed behind a refcounted ValueTailBox (one extra
// allocation + indirection per tail value). Copy/move/destroy manage the
// intrusive refcount of the heap kinds and the box refcount manually.
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
  NativeErrorNamespace,
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
  RegexpPattern,
  RegexpMatch,
  ForeignHandle,
  AstNode,
  TimeZone,
};

struct ValueTailBox; // refcounted tail box; defined in value.cpp

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
  static Value
  native_error_namespace(std::shared_ptr<NativeErrorNamespaceValue> value);
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
  static Value regexp_pattern(std::shared_ptr<RuntimeRegexpPatternValue> value);
  static Value regexp_match(std::shared_ptr<RuntimeRegexpMatchValue> value);
  static Value foreign_handle(std::shared_ptr<RuntimeForeignHandle> value);
  static Value ast_node(std::shared_ptr<RuntimeAstNode> value);
  static Value time(std::shared_ptr<RuntimeTimeValue> value);
  static Value time_zone(std::shared_ptr<RuntimeTimeZoneValue> value);
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
  bool is_native_error_namespace() const;
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
  bool is_regexp_pattern() const;
  bool is_regexp_match() const;
  bool is_foreign_handle() const;
  bool is_ast_node() const;
  bool is_time() const;
  bool is_time_zone() const;
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
  std::shared_ptr<NativeErrorNamespaceValue> as_native_error_namespace() const;
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
  std::shared_ptr<RuntimeRegexpPatternValue> as_regexp_pattern() const;
  std::shared_ptr<RuntimeRegexpMatchValue> as_regexp_match() const;
  std::shared_ptr<RuntimeForeignHandle> as_foreign_handle() const;
  std::shared_ptr<RuntimeAstNode> as_ast_node() const;
  std::shared_ptr<RuntimeTimeValue> as_time() const;
  std::shared_ptr<RuntimeTimeZoneValue> as_time_zone() const;
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
// holds either the Ok value or the Err value, discriminated by `is_ok`.
struct ResultValue {
  bool is_ok = false;
  Value payload = Value::null();
};

struct ErrorInstanceValue {
  std::uint16_t error_id = 0;
  std::string message;
  std::vector<std::pair<std::string, Value>> fields;
};

// Wrap a payload into an Ok (is_ok=true) or Err (is_ok=false) Result value.
Value make_result_value(bool is_ok, Value payload);

} // namespace amber::runtime
