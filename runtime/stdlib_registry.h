#pragma once

// Layer 0 stdlib substrate (DESIGN-stdlib-next-libs-order-2026-06-15 §4.0).
//
// A native stdlib library is reached through two hand-written `if`-chains in the
// 1.1 MB `runtime/vm.cpp`: dispatch (`try_apply_native_stdlib_send`) and name
// resolution (`lookup_native_prelude_constant`). This header is the seam that
// lets a library land as a self-contained `runtime/stdlib_<name>.{h,cpp}`
// translation unit registered through a table, instead of more branches.
//
// The ABI deliberately names no `vm.cpp`-internal type. `Value` and
// `RuntimeNativeTypeKind` come from the public runtime header; the active call
// frame is type-erased to `const void *` (the host casts it back), so a stdlib
// translation unit never has to see `Frame`, `Vm`, or their headers.

#include "runtime/vm.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amber::runtime {

// Outcome of a SEND attempt. `NotHandled` means "not mine, keep looking" and is
// what lets a registered handler coexist with the legacy inline chain: an
// unknown selector for a migrated kind returns `NotHandled` and falls through.
enum class SendStatus { Matched, NotHandled, Faulted };

// The narrow runtime facade a stdlib handler is allowed to touch. Implemented by
// the VM; every method that needs the active frame takes it type-erased so this
// interface stays free of `vm.cpp`-internal types.
class StdlibHost {
public:
  virtual ~StdlibHost() = default;

  // Record a terminal fault on the active frame (the VM's `set_fault`).
  virtual void stdlib_set_fault(const void *frame,
                                const std::string &error_class,
                                const std::string &message) = 0;

  // Look up a keyword argument by name; `nullopt` when absent.
  virtual std::optional<Value> stdlib_keyword_arg_value(
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      const std::string &name) = 0;

  // Fault unless every supplied keyword is in `allowed`. Returns false (and sets
  // the fault) on the first stray keyword.
  virtual bool stdlib_reject_unknown_keywords(
      const void *frame,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      std::initializer_list<const char *> allowed) = 0;

  // Intern `text` into the module string table and wrap it as a String value.
  virtual Value stdlib_string_value_from_text(std::string text) = 0;

  // --- value introspection (used by generators) -----------------------------
  // The text of a String or Symbol value (a Symbol yields its name); nullopt for
  // any other kind. Lets a stdlib unit read string/key text without touching the
  // module string/symbol tables directly.
  virtual std::optional<std::string> stdlib_text_of(const Value &value) = 0;

  // --- value construction (used by parsers) ----------------------------------
  // Build a List value from `items`.
  virtual Value stdlib_make_list(std::vector<Value> items) = 0;

  // Build a map from string-keyed entries. `strict=false` yields an ordinary
  // name-indifferent Map (keys stored as Str, canonical identity interned);
  // `strict=true` yields a StrictMap. Entry order is preserved; later duplicate
  // keys overwrite earlier ones per the map's strictness.
  virtual Value
  stdlib_make_object(std::vector<std::pair<std::string, Value>> entries,
                     bool strict) = 0;
};

// One SEND, packaged as a single context object instead of seven positional
// parameters. References borrow the caller's storage for the duration of the
// dispatch; `out` receives the result on `Matched`.
struct NativeStdlibCall {
  StdlibHost &host;
  const void *frame;
  RuntimeNativeTypeKind kind;
  const std::string &selector;
  const std::vector<Value> &args;
  const Value &block;
  const std::vector<std::pair<std::uint32_t, Value>> &kw_args;
  Value *out;

  // ---- guard / helper facade (the inline guards used in vm.cpp today) -------

  // Set `error_class`/`message` and report `Faulted` in one expression.
  SendStatus fault(const std::string &error_class,
                   const std::string &message) const {
    host.stdlib_set_fault(frame, error_class, message);
    return SendStatus::Faulted;
  }

  bool require_arity(std::size_t expected) const {
    if (args.size() != expected) {
      host.stdlib_set_fault(frame, "TypeError",
                            "wrong native stdlib SEND arity");
      return false;
    }
    return true;
  }

  bool require_no_block() const {
    if (!block.is_null()) {
      host.stdlib_set_fault(
          frame, "TypeError",
          "native stdlib selector does not accept block arguments");
      return false;
    }
    return true;
  }

  std::optional<Value> keyword(const std::string &name) const {
    return host.stdlib_keyword_arg_value(kw_args, name);
  }

  bool reject_unknown_keywords(
      std::initializer_list<const char *> allowed) const {
    return host.stdlib_reject_unknown_keywords(frame, kw_args, allowed);
  }

  bool bool_keyword(const std::string &name, bool fallback, bool *value) const {
    const std::optional<Value> argument = keyword(name);
    if (!argument.has_value()) {
      *value = fallback;
      return true;
    }
    if (!argument->is_bool()) {
      host.stdlib_set_fault(frame, "TypeError", name + " must be Bool");
      return false;
    }
    *value = argument->as_bool();
    return true;
  }

  Value string_value(std::string text) const {
    return host.stdlib_string_value_from_text(std::move(text));
  }

  std::optional<std::string> text_of(const Value &value) const {
    return host.stdlib_text_of(value);
  }

  Value make_list(std::vector<Value> items) const {
    return host.stdlib_make_list(std::move(items));
  }

  Value make_object(std::vector<std::pair<std::string, Value>> entries,
                    bool strict = false) const {
    return host.stdlib_make_object(std::move(entries), strict);
  }
};

// A library handler keeps the proven chain-of-responsibility shape; it owns its
// kind's selectors and returns `NotHandled` for anything it does not.
using NativeStdlibHandler = SendStatus (*)(NativeStdlibCall &call);

// Two tables, populated once during VM construction (no static initializers, to
// avoid static-init-order fiascos): `kind -> handler` for dispatch and
// `path -> kind` for prelude name resolution.
class NativeRegistry {
public:
  void register_handler(RuntimeNativeTypeKind kind, NativeStdlibHandler handler);
  void register_path(std::string path, RuntimeNativeTypeKind kind);

  // The handler that owns `kind`, or nullptr when the kind is still on the
  // legacy inline chain.
  NativeStdlibHandler handler_for(RuntimeNativeTypeKind kind) const;

  // The kind a source path (`"Math"`, `"io.ByteBuffer"`, ...) resolves to, or
  // `nullopt` when unregistered.
  std::optional<RuntimeNativeTypeKind>
  kind_for_path(const std::string &path) const;

private:
  struct KindHash {
    std::size_t operator()(RuntimeNativeTypeKind kind) const {
      return static_cast<std::size_t>(kind);
    }
  };
  std::unordered_map<RuntimeNativeTypeKind, NativeStdlibHandler, KindHash>
      handlers_;
  std::unordered_map<std::string, RuntimeNativeTypeKind> paths_;
};

// Populate `registry` with every builtin library. Each library contributes a
// `register_<name>` that adds its names and handler; this is the single place
// that lists them.
void register_builtin_stdlib(NativeRegistry &registry);

// Per-library registration entry points (defined in `runtime/stdlib_<name>.cpp`).
void register_math(NativeRegistry &registry);
void register_json(NativeRegistry &registry);

} // namespace amber::runtime
