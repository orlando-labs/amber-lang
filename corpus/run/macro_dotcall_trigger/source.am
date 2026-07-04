package conformance.run

# Explicit dot-call trigger (§8.1): `m.(args)` expands like the plain call,
# with the call channel visually explicit.
macro def double(x):
  #{x} + #{x}

def probe():
  double.(21)
