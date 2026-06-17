package bench.vm.json_integer_parse

export main

def main():
  text = "{\"a\":123456789,\"b\":-987654321,\"c\":2147483647,\"d\":0,\"items\":[1,2,3,4,5,6,7,8]}"
  checksum = 0
  i = 0
  while i < 12000:
    row = Json.parse(text)
    checksum = checksum + row[:a] + row[:b] + row[:c] + row[:items][5]
    i = i + 1
  checksum

main()

