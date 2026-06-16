package conformance.run

# Math prelude after the Layer 0 stdlib substrate migration: Math is now served
# by the registry handler (runtime/stdlib_math.cpp) instead of the inline
# vm.cpp chain. This locks the selectors the older `extrema_and_math` fixture
# does not cover -- sign (Int result), min, cbrt, round, trunc, log2, atan2,
# and the trig family -- so the registry-routed surface stays byte-for-byte.
def probe():
  sign_neg = Math.sign(0 - 3)
  sign_pos = Math.sign(3)
  mn = Math.min(10, 4)
  cb = Math.cbrt(27)
  rd = Math.round(3.5)
  tr = Math.trunc(3.9)
  l2 = Math.log2(8)
  at = Math.atan2(0, 1)
  sn = Math.sin(0)
  cs = Math.cos(0)
  "#{sign_neg}:#{sign_pos}:#{mn}:#{cb}:#{rd}:#{tr}:#{l2}:#{at}:#{sn}:#{cs}"
