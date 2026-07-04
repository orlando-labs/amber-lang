package conformance.run

# `use m` (§8.4): an injection macro invoked inside a class body receives the
# enclosing class declaration Ast and expands to member declarations spliced
# in place — compile-time capability mixins, statically typed.
macro def with_double(cls):
  def double(x):
    x + x

macro def named(cls):
  def type_name():
    #{cls.name}

class Calc:
  use with_double
  use named
  def base():
    21

def probe():
  c = Calc()
  if c.type_name() == "Calc":
    c.double(c.base())
  else:
    0
