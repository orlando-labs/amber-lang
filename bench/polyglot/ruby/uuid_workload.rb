#!/usr/bin/env ruby

require "securerandom"

ROUNDS = 5_000
FIXED = "550e8400-e29b-41d4-a716-446655440000"

class UuidValue
  attr_reader :bytes

  def initialize(bytes)
    raise ArgumentError, "UUID must contain 16 bytes" unless bytes.bytesize == 16

    @bytes = bytes.b
  end

  def self.v4
    bytes = SecureRandom.random_bytes(16).bytes
    bytes[6] = (bytes[6] & 0x0f) | 0x40
    bytes[8] = (bytes[8] & 0x3f) | 0x80
    new(bytes.pack("C*"))
  end

  def self.v7
    milliseconds = Process.clock_gettime(Process::CLOCK_REALTIME, :millisecond)
    bytes = SecureRandom.random_bytes(16).bytes
    6.times do |index|
      bytes[index] = (milliseconds >> ((5 - index) * 8)) & 0xff
    end
    bytes[6] = (bytes[6] & 0x0f) | 0x70
    bytes[8] = (bytes[8] & 0x3f) | 0x80
    new(bytes.pack("C*"))
  end

  def self.parse(text)
    normalized = text.delete("-")
    raise ArgumentError, "invalid UUID" unless normalized.match?(/\A[0-9a-fA-F]{32}\z/)

    new([normalized].pack("H*"))
  end

  def to_s
    hex = @bytes.unpack1("H*")
    "#{hex[0, 8]}-#{hex[8, 4]}-#{hex[12, 4]}-#{hex[16, 4]}-#{hex[20, 12]}"
  end

  alias inspect to_s

  def to_json
    "\"#{self}\""
  end

  def version
    (@bytes.getbyte(6) >> 4) & 0x0f
  end

  def ==(other)
    other.is_a?(UuidValue) && @bytes == other.bytes
  end
end

def secure_random_uuid
  UuidValue.v4
end

checksum = 0

ROUNDS.times do
  v4 = UuidValue.v4
  v4_text = v4.to_s
  parsed_v4 = UuidValue.parse(v4_text)

  v7 = UuidValue.v7
  v7_text = v7.inspect
  parsed_v7 = UuidValue.parse(v7_text)

  delegated = secure_random_uuid
  delegated_json = delegated.to_json

  fixed = UuidValue.parse(FIXED.upcase)
  fixed_text = fixed.to_s

  checksum += v4.version
  checksum += v7.version
  checksum += delegated.version
  checksum += v4_text.bytesize
  checksum += v7_text.bytesize
  checksum += delegated_json.bytesize

  checksum += 11 if v4 == parsed_v4
  checksum += 13 if v7 == parsed_v7
  checksum += 17 if v4.is_a?(UuidValue) && v7.is_a?(UuidValue)
  checksum += 19 if fixed.version == 4 && fixed_text == FIXED
  checksum += 23 if v4_text.include?("-") && v7_text.include?("-")
end

puts checksum
