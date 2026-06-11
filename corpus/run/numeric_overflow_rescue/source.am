package conformance.run

def probe():
  try:
    4611686018427387904 * 4
  rescue OverflowError |e|:
    "caught: " + e.message()
