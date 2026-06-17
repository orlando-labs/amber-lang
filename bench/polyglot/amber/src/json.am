package bench.polyglot.json

export main

def main():
  checksum = 0
  i = 0
  while i < 1000:
    document = {id: i, value: i * 2, group: i % 7, items: [i, i + 1, i + 2], name: "event"}
    compact = document.to_json
    parsed = Json.parse(compact)
    checksum = checksum + parsed[:id] + parsed[:value] + parsed[:group] + parsed[:items][1]
    if i < 20:
      pretty = Json.pretty_generate(parsed)
      reparsed = Json.parse(pretty)
      checksum = checksum + reparsed[:items][2]
    i = i + 1

  count = Json.stream_parse_file("bench/polyglot/build/json/events.jsonl", jsonl: true) |row|:
    checksum = checksum + row[:id] * 3 + row[:value] - row[:group]
  checksum + count

main()
