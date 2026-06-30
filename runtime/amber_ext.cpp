// Implementation of the native-extension ABI (runtime/amber_ext.h) and its
// in-tree driver surface (runtime/amber_ext_runtime.h).
//
// Decision 1 (DESIGN-native-packages-5c-dispatch-2026-06-21): an `AmberValue`
// is an opaque handle into a per-call arena of runtime `Value`s owned by an
// `AmberCtx`. The ctx wraps the active VM frame (type-erased), the `StdlibHost`
// facade, and the foreign-handle `NativeTagRegistry`. Every amber_ext.h
// function is a thin shim over those: builders push a `Value`, readers read
// one, faults and block calls forward to the host. Extension calls therefore
// execute in the VM lane on runtime `Value`s; the emitted native lane reaches
// them through the existing per-function VM bridge, so `NativeValue` is never
// involved.

#include "runtime/amber_ext_runtime.h"

#include "runtime/io.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using amber::runtime::RuntimeByteBuffer;
using amber::runtime::RuntimeBytes;
using amber::runtime::RuntimeByteSlice;
using amber::runtime::Value;

// A Bytes-like value (Bytes / ByteSlice / ByteBuffer), checked without faulting
// so the `amber_is_bytes` predicate and `amber_bytes_view` reader can report a
// wrong kind silently (per the amber_ext.h reader contract).
bool value_is_bytes(const Value &value) {
  if (!value.is_io_value()) {
    return false;
  }
  const std::shared_ptr<amber::runtime::RuntimeIoValue> io =
      value.as_io_value();
  return std::dynamic_pointer_cast<RuntimeBytes>(io) != nullptr ||
         std::dynamic_pointer_cast<RuntimeByteSlice>(io) != nullptr ||
         std::dynamic_pointer_cast<RuntimeByteBuffer>(io) != nullptr;
}

} // namespace

// The opaque call context. Defined in the global namespace to match the
// `typedef struct AmberCtx AmberCtx;` the C header declares inside extern "C".
struct AmberCtx {
  amber::runtime::StdlibHost *host = nullptr;
  const void *frame = nullptr;
  amber::runtime::NativeTagRegistry *tags = nullptr;
  // The per-call value arena. `AmberValue` is a 1-based index encoded as a
  // pointer, so reallocation here never invalidates a handle.
  std::vector<Value> arena;
  // Stable backing for borrowed string views (`stdlib_text_of` returns a copy);
  // a deque keeps element addresses valid across pushes for the call's
  // duration.
  std::deque<std::string> str_keep;
  // Keepalives for borrowed bytes views, retained so the viewed storage
  // outlives the thunk.
  std::vector<Value> keepalives;

  AmberValue push(Value value) {
    arena.push_back(std::move(value));
    return reinterpret_cast<AmberValue>(
        static_cast<std::uintptr_t>(arena.size()));
  }

  const Value &resolve(AmberValue handle) const {
    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(handle);
    if (raw == 0 || raw > arena.size()) {
      static const Value kNull = Value::null();
      return kNull;
    }
    return arena[raw - 1];
  }
};

