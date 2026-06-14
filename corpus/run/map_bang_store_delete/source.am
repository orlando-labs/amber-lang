package conformance.run

def probe():
  m = {"a": 1, "b": 2}
  m.store!("c", 3)
  v = m.delete!("a")
  if v == 1 and m["c"] == 3 and m["b"] == 2 and m.keys().count() == 2:
    42
  else:
    0
