package bench.polyglot.codecs

export main

def main():
  checksum = 0
  i = 0
  while i < 5000:
    raw = "event-" + i.to_str() + ":" + ((i * 17) % 100000).to_str()
    bytes = Bytes.new(raw)

    b64 = Base64.encode(bytes)
    roundtrip = Base64.decode(b64)
    checksum = checksum + roundtrip.count()
    checksum = checksum + roundtrip[0] + roundtrip[roundtrip.count() - 1]

    hx = Hex.encode(bytes)
    url = Base64Url.encode(Hex.decode(hx))
    back = Base64Url.decode(url)
    checksum = checksum + back.count()
    checksum = checksum + back[1]

    decoded = Hex.decode(Hex.encode(back))
    checksum = checksum + decoded[2]

    if i % 16 == 0:
      folded = Base64.decode(b64 + "\n", mode: :lenient)
      checksum = checksum + folded.count()

    if i % 31 == 0:
      padded = Base64Url.encode(bytes, padding: true)
      checksum = checksum + Base64Url.decode(padded).count()

    if i % 17 == 0:
      checksum = checksum + Hex.decode("0x" + hx, mode: :lenient)[0]

    i = i + 1

  checksum

main()
