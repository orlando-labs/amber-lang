package conformance.run

# Block-suffix with a call channel: `m(args): body`. The macro's trailing
# expression reads the caller's `y` through the hygiene read-fallback.
macro def twice_then_add(n, &blk):
  #{blk()}
  #{blk()}
  y + #{n}

def probe():
  y = 0
  twice_then_add(2):
    y = y + 20
