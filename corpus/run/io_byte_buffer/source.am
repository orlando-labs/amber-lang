package conformance.run
from io import ByteBuffer

# `from io import ByteBuffer` resolves to the io.ByteBuffer native type.
# put_all! appends the raw octets of a string; bytes() snapshots what was
# written, so the buffer round-trips the payload while reporting its capacity.
def probe():
  b = ByteBuffer.new(8)
  b.put_all!("hi".bytes())
  "#{b.capacity()}:#{b.count()}:#{b.bytes().to_str()}"
