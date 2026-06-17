require "securerandom"

ROUNDS = 2_000

def base64_decode(text)
  normalized = text + ("=" * ((4 - text.bytesize % 4) % 4))
  normalized.unpack1("m0")
end

def base64url_decode(text)
  normalized = text.tr("-_", "+/")
  normalized += "=" * ((4 - normalized.bytesize % 4) % 4)
  normalized.unpack1("m0")
end

checksum = 0

ROUNDS.times do
  checksum += SecureRandom.random_bytes(32).bytesize
  checksum += [SecureRandom.hex(16)].pack("H*").bytesize

  b64 = SecureRandom.base64(18)
  checksum += base64_decode(b64).bytesize

  compact = [SecureRandom.random_bytes(17)].pack("m0").delete("=")
  checksum += base64_decode(compact).bytesize

  url = SecureRandom.urlsafe_base64(18, padding: false)
  checksum += base64url_decode(url).bytesize

  uuid = SecureRandom.uuid
  checksum += 37 if uuid.bytesize == 36 && uuid.include?("-")

  value = SecureRandom.random_number(900) + 100
  checksum += 10 if value >= 100 && value <= 999
end

puts checksum
