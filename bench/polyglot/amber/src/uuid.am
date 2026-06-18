package bench.polyglot.uuid

export main

def main() -> Int !{random, time, mut}:
  checksum = 0
  i = 0
  while i < 5000:
    v4 = Uuid.v4()
    v4_text = v4.to_str
    parsed_v4 = UUID.parse(v4_text)

    v7 = Uuid.v7()
    v7_text = v7.inspect
    parsed_v7 = Uuid.parse(v7_text)

    delegated = SecureRandom.uuid
    delegated_json = delegated.to_json

    fixed = Uuid.parse("550E8400-E29B-41D4-A716-446655440000")
    fixed_text = fixed.to_str

    checksum = checksum + v4.version
    checksum = checksum + v7.version
    checksum = checksum + delegated.version
    checksum = checksum + v4_text.length()
    checksum = checksum + v7_text.length()
    checksum = checksum + delegated_json.length()

    if v4 == parsed_v4:
      checksum = checksum + 11
    if v7 == parsed_v7:
      checksum = checksum + 13
    if Uuid === v4 and UUID === v7:
      checksum = checksum + 17
    if fixed.version == 4 and fixed_text == "550e8400-e29b-41d4-a716-446655440000":
      checksum = checksum + 19
    if v4_text.contains?("-") and v7_text.contains?("-"):
      checksum = checksum + 23

    i = i + 1

  checksum

main()
