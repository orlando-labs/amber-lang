package conformance.run

def probe():
  m = {"a": 1, "b": 2}
  m.keep_if! |k, v|: v > 1
  m.reject! |k, v|: v < 0
  ex = m.except("b")
  m.clear!()
  if m.keys().count() == 0 and ex.keys().count() == 0:
    42
  else:
    0
