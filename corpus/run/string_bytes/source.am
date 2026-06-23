package conformance.run

# `Str#bytes` exposes the string's raw UTF-8 octets as an immutable io.Bytes
# value. "Aé" is A (0x41) plus é (0xc3 0xa9), so three bytes; count is byte
# length (not codepoints), [] yields the unsigned octet, and to_str round-trips.
def probe():
  b = "Aé".bytes()
  "#{b.count()}:#{b.hex()}:#{b[0]}:#{b.to_str()}"
