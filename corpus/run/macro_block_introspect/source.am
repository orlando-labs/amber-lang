package conformance.run

# Calling a macro block returns its recursively expanded Ast.Block. Raw body
# introspection remains available when the macro intentionally wants statements.
macro def count_stmts(&blk):
  %expanded = blk()
  #{expanded.body.size}

def probe():
  n = count_stmts():
    a = 1
    b = 2
  n * 21
