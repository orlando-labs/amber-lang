package bench.polyglot.string_ops

export main

def main():
  checksum = 0
  i = 0
  while i < 4000:
    token = "user-" + i.to_str() + "-record"
    upper = token.upcase()
    lower = upper.downcase()
    line = lower + "|" + token + "|segment-" + (i % 97).to_str()
    parts = line.split("|")
    rebuilt = parts[0] + ";" + parts[1] + ";" + parts[2]
    replaced = rebuilt.replace("user-", "member-")
    padded = "  " + replaced + "  "
    trimmed = padded.trim()
    checksum = checksum + trimmed.length() + parts.count()
    if trimmed.contains?("member-"):
      checksum = checksum + 3
    if trimmed.starts_with?("member-"):
      checksum = checksum + 7
    if trimmed.ends_with?("7"):
      checksum = checksum + 11
    if i % 5 == 0:
      checksum = checksum + line.replace("e", "E").length()
    i = i + 1
  checksum

main()
