// Math — reference migration onto the Layer 0 stdlib substrate
// (DESIGN-stdlib-next-libs-order-2026-06-15 §4.0, step 4). Math is small,
// self-contained, and couples to no VM state beyond the facade, so it validates
// the `NativeStdlibCall` ABI before `Json` and the rest depend on it.
//
// Behaviour is byte-for-byte the legacy inline `kind == Math` handler: same
// selectors, same Int/Float result typing, same fault messages.

#include "runtime/stdlib_registry.h"

#include <cmath>

namespace amber::runtime {

namespace {

double numeric_as_double(const Value &value) {
  return value.is_integer() ? static_cast<double>(value.as_integer())
                            : value.as_float();
}

SendStatus math_dispatch(NativeStdlibCall &call) {
  if (!call.require_no_block()) {
    return SendStatus::Faulted;
  }
  if (!call.kw_args.empty()) {
    return call.fault("TypeError",
                      "Math methods do not accept keyword arguments");
  }

  const std::string &selector = call.selector;
  if (selector == "PI") {
    if (!call.require_arity(0)) {
      return SendStatus::Faulted;
    }
    *call.out = Value::floating(3.141592653589793238462643383279502884);
    return SendStatus::Matched;
  }
  if (selector == "E") {
    if (!call.require_arity(0)) {
      return SendStatus::Faulted;
    }
    *call.out = Value::floating(2.718281828459045235360287471352662498);
    return SendStatus::Matched;
  }

  const auto math_arg = [&](std::size_t i, double *d) -> bool {
    if (!call.args[i].is_integer() && !call.args[i].is_float()) {
      call.fault("TypeError", "Math." + selector + " expects a number");
      return false;
    }
    *d = numeric_as_double(call.args[i]);
    return true;
  };

  // `abs` and `min`/`max` preserve the argument's Int/Float type; `sign` yields
  // an Int. Everything else returns a Float.
  if (selector == "abs") {
    if (!call.require_arity(1)) {
      return SendStatus::Faulted;
    }
    if (call.args[0].is_integer()) {
      const std::int64_t v = call.args[0].as_integer();
      *call.out = Value::integer(v < 0 ? -v : v);
      return SendStatus::Matched;
    }
    double d = 0.0;
    if (!math_arg(0, &d)) {
      return SendStatus::Faulted;
    }
    *call.out = Value::floating(std::fabs(d));
    return SendStatus::Matched;
  }
  if (selector == "sign") {
    if (!call.require_arity(1)) {
      return SendStatus::Faulted;
    }
    double d = 0.0;
    if (!math_arg(0, &d)) {
      return SendStatus::Faulted;
    }
    *call.out = Value::integer(d > 0.0 ? 1 : (d < 0.0 ? -1 : 0));
    return SendStatus::Matched;
  }
  if (selector == "min" || selector == "max") {
    if (!call.require_arity(2)) {
      return SendStatus::Faulted;
    }
    double a = 0.0;
    double b = 0.0;
    if (!math_arg(0, &a) || !math_arg(1, &b)) {
      return SendStatus::Faulted;
    }
    const bool pick_first = selector == "min" ? (a <= b) : (a >= b);
    *call.out = pick_first ? call.args[0] : call.args[1];
    return SendStatus::Matched;
  }
  if (selector == "pow" || selector == "hypot" || selector == "atan2") {
    if (!call.require_arity(2)) {
      return SendStatus::Faulted;
    }
    double a = 0.0;
    double b = 0.0;
    if (!math_arg(0, &a) || !math_arg(1, &b)) {
      return SendStatus::Faulted;
    }
    double r = 0.0;
    if (selector == "pow") {
      r = std::pow(a, b);
    } else if (selector == "hypot") {
      r = std::hypot(a, b);
    } else {
      r = std::atan2(a, b);
    }
    *call.out = Value::floating(r);
    return SendStatus::Matched;
  }
  if (selector == "sqrt" || selector == "cbrt" || selector == "exp" ||
      selector == "log" || selector == "log2" || selector == "log10" ||
      selector == "sin" || selector == "cos" || selector == "tan" ||
      selector == "asin" || selector == "acos" || selector == "atan" ||
      selector == "floor" || selector == "ceil" || selector == "round" ||
      selector == "trunc") {
    if (!call.require_arity(1)) {
      return SendStatus::Faulted;
    }
    double d = 0.0;
    if (!math_arg(0, &d)) {
      return SendStatus::Faulted;
    }
    double r = 0.0;
    if (selector == "sqrt") {
      r = std::sqrt(d);
    } else if (selector == "cbrt") {
      r = std::cbrt(d);
    } else if (selector == "exp") {
      r = std::exp(d);
    } else if (selector == "log") {
      r = std::log(d);
    } else if (selector == "log2") {
      r = std::log2(d);
    } else if (selector == "log10") {
      r = std::log10(d);
    } else if (selector == "sin") {
      r = std::sin(d);
    } else if (selector == "cos") {
      r = std::cos(d);
    } else if (selector == "tan") {
      r = std::tan(d);
    } else if (selector == "asin") {
      r = std::asin(d);
    } else if (selector == "acos") {
      r = std::acos(d);
    } else if (selector == "atan") {
      r = std::atan(d);
    } else if (selector == "floor") {
      r = std::floor(d);
    } else if (selector == "ceil") {
      r = std::ceil(d);
    } else if (selector == "round") {
      r = std::round(d);
    } else {
      r = std::trunc(d);
    }
    *call.out = Value::floating(r);
    return SendStatus::Matched;
  }

  return SendStatus::NotHandled;
}

} // namespace

void register_math(NativeRegistry &registry) {
  registry.register_path("Math", RuntimeNativeTypeKind::Math);
  registry.register_handler(RuntimeNativeTypeKind::Math, &math_dispatch);
}

} // namespace amber::runtime
