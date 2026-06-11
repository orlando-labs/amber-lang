package conformance.run

def label(v):
  case v:
    when Int:
      "int"
    when Str:
      "str"
    when Float:
      "float"
    when Bool:
      "bool"
    when Null:
      "null"
    when Symbol:
      "symbol"
    when Array:
      "array"
    when Tuple:
      "tuple"
    when Set:
      "set"
    when Map:
      "map"
    when BigInt:
      "bigint"
    when Range:
      "range"
    else:
      "other"

def ordered_arms():
  case 7:
    when 1..5:
      "low"
    when Int:
      "int-arm"
    else:
      "other"

def literal_arms():
  case 5:
    when 7:
      "seven"
    when 5:
      "five"
    else:
      "other"

def type_matches_itself():
  case Int:
    when Int:
      "type"
    else:
      "other"

def probe():
  a = label(5)
  b = label("hi")
  c = label(1.5)
  d = label(true)
  e = label(null)
  f = label(:sym)
  g = label([1, 2])
  h = label((1, 2))
  i = label({1, 2})
  j = label({"k": 1})
  k = label(BigInt(5))
  l = label(1..3)
  m = ordered_arms()
  n = literal_arms()
  o = type_matches_itself()
  "#{a}:#{b}:#{c}:#{d}:#{e}:#{f}:#{g}:#{h}:#{i}:#{j}:#{k}:#{l}:#{m}:#{n}:#{o}"
