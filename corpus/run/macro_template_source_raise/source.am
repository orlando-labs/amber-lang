package conformance.run

# The design's headline example (§6): `#{check}` splices the unevaluated
# argument, `#{check.source}` recovers its verbatim source text and splices it
# as a string literal — something no function can do.
macro def assert2(check):
  if #{check}:
    null
  else:
    raise ValueError.new("assertion failed: " + #{check.source})

def probe():
  x = 3
  caught = "no"
  try:
    assert2(x > 5)
  rescue ValueError |e|:
    caught = e.message()
  caught
