package conformance.run

import task

# Regression: the task.spawn block body references the `task` module import
# directly. The import alias must not be captured as an upvalue (doing so
# emitted a MAKE_CLOSURE reading an uninitialized register -> verifier BC1313).
def probe():
  h = task.spawn: task.sleep(0)
  h.wait()
  42
