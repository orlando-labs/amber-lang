package conformance.run

def probe():
  m = {"a": 1}
  n = m
  m.store!("b", 2)
  m.merge!({"c": 3})
  if n["a"] == 1 and n["b"] == 2 and n["c"] == 3 and n.keys().count() == 3:
    42
  else:
    0
