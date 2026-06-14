package conformance.run

def probe():
  s = Set{1, 2, 3}
  a = s.added(4)
  d = s.deleted(2)
  u = s.union(Set{3, 4})
  i = s.intersection(Set{2, 3, 5})
  sd = s.symmetric_difference(Set{2, 3, 4})
  if a.count() == 4 and d.count() == 2 and u.count() == 4 and i.count() == 2 and sd.count() == 2 and s.count() == 3:
    42
  else:
    0
