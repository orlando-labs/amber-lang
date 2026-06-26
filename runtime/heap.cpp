#include "runtime/heap.h"

#include <utility>

namespace amber::runtime {

RuntimeHeap &default_runtime_heap() {
  static RuntimeHeap heap;
  return heap;
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