extern "C" {

// ---- value predicates ---------------------------------------------------

int amber_is_null(AmberCtx *cx, AmberValue value) {
  return cx->resolve(value).is_null() ? 1 : 0;
}
int amber_is_bool(AmberCtx *cx, AmberValue value) {
  return cx->resolve(value).is_bool() ? 1 : 0;
}
int amber_is_int(AmberCtx *cx, AmberValue value) {
  return cx->resolve(value).is_integer() ? 1 : 0;
}
int amber_is_float(AmberCtx *cx, AmberValue value) {
  return cx->resolve(value).is_float() ? 1 : 0;
}
int amber_is_str(AmberCtx *cx, AmberValue value) {
  return cx->resolve(value).is_string() ? 1 : 0;
}
int amber_is_bytes(AmberCtx *cx, AmberValue value) {
  return value_is_bytes(cx->resolve(value)) ? 1 : 0;
}
int amber_is_list(AmberCtx *cx, AmberValue value) {
  return cx->resolve(value).is_list() ? 1 : 0;
}
int amber_is_handle(AmberCtx *cx, AmberValue value) {
  return cx->resolve(value).is_foreign_handle() ? 1 : 0;
}

// ---- readers ------------------------------------------------------------

int amber_as_bool(AmberCtx *cx, AmberValue value, int *out) {
  const Value &v = cx->resolve(value);
  if (!v.is_bool()) {
    return 0;
  }
  *out = v.as_bool() ? 1 : 0;
  return 1;
}
int amber_as_int(AmberCtx *cx, AmberValue value, int64_t *out) {
  const Value &v = cx->resolve(value);
  if (!v.is_integer()) {
    return 0;
  }
  *out = v.as_integer();
  return 1;
}
int amber_as_float(AmberCtx *cx, AmberValue value, double *out) {
  const Value &v = cx->resolve(value);
  if (!v.is_float()) {
    return 0;
  }
  *out = v.as_float();
  return 1;
}

int amber_str_view(AmberCtx *cx, AmberValue value, const char **ptr,
                   size_t *len) {
  const Value &v = cx->resolve(value);
  if (!v.is_string()) {
    return 0;
  }
  const std::optional<std::string> text = cx->host->stdlib_text_of(v);
  if (!text.has_value()) {
    return 0;
  }
  cx->str_keep.push_back(*text);
  const std::string &stored = cx->str_keep.back();
  *ptr = stored.data();
  *len = stored.size();
  return 1;
}

int amber_bytes_view(AmberCtx *cx, AmberValue value, const uint8_t **ptr,
                     size_t *len) {
  const Value &v = cx->resolve(value);
  if (!value_is_bytes(v)) {
    return 0;
  }
  Value keepalive = Value::null();
  if (!cx->host->stdlib_bytes_view(cx->frame, v, ptr, len, &keepalive)) {
    return 0;
  }
  cx->keepalives.push_back(std::move(keepalive));
  return 1;
}

size_t amber_list_len(AmberCtx *cx, AmberValue value) {
  const Value &v = cx->resolve(value);
  if (!v.is_list()) {
    return 0;
  }
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list =
      v.as_list();
  return list == nullptr ? 0 : list->items.size();
}
AmberValue amber_list_at(AmberCtx *cx, AmberValue value, size_t index) {
  const Value &v = cx->resolve(value);
  if (!v.is_list()) {
    return cx->push(Value::null());
  }
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list =
      v.as_list();
  if (list == nullptr || index >= list->items.size()) {
    return cx->push(Value::null());
  }
  return cx->push(list->items[index]);
}

int amber_handle_ptr(AmberCtx *cx, AmberValue value, const char *tag,
                     void **out) {
  const Value &v = cx->resolve(value);
  if (!v.is_foreign_handle()) {
    cx->host->stdlib_raise_runtime_error(cx->frame, "TypeError",
                                         "expected a native handle");
    return 0;
  }
  const std::shared_ptr<amber::runtime::RuntimeForeignHandle> handle =
      v.as_foreign_handle();
  if (handle == nullptr) {
    cx->host->stdlib_raise_runtime_error(cx->frame, "TypeError",
                                         "native handle is null");
    return 0;
  }
  if (tag != nullptr && handle->tag != tag) {
    cx->host->stdlib_raise_runtime_error(cx->frame, "TypeError",
                                         "native handle tag mismatch");
    return 0;
  }
  if (!handle->live) {
    cx->host->stdlib_raise_runtime_error(cx->frame, "LifetimeError",
                                         "native handle used after destroy!");
    return 0;
  }
  *out = handle->ptr;
  return 1;
}

// ---- builders -----------------------------------------------------------

AmberValue amber_make_null(AmberCtx *cx) { return cx->push(Value::null()); }
AmberValue amber_make_bool(AmberCtx *cx, int value) {
  return cx->push(Value::boolean(value != 0));
}
AmberValue amber_make_int(AmberCtx *cx, int64_t value) {
  return cx->push(Value::integer(value));
}
AmberValue amber_make_float(AmberCtx *cx, double value) {
  return cx->push(Value::floating(value));
}
AmberValue amber_make_str(AmberCtx *cx, const char *ptr, size_t len) {
  return cx->push(cx->host->stdlib_string_value_from_text(
      std::string(ptr == nullptr ? "" : ptr, ptr == nullptr ? 0 : len)));
}
AmberValue amber_make_bytes(AmberCtx *cx, const uint8_t *ptr, size_t len) {
  return cx->push(cx->host->stdlib_bytes_value_from_bytes(std::string(
      reinterpret_cast<const char *>(ptr), ptr == nullptr ? 0 : len)));
}
AmberValue amber_make_list(AmberCtx *cx, const AmberValue *items,
                           size_t count) {
  std::vector<Value> values;
  values.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    values.push_back(cx->resolve(items[i]));
  }
  return cx->push(cx->host->stdlib_make_list(std::move(values)));
}

