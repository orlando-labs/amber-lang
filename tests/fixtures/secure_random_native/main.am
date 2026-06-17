package secure_random.native

export main

def main() -> Int !{random, mut}:
  bytes_ok = SecureRandom.bytes(8).count() == 8
  hex_ok = Hex.decode(SecureRandom.hex(8)).count() == 8
  base64_ok = Base64.decode(SecureRandom.base64(3)).count() == 3
  uuid = SecureRandom.uuid
  if bytes_ok and hex_ok and base64_ok and uuid:
    42
  else:
    0
