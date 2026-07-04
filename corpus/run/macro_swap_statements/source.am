package conformance.run

# The design's `swap` example (§6/§9): a multi-statement template expands as
# sibling statements at a statement-position call; splice holes are valid
# assignment targets; `tmp` is hygienic across all emitted statements.
macro def swap(a, b):
  tmp = #{a}
  #{a} = #{b}
  #{b} = tmp

def probe():
  x = 1
  y = 41
  swap(x, y)
  x - y + 2