AmberValue amber_make_handle(AmberCtx *cx, const char *tag, void *ptr) {
  const std::string tag_str = tag == nullptr ? std::string() : tag;
  const amber::runtime::NativeTypeDescriptor *descriptor =
      cx->tags == nullptr ? nullptr : cx->tags->lookup(tag_str);
  if (descriptor == nullptr) {
    cx->host->stdlib_raise_runtime_error(
        cx->frame, "TypeError", "unknown native handle tag '" + tag_str + "'");
    return cx->push(Value::null());
  }
  auto handle = std::make_shared<amber::runtime::RuntimeForeignHandle>();
  handle->tag = tag_str;
  handle->ptr = ptr;
  handle->ownership = descriptor->ownership;
  switch (descriptor->ownership) {
  case amber::runtime::RuntimeForeignHandle::Ownership::Owned: {
    // The descriptor already type-erases the ctx to void* (vm.h stays free of
    // the C ABI types); the value handed in at destroy!-time is the AmberCtx*.
    void (*destroy)(void *, void *) = descriptor->owned_destructor;
    handle->teardown = [destroy](void *ctx, void *resource) {
      if (destroy != nullptr) {
        destroy(ctx, resource);
      }
    };
    break;
  }
  case amber::runtime::RuntimeForeignHandle::Ownership::Collected: {
    void (*reclaim)(void *) = descriptor->collected_reclaim;
    handle->teardown = [reclaim](void *, void *resource) {
      if (reclaim != nullptr) {
        reclaim(resource);
      }
    };
    break;
  }
  case amber::runtime::RuntimeForeignHandle::Ownership::Borrowed:
    break; // Amber never frees a borrowed handle.
  }
  return cx->push(Value::foreign_handle(std::move(handle)));
}

// ---- faults and callbacks ----------------------------------------------

AmberStatus amber_fault(AmberCtx *cx, const char *error_class,
                        const char *message) {
  // Rescuable: maps to an Amber error class an enclosing `rescue` can catch
  // (native-packages design §6), not a terminal fault.
  cx->host->stdlib_raise_runtime_error(
      cx->frame, error_class == nullptr ? "RuntimeError" : error_class,
      message == nullptr ? "" : message);
  return AMBER_ERR;
}

AmberStatus amber_call_block(AmberCtx *cx, AmberValue block,
                             const AmberValue *args, size_t argc,
                             AmberValue *out) {
  std::vector<Value> block_args;
  block_args.reserve(argc);
  for (size_t i = 0; i < argc; ++i) {
    block_args.push_back(cx->resolve(args[i]));
  }
  amber::runtime::StdlibBlockResult result = cx->host->stdlib_call_block(
      cx->frame, cx->resolve(block), std::move(block_args));
  switch (result.status) {
  case amber::runtime::StdlibBlockStatus::Returned:
    *out = cx->push(std::move(result.value));
    return AMBER_OK;
  case amber::runtime::StdlibBlockStatus::Raised:
    cx->host->stdlib_raise_exception(cx->frame, std::move(result.exception));
    return AMBER_ERR;
  case amber::runtime::StdlibBlockStatus::Stopped:
  case amber::runtime::StdlibBlockStatus::Faulted:
    break;
  }
  return AMBER_ERR; // a fault/throw has already been recorded on the frame.
}

