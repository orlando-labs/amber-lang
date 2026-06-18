package uuid.native

export main

def main() -> Int !{random, time, mut}:
  v4 = Uuid.v4()
  parsed = UUID.parse(v4.to_str)
  v7 = Uuid.v7()
  delegated = SecureRandom.uuid
  if Uuid === v4 and UUID === v7 and Uuid === UUID and UUID === Uuid and v4 == parsed and v4.inspect == v4.to_str and v4.version == 4 and v7.version == 7 and delegated.version == 4 and v4.to_str.length() == 36 and v7.to_json.length() == 38:
    42
  else:
    0
