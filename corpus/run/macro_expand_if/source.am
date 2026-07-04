package conformance.run

macro def clamp_low(x):
  quote:
    if unquote(x) < 0:
      0
    else:
      unquote(x)

def probe():
  clamp_low(0 - 5) + clamp_low(42)
