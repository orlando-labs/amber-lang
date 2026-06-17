package bench.vm.jsonl_noop_stream

export main

def main():
  total = 0
  i = 0
  while i < 5:
    count = Json.stream_parse_file("bench/polyglot/build/json/events.jsonl", jsonl: true) |row|:
      0
    total = total + count
    i = i + 1
  total

main()

