#include "runtime/heap.h"

#include <atomic>
#include <memory>
#include <utility>

namespace amber::runtime {

RuntimeHeap &default_runtime_heap() {
  static RuntimeHeap heap;
  return heap;
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
