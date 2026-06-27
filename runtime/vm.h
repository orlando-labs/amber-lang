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
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amber::runtime {

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
