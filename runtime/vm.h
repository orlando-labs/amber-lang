#pragma once

#include "bytecode/format.h"
#include "package/package.h"
#include "profile/capabilities.h"
#include "runtime/errors.h"
#include "runtime/heap.h"
#include "runtime/concurrency.h"
#include "runtime/numeric.h"
#include "runtime/text.h"
#include "runtime/value.h"
#include "runtime/value_display.h"
#include "runtime/watch.h"
#include "runtime/objects.h"
#include "runtime/world.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amber::runtime {

// Backs a `native class` instance: an opaque host pointer plus the
// deterministic lifetime model from the native-packages design (§7). Teardown
// runs through destroy!/memory.dealloc; the GC never runs an `owned` destructor
// (no implicit finalizer), and only a `collected` handle opts into GC
// reclamation. Held only via shared_ptr (single owner), so copying is disabled
// to prevent double-free.
struct RuntimeForeignHandle {
  enum class Ownership { Owned, Borrowed, Collected };

  std::string tag;     // per-(package,type) dispatch identity
  void *ptr = nullptr; // the wrapped foreign resource
  Ownership ownership = Ownership::Borrowed;
  // Reclaim callback: the context-free reclaim for `collected`, a ctx-bound
  // closure supplied by the ABI layer for `owned`, and empty for `borrowed`.
  // The ctx is the AmberCtx supplied at deterministic destroy!-time,
  // type-erased to void* so this header stays free of the amber_ext.h C ABI
  // types; an `owned` destructor uses it, a `collected` reclaim ignores it. The
  // GC reclaim path passes nullptr (collected only, context-free by
  // construction).
  std::function<void(void *ctx, void *ptr)> teardown;
  bool live = true; // tombstone: cleared once destroyed

  RuntimeForeignHandle() = default;
  RuntimeForeignHandle(const RuntimeForeignHandle &) = delete;
  RuntimeForeignHandle &operator=(const RuntimeForeignHandle &) = delete;

  // Deterministic destroy! / memory.dealloc: runs teardown once for an owning
  // handle, flips the tombstone, and reports whether this call destroyed it.
  // `ctx` is the live AmberCtx the destructor may use (owned); a collected
  // reclaim ignores it. A bytecode-only destroy! with no extension context
  // passes nullptr.
  bool destroy(void *ctx = nullptr) {
    if (!live) {
      return false;
    }
    live = false;
    if (ownership != Ownership::Borrowed && teardown) {
      teardown(ctx, ptr);
    }
    return true;
  }

  // GC reclamation: only a `collected` handle runs teardown here (the opt-in
  // finalizer). An `owned` handle never runs its destructor from the collector
  // (the leak is surfaced by a backstop diagnostic elsewhere); `borrowed` never
  // frees. The collector has no AmberCtx, so it passes nullptr — sound because
  // a `collected` reclaim is context-free by construction (design §7.4).
  ~RuntimeForeignHandle() {
    if (live && ownership == Ownership::Collected && teardown) {
      teardown(nullptr, ptr);
    }
  }
};

// Per-(package,type) descriptor for a `native class`: the dispatch tag plus the
// ownership and reclaim resolved from the manifest [[native.types]] and the
// linked extension symbols. The native binary registers one per type at
// startup; amber_make_handle(cx, tag, ptr) looks it up by tag to build a
// correctly-owned RuntimeForeignHandle. The ctx is type-erased to void* so this
// stays free of the amber_ext.h C ABI types.
struct NativeTypeDescriptor {
  std::string tag;
  RuntimeForeignHandle::Ownership ownership =
      RuntimeForeignHandle::Ownership::Borrowed;
  // ctx-bound destructor for `owned`; context-free reclaim for `collected`;
  // both null for `borrowed`.
  void (*owned_destructor)(void *ctx, void *handle) = nullptr;
  void (*collected_reclaim)(void *handle) = nullptr;
};

// The native binary's tag -> descriptor table. Populated once at startup by
// generated registration calls (one per [[native.types]] entry).
class NativeTagRegistry {
public:
  void register_type(NativeTypeDescriptor descriptor) {
    types_[descriptor.tag] = std::move(descriptor);
  }
  const NativeTypeDescriptor *lookup(const std::string &tag) const {
    const auto found = types_.find(tag);
    return found == types_.end() ? nullptr : &found->second;
  }
  std::size_t size() const { return types_.size(); }

private:
  std::unordered_map<std::string, NativeTypeDescriptor> types_;
};

std::string runtime_uuid_to_string(const RuntimeUuidValue &value);
std::string runtime_time_to_iso8601(const RuntimeTimeValue &value);
std::string runtime_time_period_to_string(const RuntimeTimePeriodValue &value);

struct ErrorInstanceValue {
  std::uint16_t error_id = 0;
  std::string message;
  std::vector<std::pair<std::string, Value>> fields;
};

struct RuntimeArgParserValue {
  enum class SpecKind { Option, Flag, Positional, Rest };

  struct Spec {
    SpecKind kind = SpecKind::Option;
    std::vector<std::string> spellings;
    std::string name;
    RuntimeNativeTypeKind type = RuntimeNativeTypeKind::Str;
    bool required = false;
    bool multiple = false;
    bool negatable = false;
    bool has_default = false;
    Value default_value = Value::null();
    bool has_choices = false;
    std::vector<Value> choices;
    std::string env;
    Value block = Value::null();
  };

  std::string name;
  std::string about;
  std::vector<std::string> cmdline;
  std::vector<std::pair<std::string, std::string>> env;
  bool add_help = true;
  std::vector<Spec> specs;
};

ExecutionResult execute_code(const bytecode::BcModule &module,
                             std::uint32_t code_id,
                             const std::vector<Value> &args = {},
                             Value self = Value::null(),
                             Value block = Value::null());

} // namespace amber::runtime
