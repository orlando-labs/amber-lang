package conformance.run

def probe():
  m = {"a": 1, "b": 2, "c": 3, "d": 4}
  m.select! |k, v|: v % 2 == 0
  m.transform_values! |v, k|: v * 10
  if m["b"] == 20 and m["d"] == 40 and m.keys().count() == 2:
    42
  else:
    0
