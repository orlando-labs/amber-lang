package conformance.run

# `;` is an explicit statement separator (lexed as a same-line newline). It is
# highly discouraged in normal code, where line breaks are idiomatic; it exists
# for ad-hoc one-liners. This fixture pins that the separator parses and runs.
def probe():
  a = 1; b = 2; c = a + b
  parts = []; parts.push!(a); parts.push!(c)
  "#{a}:#{b}:#{c}:#{parts[0]}:#{parts[1]}"
