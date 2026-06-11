package conformance.run

cell = [""]

def record(t):
  cell[0] = cell[0] + t

def inner(v):
  try:
    try:
      if v > 0:
        return "deep"
      "shallow"
    ensure:
      record("I")
  ensure:
    record("O")

def probe():
  r = inner(1)
  r + ":" + cell[0]
