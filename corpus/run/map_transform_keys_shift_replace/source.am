package conformance.run

def probe():
  m = {1: 10, 2: 20}
  tk = m.transform_keys |k, v|: k * 100
  first = m.shift!()
  m.replace!({9: 90})
  if tk[100] == 10 and tk[200] == 20 and first == (1, 10) and m[9] == 90 and m.keys().count() == 1:
    42
  else:
    0
