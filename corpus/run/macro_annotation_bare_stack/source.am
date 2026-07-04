package conformance.run

# Bare (paren-less) annotation calls stack: a contiguous line-adjacent run
# composes top-to-bottom onto the declaration below (§8 attachment rule).
macro def tagged(label, d):
  return d

tagged "first"
tagged "second"
def forty_two():
  42

def probe():
  forty_two()
