def probe():
  generated = Array.of(4) |i|:
    i + 1
  implicit = Array.of(3):
    _1 * 2
  built = Array.build(3) |i|:
    i + 10
  filled = Array.filled(4, false)
  numbers = [10, 20, 30]
  m = {a: 1, b: null}

  if generated[0] == 1 and generated[3] == 4 and implicit[2] == 4 and built[1] == 11 and filled[0] == false and numbers[?0] == 10 and numbers[?2] == 30 and numbers[?3] == null and numbers[?-1] == 30 and numbers.has_index?(2) and numbers.has_index?(3) == false and m[?:a] == 1 and m[?:b] == null and m[?:c] == null and m.contains?(:b) and m.contains?(:c) == false:
    42
  else:
    0
