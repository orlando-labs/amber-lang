package conformance.run

# Paren-less block-suffix trigger (§8.2): `name:` + INDENT passes the
# unevaluated block through the standard block channel. Calling it expands its
# nested macros and returns an Ast.Block; statement splicing flattens it.
macro def twice_do(&blk):
  #{blk()}
  #{blk()}

def probe():
  y = 0
  twice_do:
    y = y + 21
  y
