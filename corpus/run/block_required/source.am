package conformance.run

# A typed (non-null) block parameter `&blk as Fn[...]` is REQUIRED: a call that
# supplies no block raises ArgumentError at entry (AMB_BLOCK_REQUIRED), via the
# RequireBlock prologue guard — for module-level def closures as well as class
# methods. A bare `&blk` stays optional (binds null). The error is rescuable.
def needs_block(&blk as Fn[Int -> Int]):
  blk(41)

def probe():
  with_block = needs_block() |x|: x + 1
  caught = try:
    needs_block()
    "no error"
  rescue ArgumentError |e|:
    "caught"
  "#{with_block}:#{caught}"
