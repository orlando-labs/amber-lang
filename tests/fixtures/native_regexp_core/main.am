package native.regexp_core

export main

def main():
  score = 0
  digits = r"\d+"
  grouped = Regexp.compile("([a-z]+)(\\d+)", flags: "i")
  m = grouped.match("ID42")

  if m and m[0] == "ID42" and m[1] == "ID" and m[2] == "42":
    score = score + 1
  if m.count() == 2 and m.captures()[0] == "ID" and m.captures()[1] == "42":
    score = score + 1
  if m.start == 0 and m.finish == 4 and m.text == "ID42" and m.source == "ID42":
    score = score + 1
  if Regexp === digits and Regexp === m and digits == Regexp.new("\\d+"):
    score = score + 1
  if (digits =~ "abc123")[0] == "123" and ("abc123" =~ digits)[0] == "123":
    score = score + 1
  if ("abc" !~ digits) and digits.match?("x9") and digits.full_match?("123"):
    score = score + 1
  if digits.source == "\\d+" and digits.to_str == "/\\d+/" and Regexp.escape("a+b[c]") == "a\\+b\\[c\\]":
    score = score + 1

  plain = "a12b34".replace(digits, "[$0]")
  limited = "1 2 3".replace(digits, "x", count: 2)
  blocky = "a12b3".replace(digits) |hit|:
    "<" + hit[0] + ">"
  if plain == "a[12]b[34]" and limited == "x x 3" and blocky == "a<12>b<3>":
    score = score + 1

  if score == 8:
    42
  else:
    score
