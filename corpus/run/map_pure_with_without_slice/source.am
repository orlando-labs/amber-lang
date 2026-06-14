package conformance.run

def probe():
  m = {"a": 1, "b": 2, "c": 3}
  w = m.with("d", 4)
  wo = m.without("a")
  sl = m.slice("b", "c")
  if w["d"] == 4 and w.keys().count() == 4 and wo.keys().count() == 2 and wo["b"] == 2 and sl.keys().count() == 2 and sl["c"] == 3 and m.keys().count() == 3:
    42
  else:
    0
