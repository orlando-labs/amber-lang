package bench.polyglot.map_words

export main

def main():
  m = {}
  i = 0
  while i < 30000:
    key = "w" + ((i * i + i / 3) % 2000).to_str()
    if m.include?(key):
      m[key] = m[key] + 1
    else:
      m[key] = 1
    i = i + 1
  checksum = 0
  m.each |k, v|:
    checksum = checksum + v * k.length()
  hits = 0
  j = 0
  while j < 10000:
    probe = "w" + ((j * 7) % 3000).to_str()
    if m.include?(probe):
      hits = hits + m[probe]
    j = j + 1
  checksum + hits + m.count()

main()
