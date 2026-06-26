#include "runtime/value.h"
#include "runtime/objects.h"

#include <atomic>
#include <utility>
#include <variant>

namespace amber::runtime {

std::string big_int_to_decimal_string(const BigIntValue &value) {
  if (value.magnitude.empty()) {
    return "0";
  }
  // Work in base-2^32 half-limbs so the long division by 10^9 only needs
  // 64-bit accumulators (keeps -Wpedantic builds free of __int128).
  std::vector<std::uint32_t> mag;
  mag.reserve(value.magnitude.size() * 2U);
  for (std::uint64_t limb : value.magnitude) {
    mag.push_back(static_cast<std::uint32_t>(limb & 0xFFFFFFFFULL));
    mag.push_back(static_cast<std::uint32_t>(limb >> 32U));
  }
  while (!mag.empty() && mag.back() == 0U) {
    mag.pop_back();
  }
  constexpr std::uint64_t kChunk = 1000000000ULL;
  std::vector<std::uint32_t> chunks;
  while (!mag.empty()) {
    std::uint64_t remainder = 0;
    for (std::size_t i = mag.size(); i-- > 0;) {
      const std::uint64_t acc = (remainder << 32U) | mag[i];
      mag[i] = static_cast<std::uint32_t>(acc / kChunk);
      remainder = acc % kChunk;
    }
    while (!mag.empty() && mag.back() == 0U) {
      mag.pop_back();
    }
    chunks.push_back(static_cast<std::uint32_t>(remainder));
  }
  std::string out = value.negative ? "-" : "";
  out += std::to_string(chunks.back());
  for (std::size_t i = chunks.size() - 1; i-- > 0;) {
    const std::string part = std::to_string(chunks[i]);
    out += std::string(9U - part.size(), '0');
    out += part;
  }
  return out;
}

#ifndef AMBER_VALUE_REPR_TAGGED
// ==== Variant Value method bodies (default 24-byte representation) =========
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

Value Value::closure(IntrusivePtr<ClosureValue> value) {
  return {std::move(value)};
}

Value Value::instance(IntrusivePtr<InstanceValue> value) {
  return {std::move(value)};
}

Value Value::native_type(RuntimeNativeTypeKind kind) {
  return {NativeTypeValue{kind}};
}

Value Value::native_function(RuntimeNativeFunctionKind kind) {
  return {NativeFunctionValue{kind}};
}

Value Value::native_error_class(std::uint16_t error_id) {
  return {NativeErrorClassValue{error_id}};
}

Value Value::error_instance(std::shared_ptr<ErrorInstanceValue> value) {
  return {std::move(value)};
}

Value Value::big_int(std::shared_ptr<BigIntValue> value) {
  return {std::move(value)};
}

Value Value::task_module(std::shared_ptr<RuntimeTaskModule> value) {
  return {std::move(value)};
}

Value Value::task_handle(std::shared_ptr<RuntimeTaskHandle> value) {
  return {std::move(value)};
}

Value Value::channel(std::shared_ptr<RuntimeChannel> value) {
  return {std::move(value)};
}

Value Value::mutex(std::shared_ptr<RuntimeMutex> value) {
  return {std::move(value)};
}

Value Value::atomic(std::shared_ptr<RuntimeAtomic> value) {
  return {std::move(value)};
}

Value Value::barrier(std::shared_ptr<RuntimeBarrier> value) {
  return {std::move(value)};
}

Value Value::flow_module(std::shared_ptr<RuntimeFlowModule> value) {
  return {std::move(value)};
}

Value Value::io_value(std::shared_ptr<RuntimeIoValue> value) {
  return {std::move(value)};
}

Value Value::threaded_collection(
    std::shared_ptr<RuntimeThreadedCollection> value) {
  return {std::move(value)};
}

Value Value::text_writer(std::shared_ptr<RuntimeTextWriter> value) {
  return {std::move(value)};
}

Value Value::logger(std::shared_ptr<RuntimeLogger> value) {
  return {std::move(value)};
}

Value Value::watch_cell(std::shared_ptr<RuntimeWatchCell> value) {
  return {std::move(value)};
}

Value Value::watch_handle(std::shared_ptr<RuntimeWatchHandle> value) {
  return {std::move(value)};
}

Value Value::result(std::shared_ptr<ResultValue> value) {
  return {std::move(value)};
}

Value Value::arg_parser(std::shared_ptr<RuntimeArgParserValue> value) {
  return {std::move(value)};
}

Value Value::uuid(std::shared_ptr<RuntimeUuidValue> value) {
  return {std::move(value)};
}

Value Value::foreign_handle(std::shared_ptr<RuntimeForeignHandle> value) {
  return {std::move(value)};
}

Value Value::time(std::shared_ptr<RuntimeTimeValue> value) {
  return {std::move(value)};
}

Value Value::time_period(std::shared_ptr<RuntimeTimePeriodValue> value) {
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
  return std::holds_alternative<IntrusivePtr<ClosureValue>>(payload);
}

bool Value::is_instance_object() const {
  return std::holds_alternative<IntrusivePtr<InstanceValue>>(payload);
}

bool Value::is_list() const {
  return std::holds_alternative<IntrusivePtr<ListValue>>(payload);
}

bool Value::is_tuple() const {
  return std::holds_alternative<IntrusivePtr<TupleValue>>(payload);
}

bool Value::is_set() const {
  return std::holds_alternative<IntrusivePtr<SetValue>>(payload);
}

bool Value::is_map() const {
  return std::holds_alternative<IntrusivePtr<MapValue>>(payload);
}

bool Value::is_native_type() const {
  return std::holds_alternative<NativeTypeValue>(payload);
}

bool Value::is_native_function() const {
  return std::holds_alternative<NativeFunctionValue>(payload);
}

bool Value::is_native_error_class() const {
  return std::holds_alternative<NativeErrorClassValue>(payload);
}

bool Value::is_error_instance() const {
  return std::holds_alternative<std::shared_ptr<ErrorInstanceValue>>(payload);
}

bool Value::is_big_int() const {
  return std::holds_alternative<std::shared_ptr<BigIntValue>>(payload);
}

bool Value::is_task_module() const {
  return std::holds_alternative<std::shared_ptr<RuntimeTaskModule>>(payload);
}

bool Value::is_task_handle() const {
  return std::holds_alternative<std::shared_ptr<RuntimeTaskHandle>>(payload);
}

bool Value::is_channel() const {
  return std::holds_alternative<std::shared_ptr<RuntimeChannel>>(payload);
}

bool Value::is_mutex() const {
  return std::holds_alternative<std::shared_ptr<RuntimeMutex>>(payload);
}

bool Value::is_atomic() const {
  return std::holds_alternative<std::shared_ptr<RuntimeAtomic>>(payload);
}

bool Value::is_barrier() const {
  return std::holds_alternative<std::shared_ptr<RuntimeBarrier>>(payload);
}

bool Value::is_flow_module() const {
  return std::holds_alternative<std::shared_ptr<RuntimeFlowModule>>(payload);
}

bool Value::is_threaded_collection() const {
  return std::holds_alternative<std::shared_ptr<RuntimeThreadedCollection>>(
      payload);
}

bool Value::is_text_writer() const {
  return std::holds_alternative<std::shared_ptr<RuntimeTextWriter>>(payload);
}

bool Value::is_logger() const {
  return std::holds_alternative<std::shared_ptr<RuntimeLogger>>(payload);
}

bool Value::is_io_value() const {
  return std::holds_alternative<std::shared_ptr<RuntimeIoValue>>(payload);
}

bool Value::is_watch_cell() const {
  return std::holds_alternative<std::shared_ptr<RuntimeWatchCell>>(payload);
}

bool Value::is_watch_handle() const {
  return std::holds_alternative<std::shared_ptr<RuntimeWatchHandle>>(payload);
}

bool Value::is_result() const {
  return std::holds_alternative<std::shared_ptr<ResultValue>>(payload);
}

bool Value::is_arg_parser() const {
  return std::holds_alternative<std::shared_ptr<RuntimeArgParserValue>>(
      payload);
}

bool Value::is_uuid() const {
  return std::holds_alternative<std::shared_ptr<RuntimeUuidValue>>(payload);
}

bool Value::is_foreign_handle() const {
  return std::holds_alternative<std::shared_ptr<RuntimeForeignHandle>>(payload);
}

bool Value::is_time() const {
  return std::holds_alternative<std::shared_ptr<RuntimeTimeValue>>(payload);
}

bool Value::is_time_period() const {
  return std::holds_alternative<std::shared_ptr<RuntimeTimePeriodValue>>(
      payload);
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

IntrusivePtr<ClosureValue> Value::as_closure() const {
  return std::get<IntrusivePtr<ClosureValue>>(payload);
}

IntrusivePtr<InstanceValue> Value::as_instance_object() const {
  return std::get<IntrusivePtr<InstanceValue>>(payload);
}

IntrusivePtr<ListValue> Value::as_list() const {
  return std::get<IntrusivePtr<ListValue>>(payload);
}

IntrusivePtr<TupleValue> Value::as_tuple() const {
  return std::get<IntrusivePtr<TupleValue>>(payload);
}

IntrusivePtr<SetValue> Value::as_set() const {
  return std::get<IntrusivePtr<SetValue>>(payload);
}

IntrusivePtr<MapValue> Value::as_map() const {
  return std::get<IntrusivePtr<MapValue>>(payload);
}

NativeTypeValue Value::as_native_type() const {
  return std::get<NativeTypeValue>(payload);
}

NativeFunctionValue Value::as_native_function() const {
  return std::get<NativeFunctionValue>(payload);
}

NativeErrorClassValue Value::as_native_error_class() const {
  return std::get<NativeErrorClassValue>(payload);
}

std::shared_ptr<ErrorInstanceValue> Value::as_error_instance() const {
  return std::get<std::shared_ptr<ErrorInstanceValue>>(payload);
}

std::shared_ptr<BigIntValue> Value::as_big_int() const {
  return std::get<std::shared_ptr<BigIntValue>>(payload);
}

std::shared_ptr<RuntimeTaskModule> Value::as_task_module() const {
  return std::get<std::shared_ptr<RuntimeTaskModule>>(payload);
}

std::shared_ptr<RuntimeTaskHandle> Value::as_task_handle() const {
  return std::get<std::shared_ptr<RuntimeTaskHandle>>(payload);
}

std::shared_ptr<RuntimeChannel> Value::as_channel() const {
  return std::get<std::shared_ptr<RuntimeChannel>>(payload);
}

std::shared_ptr<RuntimeMutex> Value::as_mutex() const {
  return std::get<std::shared_ptr<RuntimeMutex>>(payload);
}

std::shared_ptr<RuntimeAtomic> Value::as_atomic() const {
  return std::get<std::shared_ptr<RuntimeAtomic>>(payload);
}

std::shared_ptr<RuntimeBarrier> Value::as_barrier() const {
  return std::get<std::shared_ptr<RuntimeBarrier>>(payload);
}

std::shared_ptr<RuntimeFlowModule> Value::as_flow_module() const {
  return std::get<std::shared_ptr<RuntimeFlowModule>>(payload);
}

std::shared_ptr<RuntimeThreadedCollection>
Value::as_threaded_collection() const {
  return std::get<std::shared_ptr<RuntimeThreadedCollection>>(payload);
}

std::shared_ptr<RuntimeTextWriter> Value::as_text_writer() const {
  return std::get<std::shared_ptr<RuntimeTextWriter>>(payload);
}

std::shared_ptr<RuntimeLogger> Value::as_logger() const {
  return std::get<std::shared_ptr<RuntimeLogger>>(payload);
}

std::shared_ptr<RuntimeIoValue> Value::as_io_value() const {
  return std::get<std::shared_ptr<RuntimeIoValue>>(payload);
}

std::shared_ptr<RuntimeWatchCell> Value::as_watch_cell() const {
  return std::get<std::shared_ptr<RuntimeWatchCell>>(payload);
}

std::shared_ptr<RuntimeWatchHandle> Value::as_watch_handle() const {
  return std::get<std::shared_ptr<RuntimeWatchHandle>>(payload);
}

std::shared_ptr<ResultValue> Value::as_result() const {
  return std::get<std::shared_ptr<ResultValue>>(payload);
}

std::shared_ptr<RuntimeArgParserValue> Value::as_arg_parser() const {
  return std::get<std::shared_ptr<RuntimeArgParserValue>>(payload);
}

std::shared_ptr<RuntimeUuidValue> Value::as_uuid() const {
  return std::get<std::shared_ptr<RuntimeUuidValue>>(payload);
}

std::shared_ptr<RuntimeForeignHandle> Value::as_foreign_handle() const {
  return std::get<std::shared_ptr<RuntimeForeignHandle>>(payload);
}

std::shared_ptr<RuntimeTimeValue> Value::as_time() const {
  return std::get<std::shared_ptr<RuntimeTimeValue>>(payload);
}

std::shared_ptr<RuntimeTimePeriodValue> Value::as_time_period() const {
  return std::get<std::shared_ptr<RuntimeTimePeriodValue>>(payload);
}

Value Value::list(IntrusivePtr<ListValue> value) { return {std::move(value)}; }
Value Value::tuple(IntrusivePtr<TupleValue> value) {
  return {std::move(value)};
}
Value Value::set(IntrusivePtr<SetValue> value) { return {std::move(value)}; }
Value Value::map(IntrusivePtr<MapValue> value) { return {std::move(value)}; }

std::uint32_t Value::kind_index() const {
  return static_cast<std::uint32_t>(payload.index());
}

const std::int64_t *Value::integer_if() const {
  return std::get_if<std::int64_t>(&payload);
}

#else // AMBER_VALUE_REPR_TAGGED
// ==== Tagged Value method bodies (PLAN Phase 4 prototype, 16-byte rep) =====

// Refcounted box for the cold tail kinds. One heap allocation per tail value;
// the concrete shared_ptr is type-erased through shared_ptr<void> so its
// original typed deleter still runs on drop, and static_pointer_cast recovers
// the typed handle in the accessors.
struct ValueTailBox {
  std::atomic<std::uint32_t> refcount{1};
  ValueTailKind kind;
  std::shared_ptr<void> ptr;
  ValueTailBox(ValueTailKind k, std::shared_ptr<void> p)
      : kind(k), ptr(std::move(p)) {}
};

namespace {
// Drop one reference to an ObjHeader-bearing heap object. The concrete type is
// recovered from header.kind so the right destructor runs; this reuses the
// typed runtime_heap_release, keeping all RuntimeHeap bookkeeping intact.
void release_tagged_heap_object(ObjHeader *header) noexcept {
  switch (header->kind) {
  case HeapObjectKind::Closure:
    runtime_heap_release(reinterpret_cast<ClosureValue *>(header));
    break;
  case HeapObjectKind::Instance:
    runtime_heap_release(reinterpret_cast<InstanceValue *>(header));
    break;
  case HeapObjectKind::List:
    runtime_heap_release(reinterpret_cast<ListValue *>(header));
    break;
  case HeapObjectKind::Tuple:
    runtime_heap_release(reinterpret_cast<TupleValue *>(header));
    break;
  case HeapObjectKind::Set:
    runtime_heap_release(reinterpret_cast<SetValue *>(header));
    break;
  case HeapObjectKind::Map:
    runtime_heap_release(reinterpret_cast<MapValue *>(header));
    break;
  }
}
} // namespace

void Value::retain_payload(ValueTag tag, const Storage &storage) noexcept {
  if (tag >= ValueTag::Closure && tag <= ValueTag::Map) {
    if (storage.obj != nullptr) {
      storage.obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    }
  } else if (tag == ValueTag::Tail) {
    if (storage.tail != nullptr) {
      storage.tail->refcount.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Value::release_payload(ValueTag tag, Storage &storage) noexcept {
  if (tag >= ValueTag::Closure && tag <= ValueTag::Map) {
    if (storage.obj != nullptr) {
      release_tagged_heap_object(storage.obj);
    }
  } else if (tag == ValueTag::Tail) {
    if (storage.tail != nullptr &&
        storage.tail->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete storage.tail;
    }
  }
}

Value::Value(const Value &other) noexcept : tag_(other.tag_), u_(other.u_) {
  retain_payload(tag_, u_);
}

Value::Value(Value &&other) noexcept : tag_(other.tag_), u_(other.u_) {
  other.tag_ = ValueTag::Null;
  other.u_.i = 0;
}

Value &Value::operator=(const Value &other) noexcept {
  if (this != &other) {
    // Retain the source payload before releasing ours so aliasing (two Values
    // pointing at the same object) is safe.
    ValueTag incoming_tag = other.tag_;
    Storage incoming = other.u_;
    retain_payload(incoming_tag, incoming);
    release_payload(tag_, u_);
    tag_ = incoming_tag;
    u_ = incoming;
  }
  return *this;
}

Value &Value::operator=(Value &&other) noexcept {
  if (this != &other) {
    release_payload(tag_, u_);
    tag_ = other.tag_;
    u_ = other.u_;
    other.tag_ = ValueTag::Null;
    other.u_.i = 0;
  }
  return *this;
}

Value::~Value() { release_payload(tag_, u_); }

Value Value::make_tail(ValueTailKind kind, std::shared_ptr<void> ptr) {
  Value v;
  v.tag_ = ValueTag::Tail;
  v.u_.tail = new ValueTailBox(kind, std::move(ptr));
  return v;
}

template <class T> Value Value::make_heap(ValueTag tag, IntrusivePtr<T> value) {
  Value v;
  v.tag_ = tag;
  T *p = value.release(); // adopt the +1 reference, no atomic
  v.u_.obj = (p != nullptr) ? &p->header : nullptr;
  return v;
}

Value Value::null() { return Value(); }

Value Value::boolean(bool value) {
  Value v;
  v.tag_ = ValueTag::Bool;
  v.u_.b = value;
  return v;
}

Value Value::integer(std::int64_t value) {
  Value v;
  v.tag_ = ValueTag::Int;
  v.u_.i = value;
  return v;
}

Value Value::floating(double value) {
  Value v;
  v.tag_ = ValueTag::Float;
  v.u_.d = value;
  return v;
}

Value Value::symbol(std::uint32_t symbol_id) {
  Value v;
  v.tag_ = ValueTag::Symbol;
  v.u_.u32 = symbol_id;
  return v;
}

Value Value::string(std::uint32_t string_id) {
  Value v;
  v.tag_ = ValueTag::String;
  v.u_.u32 = string_id;
  return v;
}

Value Value::class_object(std::uint32_t class_index) {
  Value v;
  v.tag_ = ValueTag::ClassObject;
  v.u_.u32 = class_index;
  return v;
}

Value Value::native_type(RuntimeNativeTypeKind kind) {
  Value v;
  v.tag_ = ValueTag::NativeType;
  v.u_.ntype = kind;
  return v;
}

Value Value::native_function(RuntimeNativeFunctionKind kind) {
  Value v;
  v.tag_ = ValueTag::NativeFunction;
  v.u_.nfn = kind;
  return v;
}

Value Value::native_error_class(std::uint16_t error_id) {
  Value v;
  v.tag_ = ValueTag::NativeErrorClass;
  v.u_.u16 = error_id;
  return v;
}

bool Value::is_null() const { return tag_ == ValueTag::Null; }
bool Value::is_bool() const { return tag_ == ValueTag::Bool; }
bool Value::is_integer() const { return tag_ == ValueTag::Int; }
bool Value::is_float() const { return tag_ == ValueTag::Float; }
bool Value::is_symbol() const { return tag_ == ValueTag::Symbol; }
bool Value::is_string() const { return tag_ == ValueTag::String; }
bool Value::is_class_object() const { return tag_ == ValueTag::ClassObject; }
bool Value::is_native_type() const { return tag_ == ValueTag::NativeType; }
bool Value::is_native_function() const {
  return tag_ == ValueTag::NativeFunction;
}
bool Value::is_native_error_class() const {
  return tag_ == ValueTag::NativeErrorClass;
}

bool Value::as_bool() const { return u_.b; }
std::int64_t Value::as_integer() const { return u_.i; }
double Value::as_float() const { return u_.d; }
SymbolValue Value::as_symbol() const { return SymbolValue{u_.u32}; }
StringValue Value::as_string() const { return StringValue{u_.u32}; }
ClassObjectValue Value::as_class_object() const {
  return ClassObjectValue{u_.u32};
}
NativeTypeValue Value::as_native_type() const {
  return NativeTypeValue{u_.ntype};
}
NativeFunctionValue Value::as_native_function() const {
  return NativeFunctionValue{u_.nfn};
}
NativeErrorClassValue Value::as_native_error_class() const {
  return NativeErrorClassValue{u_.u16};
}

// Heap kinds: stored inline as the embedded ObjHeader*; the tag names the
// concrete type. Factory adopts the IntrusivePtr's reference; accessor hands
// back a fresh owning IntrusivePtr (one incref, matching the variant get-copy).
#define X(name, is_fn, as_fn, Type, Tag)                                       \
  Value Value::name(IntrusivePtr<Type> value) {                                \
    return make_heap(ValueTag::Tag, std::move(value));                         \
  }                                                                            \
  bool Value::is_fn() const { return tag_ == ValueTag::Tag; }                  \
  IntrusivePtr<Type> Value::as_fn() const {                                    \
    if (u_.obj == nullptr) {                                                   \
      return {};                                                               \
    }                                                                          \
    Type *p = reinterpret_cast<Type *>(u_.obj);                                \
    runtime_heap_add_ref(p);                                                   \
    return IntrusivePtr<Type>(p, typename IntrusivePtr<Type>::Adopt{});        \
  }
AMBER_VALUE_HEAP_KINDS(X)
#undef X

// Tail kinds: boxed behind a refcounted ValueTailBox; the box's ValueTailKind
// names the concrete shared_ptr type.
#define X(name, is_fn, as_fn, Type, Tag)                                       \
  Value Value::name(std::shared_ptr<Type> value) {                             \
    return make_tail(ValueTailKind::Tag, std::move(value));                    \
  }                                                                            \
  bool Value::is_fn() const {                                                  \
    return tag_ == ValueTag::Tail && u_.tail != nullptr &&                     \
           u_.tail->kind == ValueTailKind::Tag;                                \
  }                                                                            \
  std::shared_ptr<Type> Value::as_fn() const {                                 \
    return std::static_pointer_cast<Type>(u_.tail->ptr);                       \
  }
AMBER_VALUE_TAIL_KINDS(X)
#undef X

std::uint32_t Value::kind_index() const {
  if (tag_ == ValueTag::Tail && u_.tail != nullptr) {
    return 0x100u + static_cast<std::uint32_t>(u_.tail->kind);
  }
  return static_cast<std::uint32_t>(tag_);
}

const std::int64_t *Value::integer_if() const {
  return tag_ == ValueTag::Int ? &u_.i : nullptr;
}

#endif // AMBER_VALUE_REPR_TAGGED

} // namespace amber::runtime
