package conformance.run
import task
import net
from io import ByteBuffer

def probe():
  listener = net.tcp.listen("127.0.0.1", 0)
  port = listener.local_endpoint().port()
  client = net.tcp.connect("127.0.0.1", port)
  client.write_all!("ping".bytes())
  server = listener.accept!()
  worker = task.spawn:
    server.adopt!()
    buf = ByteBuffer.new(4)
    server.read!(buf)
    payload = buf.bytes().to_str()
    server.close!()
    payload
  out = worker.wait()
  client.close!()
  listener.close!()
  out
