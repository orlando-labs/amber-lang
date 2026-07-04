#include "runtime/objects.h"

#include "runtime/heap.h"
#include "runtime/watch.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace amber::runtime {

namespace {

bool values_are_numeric(const Value &lhs, const Value &rhs) {
  return (lhs.is_integer() || lhs.is_float()) &&
         (rhs.is_integer() || rhs.is_float());
}

double numeric_value_as_double(const Value &value) {
  return value.is_integer() ? static_cast<double>(value.as_integer())
                            : value.as_float();
}

Value unwrap_watch_value(const Value &value) {
  if (!value.is_watch_cell()) {
    return value;
  }
  const std::shared_ptr<RuntimeWatchCell> cell = value.as_watch_cell();
  return cell == nullptr ? value : cell->read();
}

CollectionKeyError collection_key_type_error(const std::string &message) {
  return CollectionKeyError{"TypeError", message};
}

std::optional<Value>
normalize_collection_key_impl(const Value &value, const char *context,
                              std::unordered_set<std::uint64_t> *active,
                              CollectionKeyError *error) {
  if (value.is_float() && std::isnan(value.as_float())) {
    *error = collection_key_type_error(std::string("NaN cannot be used as ") +
                                       context);
    return std::nullopt;
  }

  if (value.is_map() || value.is_set() || value.is_closure()) {
    *error = collection_key_type_error(std::string(context) +
                                       " does not support deep object keys");
    return std::nullopt;
  }

  if (value.is_list() || value.is_tuple()) {
    const ObjHeader *header = heap_header_from_value(value);
    if (header == nullptr) {
      *error = collection_key_type_error(std::string(context) +
                                         " composite key is null");
      return std::nullopt;
    }
    const std::optional<std::string> lifecycle_error =
        lifecycle_access_error_name(*header);
    if (lifecycle_error.has_value()) {
      *error = CollectionKeyError{
          *lifecycle_error, lifecycle_access_error_message(*lifecycle_error)};
      return std::nullopt;
    }
    if (header->allocation_id != 0 &&
        !active->insert(header->allocation_id).second) {
      *error = collection_key_type_error(std::string("cyclic composite ") +
                                         context + " is not supported");
      return std::nullopt;
    }

    std::vector<Value> items;
    if (value.is_list()) {
      const IntrusivePtr<ListValue> list = value.as_list();
      if (list == nullptr) {
        *error = collection_key_type_error(std::string(context) +
                                           " list key is null");
        return std::nullopt;
      }
      items.reserve(list->items.size());
      for (const Value &item : list->items) {
        std::optional<Value> normalized =
            normalize_collection_key_impl(item, context, active, error);
        if (!normalized.has_value()) {
          return std::nullopt;
        }
        items.push_back(*normalized);
      }
    } else {
      const IntrusivePtr<TupleValue> tuple = value.as_tuple();
      if (tuple == nullptr) {
        *error = collection_key_type_error(std::string(context) +
                                           " tuple key is null");
        return std::nullopt;
      }
      items.reserve(tuple->items.size());
      for (const Value &item : tuple->items) {
        std::optional<Value> normalized =
            normalize_collection_key_impl(item, context, active, error);
        if (!normalized.has_value()) {
          return std::nullopt;
        }
        items.push_back(*normalized);
      }
    }
    if (header->allocation_id != 0) {
      active->erase(header->allocation_id);
    }
    return make_tuple_value(std::move(items));
  }

  return value;
}

} // namespace

MapEntry::MapEntry(std::uint32_t key_symbol_id, Value entry_value)
    : symbol_id(key_symbol_id), key(Value::symbol(key_symbol_id)),
      value(std::move(entry_value)) {}

MapEntry::MapEntry(Value entry_key, Value entry_value)
    : symbol_id(entry_key.is_symbol() ? entry_key.as_symbol().symbol_id : 0),
      key(std::move(entry_key)), value(std::move(entry_value)) {}

bool value_has_heap_payload_tag(const Value &value) {
  return value.is_closure() || value.is_instance_object() || value.is_list() ||
         value.is_tuple() || value.is_set() || value.is_map();
}

const ObjHeader *heap_header_from_value(const Value &value) {
  if (value.is_closure()) {
    const IntrusivePtr<ClosureValue> object = value.as_closure();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_instance_object()) {
    const IntrusivePtr<InstanceValue> object = value.as_instance_object();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_list()) {
    const IntrusivePtr<ListValue> object = value.as_list();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_tuple()) {
    const IntrusivePtr<TupleValue> object = value.as_tuple();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_set()) {
    const IntrusivePtr<SetValue> object = value.as_set();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_map()) {
    const IntrusivePtr<MapValue> object = value.as_map();
    return object == nullptr ? nullptr : &object->header;
  }
  return nullptr;
}

