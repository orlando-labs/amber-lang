package conformance.run

def sum_odds(n):
  i = 0
  total = 0
  while i < n:
    i = i + 1
    if i % 2 == 0:
      next
    total = total + i
  total

def until_skip(n):
  i = 0
  acc = 0
  until i >= n:
    i = i + 1
    if i == 3:
      next
    acc = acc + i
  acc

def loop_until_break():
  i = 0
  total = 0
  loop:
    i = i + 1
    if i == 2:
      next
    if i > 5:
      break total
    total = total + i

# `next` stays an ordinary identifier when it is not a bare statement.
def next_identifier():
  next = 7
  next + 1

# `next <value>` inside an iterator block finishes that block invocation early,
# yielding the value (here: 0 for evens). `next` is block-local, not loop-control.
def block_next(xs):
  ys = xs.map |x|:
    if x % 2 == 0:
      next 0
    x * 10
  ys.reduce(0) |a, b|: a + b

def probe():
  "#{sum_odds(6)}:#{until_skip(5)}:#{loop_until_break()}:#{next_identifier()}:#{block_next([1, 2, 3, 4])}"
