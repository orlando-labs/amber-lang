package conformance.run

macro def double(x):
  #{x} + #{x}

macro def quadruple(x):
  double(double(#{x}))

def probe():
  quadruple(10) + 2
