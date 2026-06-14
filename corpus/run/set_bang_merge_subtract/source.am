package conformance.run

def probe():
  s = Set{1, 2}
  t = s
  s.merge!(Set{2, 3, 4})
  s.subtract!(Set{1, 4})
  e = Set{2, 3}
  if t.count() == 2 and t.difference(e).count() == 0 and e.difference(t).count() == 0:
    42
  else:
    0
