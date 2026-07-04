package conformance.run

# `#{*list}` splices a compile-time sequence of Ast values as siblings —
# here a rest-param Tuple becomes call arguments between literal ones.
macro def call_add3(*xs):
  add3(1, #{*xs}, 2)

def add3(a, b, c):
  a + b + c

def probe():
  call_add3(39)
