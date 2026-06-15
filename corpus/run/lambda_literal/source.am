package conformance.run

# Standalone lambda literals: `|params|: body` in expression position is a
# first-class callable. Captures lexical scope, composes (lambda returning
# lambda), and forwards through the block channel (&fn). Distinct from the
# infix bitwise-or `|`, which is only ambiguous-free because a leading `|` is
# always a lambda.
def apply2(v, &fn):
  fn(v)

def probe():
  inc = |x|: x + 1
  add = |a, b|: a + b
  n = 100
  cap = |x|: x + n
  make = |k|: |x|: x * k
  triple = make(3)
  "#{inc(10)}:#{add(3, 4)}:#{cap(5)}:#{triple(7)}:#{apply2(9, &inc)}:#{(|x|: x * x)(6)}"
