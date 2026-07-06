package conformance.run

def probe():
  try:
    (1..3).foobar()
    "not caught"
  rescue NoMethodError |e|:
    e.message()
