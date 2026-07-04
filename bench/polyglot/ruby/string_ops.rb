def main_workload
  checksum = 0
  i = 0
  while i < 4000
    token = "user-" + i.to_s + "-record"
    upper = token.upcase
    lower = upper.downcase
    line = lower + "|" + token + "|segment-" + (i % 97).to_s
    parts = line.split("|")
    rebuilt = parts[0] + ";" + parts[1] + ";" + parts[2]
    replaced = rebuilt.gsub("user-", "member-")
    padded = "  " + replaced + "  "
    trimmed = padded.strip
    checksum += trimmed.length + parts.length
    checksum += 3 if trimmed.include?("member-")
    checksum += 7 if trimmed.start_with?("member-")
    checksum += 11 if trimmed.end_with?("7")
    checksum += line.gsub("e", "E").length if i % 5 == 0
    i += 1
  end
  checksum
end

puts main_workload
