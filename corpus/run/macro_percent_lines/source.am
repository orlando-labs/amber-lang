package conformance.run

# §6 %-control lines: a line-leading % in a macro body is a compile-time
# statement (runs at expansion, not emitted); template lines inside a %-loop
# are emitted once per iteration, and %-locals feed later template splices.
macro def push_doubled(target, *exprs):
  %count = exprs.count()
  %exprs.each |e|:
    #{target}.push!(#{e} * 2)
  #{target}.push!(#{Ast.lift(count)})

# %if — conditional emission; the template emits exactly one node here, so
# the macro is usable in expression position.
macro def maybe_double(flag, x):
  %if flag.kind == "AstLiteral":
    #{x} * 2

def probe():
  xs = []
  push_doubled(xs, 1, 2, 3)
  y = maybe_double(1, 20 + 1)
  ok_push = xs == [2, 4, 6, 3]
  ok_expr = y == 42
  if ok_push and ok_expr:
    42
  else:
    0
