package conformance.run

def probe():
  big = BigInt(9223372036854775807)
  square = big * big
  parsed = BigInt("123456789012345678901234567890")
  mixed = 5 + BigInt(10)
  back = BigInt(42).to_int()
  quotient = BigInt(0 - 5) / 2
  remainder = BigInt(0 - 5) % 2
  power = BigInt(2) ** 100
  "#{square}|#{parsed % 97}|#{mixed}|#{back}|#{quotient}:#{remainder}|#{power}"
