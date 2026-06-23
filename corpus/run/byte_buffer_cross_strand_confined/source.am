package conformance.run
import task
from io import ByteBuffer

# A non-shareable buffer is confined to the strand that created it. A task that
# touches it WITHOUT the explicit adopt! handoff fails deterministically with
# IsolationError, regardless of which worker runs the task -- the owner identity
# is namespace-tagged, so the entry strand and the task strand never collide.
def probe():
  buf = ByteBuffer.new(4)
  worker = task.spawn:
    buf.put!(120)
    "reached"
  worker.wait()
