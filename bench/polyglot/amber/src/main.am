package bench.polyglot

export main

def main():
  n = 1000000
  i = 0
  a = 1
  b = 2
  checksum = 0
  while i < n:
    a = a + b + 3
    if a > 2147483647:
      a = a - 2147483647
    b = b + a + i
    if b > 2147483647:
      b = b - 2147483647
    if a > b:
      checksum = checksum + a - b
    else:
      checksum = checksum + b - a
    i = i + 1
  checksum + a + b

main()