ObjHeader *mutable_heap_header_from_value(const Value &value) {
  if (value.is_closure()) {
    const IntrusivePtr<ClosureValue> object = value.as_closure();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_instance_object()) {
    const IntrusivePtr<InstanceValue> object = value.as_instance_object();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_list()) {
    const IntrusivePtr<ListValue> object = value.as_list();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_tuple()) {
    const IntrusivePtr<TupleValue> object = value.as_tuple();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_set()) {
    const IntrusivePtr<SetValue> object = value.as_set();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_map()) {
    const IntrusivePtr<MapValue> object = value.as_map();
    return object == nullptr ? nullptr : &object->header;
  }
  return nullptr;
}

bool header_is_deallocated(const ObjHeader &header) {
  return header.lifetime_state == ObjectLifetimeState::Deallocated ||
         (header.flags & kObjectFlagDead) != 0U ||
         (header.shape != nullptr && header.shape->dead);
}

bool header_is_destroyed(const ObjHeader &header) {
  return header.lifetime_state == ObjectLifetimeState::Destroyed ||
         header.lifetime_state == ObjectLifetimeState::Destroying ||
         (header.flags & kObjectFlagDestroyed) != 0U ||
         (header.flags & kObjectFlagDestroying) != 0U;
}

std::optional<std::string>
lifecycle_access_error_name(const ObjHeader &header) {
  if (header_is_deallocated(header)) {
    return "UseAfterFreeError";
  }
  if (header_is_destroyed(header)) {
    return "DestroyedAccessError";
  }
  return std::nullopt;
}

std::string lifecycle_access_error_message(const std::string &error_name) {
  return error_name == "UseAfterFreeError" ? "access to deallocated object"
                                           : "access to destroyed object";
}

std::string lifecycle_debug_label(const ObjHeader &header) {
  if (header_is_deallocated(header)) {
    return "deallocated";
  }
  if (header_is_destroyed(header)) {
    return "destroyed";
  }
  return "";
}

bool instance_is_native_range(const IntrusivePtr<InstanceValue> &instance) {
  if (instance == nullptr ||
      instance->class_index != kNativeSyntheticClassIndex) {
    return false;
  }
  const auto marker = instance->ivars.find(kNativeRangeMarker);
  return marker != instance->ivars.end() && marker->second.is_bool() &&
         marker->second.as_bool();
}

