#include "runtime/objects.h"

#include <utility>

namespace amber::runtime {

MapEntry::MapEntry(std::uint32_t key_symbol_id, Value entry_value)
    : symbol_id(key_symbol_id), key(Value::symbol(key_symbol_id)),
      value(std::move(entry_value)) {}

MapEntry::MapEntry(Value entry_key, Value entry_value)
    : symbol_id(entry_key.is_symbol() ? entry_key.as_symbol().symbol_id : 0),
      key(std::move(entry_key)), value(std::move(entry_value)) {}

} // namespace amber::runtime
