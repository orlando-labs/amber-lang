def probe():
  ok_upcased = "straße".upcased() == "STRAßE" and "Привет".upcased() == "ПРИВЕТ" and "ab".upcase() == "AB"
  ok_downcased = "ΣΟΦΌΣ".downcased() == "σοφόσ" and "Ёлка".downcased() == "ёлка" and "AB".downcase() == "ab"
  ok_capitalized = "hello world".capitalized() == "Hello world" and "ПРИВЕТ".capitalized() == "Привет"
  ok_titlecased = "hello wide world".titlecased() == "Hello Wide World"
  ok_swapcased = "Hello".swapcased() == "hELLO"
  ok_humanized = "user_id".humanized() == "User id"
  ok_underscored = "HTTPServer".underscored() == "http_server" and "userId".underscored() == "user_id" and "a-b".underscored() == "a_b"
  ok_camelized = "user_id".camelized() == "UserId"
  ok_dasherized = "a_b".dasherized() == "a-b"
  ok_reversed = "abc".reversed() == "cba"
  if ok_upcased and ok_downcased and ok_capitalized and ok_titlecased and ok_swapcased and ok_humanized and ok_underscored and ok_camelized and ok_dasherized and ok_reversed:
    42
  else:
    0
