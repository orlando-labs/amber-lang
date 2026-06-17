package bench.vm.json_compact_roundtrip

export main

def main():
  checksum = 0
  i = 0
  while i < 4000:
    document = {id: i, value: i * 2, group: i % 11, items: [i, i + 1, i + 2], name: "event"}
    compact = Json.generate(document)
    parsed = Json.parse(compact)
    checksum = checksum + parsed[:id] + parsed[:value] + parsed[:group] + parsed[:items][2]
    i = i + 1
  checksum

main()

