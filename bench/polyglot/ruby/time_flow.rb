#!/usr/bin/env ruby

NANOS_PER_SECOND = 1_000_000_000

class Period
  attr_reader :months, :days, :nanoseconds

  def initialize(months = 0, days = 0, nanoseconds = 0)
    @months = months
    @days = days
    @nanoseconds = nanoseconds
  end

  def +(other)
    if other.is_a?(Instant)
      other + self
    else
      Period.new(
        @months + other.months,
        @days + other.days,
        @nanoseconds + other.nanoseconds
      )
    end
  end

  def total_nanoseconds
    raise "calendar period has no fixed total" unless @months == 0

    @days * 86_400 * NANOS_PER_SECOND + @nanoseconds
  end
end

class Instant
  include Comparable

  attr_reader :seconds, :nanosecond

  def initialize(seconds, nanosecond = 0)
    @seconds = seconds
    @nanosecond = nanosecond
  end

  def self.utc(year, month, day, hour = 0, minute = 0, second = 0,
               nanosecond = 0)
    Instant.new(Time.utc(year, month, day, hour, minute, second).to_i,
                nanosecond)
  end

  def self.from_unix_ms(milliseconds)
    seconds, millis = milliseconds.divmod(1000)
    Instant.new(seconds, millis * 1_000_000)
  end

  def self.from_unix_ns(nanoseconds)
    seconds, nanos = nanoseconds.divmod(NANOS_PER_SECOND)
    Instant.new(seconds, nanos)
  end

  def self.parse_iso8601(text)
    prefix = text[0...-6]
    offset = text[-6..]
    sign = offset[0] == '+' ? 1 : -1
    offset_seconds = sign * (offset[1, 2].to_i * 3600 + offset[4, 2].to_i * 60)
    head, frac = prefix.split('.')
    parts = head.scan(/\d+/).map(&:to_i)
    nanos = (frac + '000000000')[0, 9].to_i
    local = Time.utc(parts[0], parts[1], parts[2], parts[3], parts[4], parts[5])
    Instant.new(local.to_i - offset_seconds, nanos)
  end

  def fields
    Time.at(@seconds).utc
  end

  def year
    fields.year
  end

  def month
    fields.month
  end

  def day
    fields.day
  end

  def hour
    fields.hour
  end

  def minute
    fields.min
  end

  def second
    fields.sec
  end

  def iso8601
    text = fields.strftime('%Y-%m-%dT%H:%M:%S')
    if @nanosecond != 0
      frac = format('%09d', @nanosecond).sub(/0+$/, '')
      text += ".#{frac}"
    end
    text + 'Z'
  end

  def unix_nanoseconds
    @seconds * NANOS_PER_SECOND + @nanosecond
  end

  def +(period)
    base = fields
    total_month = base.year * 12 + (base.month - 1) + period.months
    year, month0 = total_month.divmod(12)
    month = month0 + 1
    day = [base.day, days_in_month(year, month)].min
    shifted = Time.utc(year, month, day, base.hour, base.min, base.sec)
    shifted += period.days * 86_400
    seconds_delta, nanos = (@nanosecond + period.nanoseconds).divmod(
      NANOS_PER_SECOND
    )
    shifted += seconds_delta
    Instant.new(shifted.to_i, nanos)
  end

  def -(other)
    if other.is_a?(Period)
      self + Period.new(-other.months, -other.days, -other.nanoseconds)
    else
      Period.new(0, 0, unix_nanoseconds - other.unix_nanoseconds)
    end
  end

  def <=>(other)
    [@seconds, @nanosecond] <=> [other.seconds, other.nanosecond]
  end
end

def days_in_month(year, month)
  if month == 2
    return ((year % 4).zero? && (!(year % 100).zero? || (year % 400).zero?)) ? 29 : 28
  end
  [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][month - 1]
end

def months(value)
  Period.new(value, 0, 0)
end

def days(value)
  Period.new(0, value, 0)
end

def seconds(value)
  Period.new(0, 0, value * NANOS_PER_SECOND)
end

def nanoseconds(value)
  Period.new(0, 0, value)
end

def main
  checksum = 0
  anchor = Instant.parse_iso8601('2026-06-17T03:04:05.123456789+03:00')

  12_000.times do |i|
    base = Instant.utc(
      2020 + (i % 7),
      1 + (i % 12),
      25 + (i % 4),
      i % 24,
      (i * 7) % 60,
      (i * 11) % 60,
      (i * 1_234_567) % 1_000_000_000
    )
    period = months((i % 15) + 1)
    period += days(i % 21)
    period += seconds(i % 3600)
    period += nanoseconds((i % 1000) * 1000)
    shifted = base + period
    elapsed = shifted - base
    epoch = Instant.from_unix_ms(1_700_000_000_000 + (i * 37))
    tiny = Instant.from_unix_ns(1_000_000_000 + (i * 1_000_003) + 999)
    clamp = Instant.utc(2024, 1, 31) + months((i % 3) + 1)
    probe = seconds(5) + days(1) + anchor

    checksum += shifted.year * 37
    checksum += shifted.month * 31
    checksum += shifted.day * 29
    checksum += shifted.hour * 23
    checksum += shifted.minute * 19
    checksum += shifted.second * 17
    checksum += shifted.nanosecond % 1_000_003
    checksum += elapsed.total_nanoseconds % 1_000_003
    checksum += epoch.second + epoch.nanosecond % 1009
    checksum += tiny.unix_nanoseconds % 9973
    checksum += clamp.day * 13 + clamp.month
    checksum += probe.day + probe.second
    checksum += shifted.iso8601.length if (i % 97).zero?
    checksum += shifted > base ? 7 : 3
    checksum %= 2_147_483_647
  end

  checksum
end

puts main
