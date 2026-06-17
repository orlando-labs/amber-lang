def base64_encode(bytes)
  [bytes].pack("m0")
end

def base64_decode(text)
  text.unpack1("m0")
end

def base64_lenient_decode(text)
  text.delete(" \n\r\t").unpack1("m0")
end

def base64url_encode(bytes, padding:)
  encoded = base64_encode(bytes).tr("+/", "-_")
  padding ? encoded : encoded.delete("=")
end

def base64url_decode(text)
  normalized = text.tr("-_", "+/")
  normalized += "=" * ((4 - normalized.bytesize % 4) % 4)
  normalized.unpack1("m0")
end

checksum = 0

5_000.times do |i|
  raw = "event-#{i}:#{(i * 17) % 100_000}"

  b64 = base64_encode(raw)
  roundtrip = base64_decode(b64)
  checksum += roundtrip.bytesize
  checksum += roundtrip.getbyte(0) + roundtrip.getbyte(roundtrip.bytesize - 1)

  hx = raw.unpack1("H*")
  url = base64url_encode([hx].pack("H*"), padding: false)
  padded_url = url + ("=" * ((4 - (url.bytesize % 4)) % 4))
  back = base64url_decode(padded_url)
  checksum += back.bytesize
  checksum += back.getbyte(1)

  decoded = [back.unpack1("H*")].pack("H*")
  checksum += decoded.getbyte(2)

  if (i % 16).zero?
    folded = base64_lenient_decode(b64 + "\n")
    checksum += folded.bytesize
  end

  if (i % 31).zero?
    padded = base64url_encode(raw, padding: true)
    checksum += base64url_decode(padded).bytesize
  end

  if (i % 17).zero?
    checksum += [hx].pack("H*").getbyte(0)
  end
end

puts checksum
