package native.numeric_saturating_core

numeric:
  int: Int8
  overflow: saturating

export main

def main():
  score = 0
  low = 0 - 127 - 1

  if 100 + 100 == 127:
    score = score + 1
  if 0 - 100 - 100 == low and low < (0 - 127):
    score = score + 1
  if 100 * 2 == 127:
    score = score + 1
  if low / (0 - 1) == 127:
    score = score + 1
  if low // (0 - 1) == 127:
    score = score + 1
  if 100 << 1 == 127:
    score = score + 1
  if 2 ** 7 == 127:
    score = score + 1
  if (0 - 2) ** 7 == low and low < (0 - 127):
    score = score + 1
  if (0 - 2) ** 8 == 127:
    score = score + 1

  score