bool value_equals(const Value &lhs, const Value &rhs) {
  if (lhs.is_watch_cell() || rhs.is_watch_cell()) {
    return value_equals(unwrap_watch_value(lhs), unwrap_watch_value(rhs));
  }
  if (values_are_numeric(lhs, rhs)) {
    if (lhs.is_integer() && rhs.is_integer()) {
      return lhs.as_integer() == rhs.as_integer();
    }
    return numeric_value_as_double(lhs) == numeric_value_as_double(rhs);
  }
  if (lhs.kind_index() != rhs.kind_index()) {
    return false;
  }
  if (lhs.is_null()) {
    return true;
  }
  if (lhs.is_bool()) {
    return lhs.as_bool() == rhs.as_bool();
  }
  if (lhs.is_integer()) {
    return lhs.as_integer() == rhs.as_integer();
  }
  if (lhs.is_float()) {
    return lhs.as_float() == rhs.as_float();
  }
  if (lhs.is_symbol()) {
    return lhs.as_symbol().symbol_id == rhs.as_symbol().symbol_id;
  }
  if (lhs.is_string()) {
    return lhs.as_string().string_id == rhs.as_string().string_id;
  }
  if (lhs.is_class_object()) {
    return lhs.as_class_object().class_index ==
           rhs.as_class_object().class_index;
  }
  if (lhs.is_native_type()) {
    return lhs.as_native_type().kind == rhs.as_native_type().kind;
  }
  if (lhs.is_native_function()) {
    return lhs.as_native_function().kind == rhs.as_native_function().kind;
  }
  if (lhs.is_native_error_class()) {
    return lhs.as_native_error_class().error_id ==
           rhs.as_native_error_class().error_id;
  }
  if (lhs.is_error_instance()) {
    const std::shared_ptr<ErrorInstanceValue> left = lhs.as_error_instance();
    const std::shared_ptr<ErrorInstanceValue> right = rhs.as_error_instance();
    if (left == right) {
      return true;
    }
    return left != nullptr && right != nullptr &&
           left->error_id == right->error_id && left->message == right->message;
  }
  if (lhs.is_big_int()) {
    const std::shared_ptr<BigIntValue> left = lhs.as_big_int();
    const std::shared_ptr<BigIntValue> right = rhs.as_big_int();
    if (left == right) {
      return true;
    }
    return left != nullptr && right != nullptr &&
           left->negative == right->negative &&
           left->magnitude == right->magnitude;
  }
  if (lhs.is_uuid()) {
    const std::shared_ptr<RuntimeUuidValue> left = lhs.as_uuid();
    const std::shared_ptr<RuntimeUuidValue> right = rhs.as_uuid();
    if (left == right) {
      return true;
    }
    return left != nullptr && right != nullptr && left->bytes == right->bytes;
  }
  if (lhs.is_foreign_handle()) {
    return lhs.as_foreign_handle() == rhs.as_foreign_handle();
  }
  if (lhs.is_ast_node()) {
    return lhs.as_ast_node() == rhs.as_ast_node();
  }
  if (lhs.is_time()) {
    const std::shared_ptr<RuntimeTimeValue> left = lhs.as_time();
    const std::shared_ptr<RuntimeTimeValue> right = rhs.as_time();
    if (left == right) {
      return true;
    }
    return left != nullptr && right != nullptr &&
           left->epoch_seconds == right->epoch_seconds &&
           left->nanosecond == right->nanosecond;
  }
  if (lhs.is_time_zone()) {
    const std::shared_ptr<RuntimeTimeZoneValue> left = lhs.as_time_zone();
    const std::shared_ptr<RuntimeTimeZoneValue> right = rhs.as_time_zone();
    if (left == right) {
      return true;
    }
    return left != nullptr && right != nullptr &&
           left->offset_seconds == right->offset_seconds &&
           left->name == right->name &&
           left->fixed_offset == right->fixed_offset;
  }
  if (lhs.is_time_period()) {
    const std::shared_ptr<RuntimeTimePeriodValue> left = lhs.as_time_period();
    const std::shared_ptr<RuntimeTimePeriodValue> right = rhs.as_time_period();
    if (left == right) {
      return true;
    }
    return left != nullptr && right != nullptr &&
           left->months == right->months && left->days == right->days &&
           left->nanoseconds == right->nanoseconds;
  }
  if (lhs.is_result()) {
    const std::shared_ptr<ResultValue> left = lhs.as_result();
    const std::shared_ptr<ResultValue> right = rhs.as_result();
    if (left == right) {
      return true;
    }
    return left != nullptr && right != nullptr && left->is_ok == right->is_ok &&
           value_equals(left->payload, right->payload);
  }
  if (lhs.is_closure()) {
    return lhs.as_closure() == rhs.as_closure();
  }
  if (lhs.is_task_module()) {
    return lhs.as_task_module() == rhs.as_task_module();
  }
  if (lhs.is_task_handle()) {
    return lhs.as_task_handle() == rhs.as_task_handle();
  }
  if (lhs.is_channel()) {
    return lhs.as_channel() == rhs.as_channel();
  }
  if (lhs.is_mutex()) {
    return lhs.as_mutex() == rhs.as_mutex();
  }
  if (lhs.is_atomic()) {
    return lhs.as_atomic() == rhs.as_atomic();
  }
  if (lhs.is_barrier()) {
    return lhs.as_barrier() == rhs.as_barrier();
  }
  if (lhs.is_flow_module()) {
    return lhs.as_flow_module() == rhs.as_flow_module();
  }
  if (lhs.is_threaded_collection()) {
    return lhs.as_threaded_collection() == rhs.as_threaded_collection();
  }
  if (lhs.is_text_writer()) {
    return lhs.as_text_writer() == rhs.as_text_writer();
  }
  if (lhs.is_logger()) {
    return lhs.as_logger() == rhs.as_logger();
  }
  if (lhs.is_io_value()) {
    return lhs.as_io_value() == rhs.as_io_value();
  }
  if (lhs.is_watch_handle()) {
    return lhs.as_watch_handle() == rhs.as_watch_handle();
  }
  if (lhs.is_instance_object()) {
    const IntrusivePtr<InstanceValue> left = lhs.as_instance_object();
    const IntrusivePtr<InstanceValue> right = rhs.as_instance_object();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (instance_is_native_range(left) && instance_is_native_range(right)) {
      auto ivar_or_null = [](const IntrusivePtr<InstanceValue> &instance,
                             const std::string &name) {
        const auto found = instance->ivars.find(name);
        return found == instance->ivars.end() ? Value::null() : found->second;
      };
      return value_equals(ivar_or_null(left, "start"),
                          ivar_or_null(right, "start")) &&
             value_equals(ivar_or_null(left, "finish"),
                          ivar_or_null(right, "finish")) &&
             value_equals(ivar_or_null(left, "inclusive_end"),
                          ivar_or_null(right, "inclusive_end")) &&
             value_equals(ivar_or_null(left, "step"),
                          ivar_or_null(right, "step"));
    }
    return left == right;
  }
  if (lhs.is_list()) {
    const IntrusivePtr<ListValue> left = lhs.as_list();
    const IntrusivePtr<ListValue> right = rhs.as_list();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (left->items.size() != right->items.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left->items.size(); ++i) {
      if (!value_equals(left->items[i], right->items[i])) {
        return false;
      }
    }
    return true;
  }
  if (lhs.is_tuple()) {
    const IntrusivePtr<TupleValue> left = lhs.as_tuple();
    const IntrusivePtr<TupleValue> right = rhs.as_tuple();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (left->items.size() != right->items.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left->items.size(); ++i) {
      if (!value_equals(left->items[i], right->items[i])) {
        return false;
      }
    }
    return true;
  }
  if (lhs.is_set()) {
    const IntrusivePtr<SetValue> left = lhs.as_set();
    const IntrusivePtr<SetValue> right = rhs.as_set();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (left->items.size() != right->items.size()) {
      return false;
    }
    std::vector<bool> matched(right->items.size(), false);
    for (const Value &left_item : left->items) {
      bool found = false;
      for (std::size_t i = 0; i < right->items.size(); ++i) {
        if (!matched[i] && value_equals(left_item, right->items[i])) {
          matched[i] = true;
          found = true;
          break;
        }
      }
      if (!found) {
        return false;
      }
    }
    return true;
  }
  if (lhs.is_map()) {
    const IntrusivePtr<MapValue> left = lhs.as_map();
    const IntrusivePtr<MapValue> right = rhs.as_map();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (left->entries.size() != right->entries.size()) {
      return false;
    }
    std::vector<bool> matched(right->entries.size(), false);
    for (const MapEntry &entry : left->entries) {
      bool found = false;
      for (std::size_t i = 0; i < right->entries.size(); ++i) {
        if (!matched[i] &&
            collection_keys_equal(entry.key, right->entries[i].key)) {
          matched[i] = true;
          found = true;
          if (!value_equals(entry.value, right->entries[i].value)) {
            return false;
          }
          break;
        }
      }
      if (!found) {
        return false;
      }
    }
    return true;
  }
  return false;
}

