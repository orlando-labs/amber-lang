package conformance.run

def probe():
  m = {"a": 1, "b": null, "c": 3}
  c = m.compact()
  m.delete_if! |k, v|: v == null
  if c.keys().count() == 2 and m.keys().count() == 2 and m["a"] == 1 and m["c"] == 3:
    42
  else:
    0
