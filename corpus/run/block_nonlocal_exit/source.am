package conformance.run

# throw/catch and raise/rescue are dynamically scoped THROUGH iterator blocks:
# an exception or throw raised inside a block propagates to a handler in the
# enclosing scope (blocks run on a pooled child VM, but control flow tunnels
# out). This is the blessed early-exit mechanism for chains; `break` is not.
def find_via_throw(xs):
  catch(:found):
    xs.each |x|:
      if x > 3:
        throw :found, x * 10
    -1

def rescue_across_map(xs):
  try:
    xs.map |x|:
      x / 0
    "no error"
  rescue ZeroDivisionError |e|:
    "caught"

# A handler INSIDE the block stays contained — it does not escape to the caller.
def contained(xs):
  xs.map |x|:
    catch(:inner):
      if x == 2:
        throw :inner, 99
      x

def probe():
  "#{find_via_throw([1, 2, 3, 4, 5])}:#{rescue_across_map([1, 2])}:#{contained([1, 2, 3])}"
