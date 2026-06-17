package bench.vm.map_symbol_lookup

export main

def main():
  row = {id: 17, value: 31, group: 5, count: 7}
  checksum = 0
  i = 0
  while i < 300000:
    checksum = checksum + row[:id] + row[:value] - row[:group] + row[:count]
    i = i + 1
  checksum

main()

