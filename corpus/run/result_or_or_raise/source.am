package conformance.run

# Value-based Result[T,E]: Ok/Err construction, `.or` (default on Err), and the
# `.or_raise` bridge (Ok unwraps; Err re-raises through the exception path so an
# enclosing `rescue` catches it).
def probe():
  parts = []
  parts = parts + [Ok(42).or(0)]
  parts = parts + [Err("e").or(99)]
  parts = parts + [Ok(7).or_raise]
  caught = "no"
  try:
    Err("boom").or_raise
  rescue |e|:
    caught = "yes"
  parts = parts + [caught]
  parts.to_str()
