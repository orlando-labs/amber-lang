package conformance.run

# M4 template surface: a `macro def` body is emitted code by default (an
# implicit quote); `#{expr}` punches unquote holes. A bare `#` inside the
# body is still a comment.
macro def twice(x):
  # emitted once, with the argument AST spliced twice
  #{x} + #{x}

def probe():
  twice(21)
