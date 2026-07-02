package native.numeric_wrapping_core

numeric:
  int: Int8
  overflow: wrapping

export main

def main():
  score = 0
  low = 0 - 127 - 1

  if 100 + 100 == -56:
    score = score + 1
  if 0 - 100 - 100 == 56:
    score = score + 1
  if 100 * 2 == -56:
    score = score + 1
  if low / (0 - 1) == low and low < (0 - 127):
    score = score + 1
  if 100 << 1 == -56:
    score = score + 1
  if 2 ** 7 == low:
    score = score + 1
  if (0 - 2) ** 8 == 0:
    score = score + 1

  score
