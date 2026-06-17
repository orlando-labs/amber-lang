require "json"

JSON_PATH = "bench/polyglot/build/json/events.jsonl"

def main
  checksum = 0
  i = 0
  while i < 1000
    document = {
      id: i,
      value: i * 2,
      group: i % 7,
      items: [i, i + 1, i + 2],
      name: "event",
    }
    compact = JSON.generate(document)
    parsed = JSON.parse(compact)
    checksum += parsed["id"] + parsed["value"] + parsed["group"]
    checksum += parsed["items"][1]
    if i < 20
      pretty = JSON.pretty_generate(parsed)
      reparsed = JSON.parse(pretty)
      checksum += reparsed["items"][2]
    end
    i += 1
  end

  count = 0
  File.foreach(JSON_PATH) do |line|
    row = JSON.parse(line)
    checksum += row["id"] * 3 + row["value"] - row["group"]
    count += 1
  end
  checksum + count
end

puts main
