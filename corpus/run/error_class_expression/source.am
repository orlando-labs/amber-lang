package conformance.run

# Builtin runtime error-class names bind as ordinary prelude constants in
# expression position (not only in `rescue` clauses). `SomeError.new(msg)` and
# the bare-call form `SomeError(msg)` both construct a rescuable instance, so an
# error can be built, raised, or stored in an `Err(...)` for Result[T,E].
def probe():
  parts = []
  # `.new(msg)` and bare-call `(msg)` both carry the message through.
  parts = parts + [ValueError.new("bad").message()]
  parts = parts + [OverflowError("big").message()]
  # A constructed error is rescuable by its class, just like a raised builtin.
  caught = "no"
  try:
    raise ArgumentError.new("nope")
  rescue ArgumentError |e|:
    caught = e.message()
  parts = parts + [caught]
  # Held in an Err, `.or_raise` re-raises it through the same exception path.
  bridged = "no"
  try:
    Err(KeyError("missing")).or_raise
  rescue KeyError |e|:
    bridged = e.message()
  parts = parts + [bridged]
  parts.to_str()
