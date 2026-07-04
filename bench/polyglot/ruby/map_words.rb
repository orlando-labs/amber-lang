def main_workload
  m = {}
  i = 0
  while i < 30000
    key = "w" + ((i * i + i / 3) % 2000).to_s
    if m.key?(key)
      m[key] = m[key] + 1
    else
      m[key] = 1
    end
    i += 1
  end
  checksum = 0
  m.each do |k, v|
    checksum += v * k.length
  end
  hits = 0
  j = 0
  while j < 10000
    probe = "w" + ((j * 7) % 3000).to_s
    hits += m[probe] if m.key?(probe)
    j += 1
  end
  checksum + hits + m.length
end

puts main_workload
