package conformance.run

# Kernel form of sibling splicing: `unquote_splice(list)` inside an explicit
# `quote:` splices a List/Tuple of Ast values between literal siblings.
macro def call_mixed(*xs):
  quote:
    add3(1, unquote_splice(xs), 2)

def add3(a, b, c):
  a + b + c

def probe():
  call_mixed(39)
