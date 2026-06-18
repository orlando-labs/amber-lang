#pragma once

#include "runtime/stdlib_registry.h"

namespace amber::runtime {

// Shared by Uuid.v4() and SecureRandom.uuid so both APIs produce the same
// immutable UUID value and use the same version/variant bit logic.
SendStatus uuid_v4(NativeStdlibCall &call);

} // namespace amber::runtime