uint32_t amber_ext_abi_version(void) { return AMBER_EXT_ABI_VERSION; }

} // extern "C"

namespace amber::runtime {

// ---- process-global registration ---------------------------------------

void NativeExtRegistry::register_thunk(const std::string &logical, void *fn) {
  thunks_[logical] = fn;
}
void NativeExtRegistry::register_type(NativeTypeDescriptor descriptor) {
  tags_.register_type(std::move(descriptor));
}
void NativeExtRegistry::register_error(NativeExtErrorDescriptor descriptor) {
  errors_.push_back(std::move(descriptor));
}
void NativeExtRegistry::register_errors(RuntimeErrorRegistry &errors) const {
  for (const NativeExtErrorDescriptor &descriptor : errors_) {
    errors.register_error(descriptor.name,
                          descriptor.parent.empty() ? "NativeError"
                                                    : descriptor.parent,
                          descriptor.default_message,
                          descriptor.default_exit_code,
                          descriptor.field_mask);
  }
}
void *NativeExtRegistry::lookup(const std::string &logical) const {
  const auto found = thunks_.find(logical);
  return found == thunks_.end() ? nullptr : found->second;
}
NativeExtRegistry &NativeExtRegistry::global() {
  static NativeExtRegistry registry;
  return registry;
}

// ---- bridge helpers (the VM SEND path) ---------------------------------

NativeExtCallOutcome amber_ext_invoke_free(StdlibHost &host, const void *frame,
                                           NativeTagRegistry &tags,
                                           AmberFreeFn fn,
                                           const std::vector<Value> &args) {
  AmberCtx ctx;
  ctx.host = &host;
  ctx.frame = frame;
  ctx.tags = &tags;
  std::vector<AmberValue> handles;
  handles.reserve(args.size());
  for (const Value &arg : args) {
    handles.push_back(ctx.push(arg));
  }
  AmberValue out = nullptr;
  const AmberStatus status = fn(&ctx, handles.data(), handles.size(), &out);
  NativeExtCallOutcome outcome;
  if (status == AMBER_OK) {
    outcome.ok = true;
    outcome.value = ctx.resolve(out);
  }
  return outcome;
}

NativeExtCallOutcome
amber_ext_invoke_method(StdlibHost &host, const void *frame,
                        NativeTagRegistry &tags, AmberMethodFn fn,
                        const Value &self, const std::vector<Value> &args) {
  AmberCtx ctx;
  ctx.host = &host;
  ctx.frame = frame;
  ctx.tags = &tags;
  const AmberValue self_handle = ctx.push(self);
  std::vector<AmberValue> handles;
  handles.reserve(args.size());
  for (const Value &arg : args) {
    handles.push_back(ctx.push(arg));
  }
  AmberValue out = nullptr;
  const AmberStatus status =
      fn(&ctx, self_handle, handles.data(), handles.size(), &out);
  NativeExtCallOutcome outcome;
  if (status == AMBER_OK) {
    outcome.ok = true;
    outcome.value = ctx.resolve(out);
  }
  return outcome;
}

// ---- direct ctx surface (bridge helpers + unit tests) ------------------

AmberCtx *amber_ext_ctx_open(StdlibHost &host, const void *frame,
                             NativeTagRegistry &tags) {
  AmberCtx *ctx = new AmberCtx();
  ctx->host = &host;
  ctx->frame = frame;
  ctx->tags = &tags;
  return ctx;
}
void amber_ext_ctx_close(AmberCtx *ctx) { delete ctx; }
AmberValue amber_ext_ctx_import(AmberCtx *ctx, Value value) {
  return ctx->push(std::move(value));
}
Value amber_ext_ctx_export(AmberCtx *ctx, AmberValue handle) {
  return ctx->resolve(handle);
}

} // namespace amber::runtime
