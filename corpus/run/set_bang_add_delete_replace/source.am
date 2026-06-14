package conformance.run

def probe():
  s = Set{1, 2, 3}
  s.add!(4)
  s.delete!(1)
  s.replace!(Set{7, 8})
  s.add!(8)
  e = Set{7, 8}
  if s.count() == 2 and s.difference(e).count() == 0 and e.difference(s).count() == 0:
    42
  else:
    0