std::optional<Value> normalize_map_key(const Value &key,
                                       CollectionKeyError *error) {
  std::unordered_set<std::uint64_t> active;
  return normalize_collection_key_impl(key, "Map key", &active, error);
}

std::optional<Value> normalize_set_element(const Value &value,
                                           CollectionKeyError *error) {
  std::unordered_set<std::uint64_t> active;
  return normalize_collection_key_impl(value, "Set element", &active, error);
}

bool collection_keys_equal(const Value &stored, const Value &lookup) {
  return value_equals(stored, lookup);
}

bool map_key_is_nameable(const Value &key) {
  return key.is_symbol() || key.is_string();
}

bool map_entry_key_equivalent(const MapEntry &entry, const Value &lookup_key,
                              std::optional<std::uint32_t> lookup_id,
                              bool strict) {
  if (!strict && lookup_id.has_value() && map_key_is_nameable(entry.key) &&
      entry.symbol_id == *lookup_id) {
    return true;
  }
  return value_equals(entry.key, lookup_key);
}

bool map_entries_same_key(const MapEntry &a, const MapEntry &b, bool strict) {
  if (!strict && map_key_is_nameable(a.key) && map_key_is_nameable(b.key)) {
    return a.symbol_id == b.symbol_id;
  }
  return value_equals(a.key, b.key);
}

void upsert_normalized_map_entry(std::vector<MapEntry> *entries, MapEntry entry,
                                 bool strict) {
  auto existing = std::find_if(
      entries->begin(), entries->end(), [&](const MapEntry &candidate) {
        return map_entries_same_key(candidate, entry, strict);
      });
  if (existing == entries->end()) {
    entries->push_back(std::move(entry));
    return;
  }
  // First occurrence's key representation wins; last assignment's value wins.
  existing->value = std::move(entry.value);
}

std::optional<std::vector<MapEntry>>
normalize_map_entries(std::vector<MapEntry> entries, bool strict,
                      CollectionKeyError *error) {
  std::vector<MapEntry> normalized;
  normalized.reserve(entries.size());
  for (MapEntry &entry : entries) {
    std::optional<Value> key = normalize_map_key(entry.key, error);
    if (!key.has_value()) {
      return std::nullopt;
    }
    // Preserve the canonical identity computed at construction; the
    // `(Value, Value)` constructor would recompute it as 0 for Str keys.
    MapEntry rebuilt;
    rebuilt.symbol_id = entry.symbol_id;
    rebuilt.key = std::move(*key);
    rebuilt.value = entry.value;
    upsert_normalized_map_entry(&normalized, std::move(rebuilt), strict);
  }
  return normalized;
}

} // namespace amber::runtime
