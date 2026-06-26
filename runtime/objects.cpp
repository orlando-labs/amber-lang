#include "runtime/objects.h"

#include <utility>

namespace amber::runtime {

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

} // namespace amber::runtime
