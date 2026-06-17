def probe():
  k = "dyn"
  m = {1: 10, 1.0: 20, "name": 30, (k): 40, [1, 2]: 50}
  s = {[1, 2], (1, 2), 1, 1.0}
  tm = Map{"typed": 7}
  ts = Set{1, 1.0, [1, 2], (1, 2)}
  r = {(1..3): 60}

  if m[1] == 20 and m[1.0] == 20 and m["name"] == 30 and m[?:name] == 30 and m[k] == 40 and m[(1, 2)] == 50 and m[[1, 2]] == 50 and m.keys().count() == 4 and s.count() == 2 and tm["typed"] == 7 and ts.count() == 2 and r[(1..3)] == 60:
    42
  else:
    0
