#pragma once

// In-tree C++ surface for the native-extension ABI (native-packages design §6,
// 5c-ii dispatch). `runtime/amber_ext.h` is the stable C contract an external
// author compiles against; this header is how the runtime *drives* that contract
// from inside the tree: a process-global registration table the generated native
// binary populates at startup, and the bridge helpers the VM SEND path uses to
// marshal a call across the ABI to a linked C thunk.
//
// The marshalling itself (AmberCtx, AmberValue arena, every amber_ext.h
// function) lives in `runtime/amber_ext.cpp`; AmberCtx is opaque here, exactly
// as it is to an extension author.

#include "runtime/amber_ext.h"
#include "runtime/stdlib_registry.h" // StdlibHost, NativeTagRegistry, Value

#include <string>
#include <unordered_map>
#include <vector>

namespace amber::runtime {

// One registered native binding: a thunk function pointer plus whether it is a
// free function (`(args) -> out`) or a handle method (`(self, args) -> out`).
struct NativeExtBinding {
  enum class Kind { Free, Method };
  Kind kind = Kind::Free;
  void *fn = nullptr; // AmberFreeFn or AmberMethodFn, per `kind`
};

// The native binary's logical-name -> thunk table plus the foreign-handle tag
// registry. Populated once at process startup by generated registration calls
// (one per `[native.symbols]` / `[[native.types]]` entry); empty in a plain
// bytecode run, which is exactly what makes a `native def` fall back to its
// Amber body when no thunk is registered.
class NativeExtRegistry {
public:
  void register_free(const std::string &logical, AmberFreeFn fn);
  void register_method(const std::string &logical, AmberMethodFn fn);
  void register_type(NativeTypeDescriptor descriptor);

  // The binding for `logical`, or nullptr when none is registered.
  const NativeExtBinding *lookup(const std::string &logical) const;

  NativeTagRegistry &tags() { return tags_; }
  const NativeTagRegistry &tags() const { return tags_; }

  bool empty() const { return bindings_.empty(); }
  std::size_t size() const { return bindings_.size(); }

  // The single process-global instance the generated binary fills and the VM
  // consults.
  static NativeExtRegistry &global();

private:
  std::unordered_map<std::string, NativeExtBinding> bindings_;
  NativeTagRegistry tags_;
};

// Outcome of bridging a native-extension call through the C ABI. On `!ok` a
// fault or exception has already been recorded on the active frame via the host,
// so the VM only has to stop and let it propagate.
struct NativeExtCallOutcome {
  bool ok = false;
  Value value = Value::null();
};

// Build an AmberCtx over (host, frame, tags), marshal `args` (and `self` for a
// method) into the per-call arena, invoke the thunk, and marshal the result
// back. The frame is type-erased to `const void *` exactly as in StdlibHost.
NativeExtCallOutcome amber_ext_invoke_free(StdlibHost &host, const void *frame,
                                           NativeTagRegistry &tags,
                                           AmberFreeFn fn,
                                           const std::vector<Value> &args);
NativeExtCallOutcome amber_ext_invoke_method(StdlibHost &host,
                                             const void *frame,
                                             NativeTagRegistry &tags,
                                             AmberMethodFn fn, const Value &self,
                                             const std::vector<Value> &args);

// Direct AmberCtx surface, used by the bridge helpers above and by unit tests
// that exercise individual amber_ext.h functions. `import` pushes a runtime
// Value into the arena and returns its opaque handle; `export` reads a handle
// back out. Handles are valid until `amber_ext_ctx_close`.
AmberCtx *amber_ext_ctx_open(StdlibHost &host, const void *frame,
                             NativeTagRegistry &tags);
void amber_ext_ctx_close(AmberCtx *ctx);
AmberValue amber_ext_ctx_import(AmberCtx *ctx, Value value);
Value amber_ext_ctx_export(AmberCtx *ctx, AmberValue handle);

} // namespace amber::runtime
