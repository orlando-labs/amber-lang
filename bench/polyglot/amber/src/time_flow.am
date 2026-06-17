package bench.polyglot.time_flow

export main

def main():
  checksum = 0
  i = 0
  anchor = Time.parse("2026-06-17T03:04:05.123456789+03:00")

  while i < 12000:
    base = Time.utc(
      2020 + (i % 7),
      1 + (i % 12),
      25 + (i % 4),
      hour: i % 24,
      minute: (i * 7) % 60,
      second: (i * 11) % 60,
      nanosecond: (i * 1234567) % 1000000000
    )
    period = ((i % 15) + 1).months
    period = period + (i % 21).days
    period = period + (i % 3600).seconds
    period = period + ((i % 1000) * 1000).nanoseconds
    shifted = base + period
    elapsed = shifted - base
    epoch = Time.from_unix_ms(1700000000000 + (i * 37))
    tiny = Time.from_unix_ns(1000000000 + (i * 1000003) + 999)
    clamp = Time.utc(2024, 1, 31) + ((i % 3) + 1).months
    probe = 5.seconds + 1.day + anchor

    checksum = checksum + shifted.year * 37
    checksum = checksum + shifted.month * 31
    checksum = checksum + shifted.day * 29
    checksum = checksum + shifted.hour * 23
    checksum = checksum + shifted.minute * 19
    checksum = checksum + shifted.second * 17
    checksum = checksum + shifted.nanosecond % 1000003
    checksum = checksum + elapsed.total_nanoseconds % 1000003
    checksum = checksum + epoch.second + epoch.nanosecond % 1009
    checksum = checksum + tiny.unix_nanoseconds % 9973
    checksum = checksum + clamp.day * 13 + clamp.month
    checksum = checksum + probe.day + probe.second

    if i % 97 == 0:
      checksum = checksum + shifted.iso8601.length()

    if shifted > base:
      checksum = checksum + 7
    else:
      checksum = checksum + 3

    checksum = checksum % 2147483647
    i = i + 1

  checksum

main()
