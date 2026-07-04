package conformance.run

# Template bodies are hygienic like explicit quotes: `tmp` introduced by the
# template cannot clobber the caller's `tmp`.
macro def set_tmp(x):
  tmp = #{x}

def probe():
  tmp = 100
  set_tmp(5)
  tmp
