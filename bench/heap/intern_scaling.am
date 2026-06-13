package bench.heap.intern_scaling

export main

# Micro-benchmark for the runtime string-intern path (RESEARCH §7.1). Each round
# builds a distinct string (plus distinct .to_str() results), so the runtime
# string table grows ~linearly with ROUNDS. With the old linear intern scan this
# is O(n^2) in time; with the Layer-2a hash index it is O(n). Same ROUNDS under
# both builds isolates the intern cost. (It still grows memory unboundedly --
# that is Layer-2b, not addressed here.)

def rounds():
  30000

def build_strings(n):
  total = 0
  n.times |i|:
    s = "k-" + i.to_str() + "-" + (i * 1000003 % 100000007).to_str()
    if s != "sentinel":
      total = (total + 1) % 2147483647
  total

def main():
  build_strings(rounds())

main()
