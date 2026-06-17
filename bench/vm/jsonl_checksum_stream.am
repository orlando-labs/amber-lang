package bench.vm.jsonl_checksum_stream

export main

def main():
  checksum = 0
  count = Json.stream_parse_file("bench/polyglot/build/json/events.jsonl", jsonl: true) |row|:
    checksum = checksum + row[:id] * 3 + row[:value] - row[:group]
  checksum + count

main()

