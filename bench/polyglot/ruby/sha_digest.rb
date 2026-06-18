require "digest"
require "openssl"
require "zlib"

PAYLOADS = [
  "Amber digest polyglot benchmark payload zero".b,
  "Amber digest polyglot benchmark payload one 1234567890".b,
  "The quick brown fox jumps over the lazy dog".b,
  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef".b,
].freeze
KEY = "amber-digest-benchmark-key".b

def fold_digest(checksum, digest)
  checksum + digest.bytesize + digest.getbyte(0) + digest.getbyte(-1)
end

def main
  checksum = 0
  4000.times do |i|
    data = PAYLOADS[i % PAYLOADS.length]
    checksum = fold_digest(checksum, [Zlib.crc32(data)].pack("N"))
    checksum = fold_digest(checksum, Digest::MD5.digest(data))
    checksum = fold_digest(checksum, Digest::SHA1.digest(data))
    checksum = fold_digest(checksum, Digest::SHA256.digest(data))
    checksum = fold_digest(
      checksum,
      OpenSSL::HMAC.digest(OpenSSL::Digest.new("SHA256"), KEY, data),
    )
  end
  checksum
end

puts main
