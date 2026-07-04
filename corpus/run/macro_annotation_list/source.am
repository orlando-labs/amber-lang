package conformance.run

# Annotation trigger (§8.3): a macro declared with one more parameter than
# the call passes receives the adjacent declaration below as its trailing
# argument, and may expand to a List of declarations.
macro def with_extra(d):
  q = quote:
    def extra():
      21
  return [d, q]

with_extra()
def base():
  21

def probe():
  base() + extra()
