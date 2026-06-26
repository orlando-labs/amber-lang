#include "runtime/heap.h"

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
