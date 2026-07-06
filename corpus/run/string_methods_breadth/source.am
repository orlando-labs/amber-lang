def probe():
  ok_repeat = "ab" * 3 == "ababab" and "x" * 0 == ""
  ok_just = "5".rjust(3, "0") == "005" and "ab".ljust(5, ".") == "ab..." and "abc".ljust(2) == "abc" and "x".rjust(3) == "  x"
  ok_index = "hello".index("ll") == 2 and "hello".index("l", 3) == 3 and "hello".index("z") == null and "hello".rindex("l") == 3
  ok_lines = "a\nb\r\nc".lines() == ["a", "b", "c"] and "a\n".lines() == ["a"]
  ok_count = "banana".count("an") == 2 and "aaa".count("aa") == 1
  ok_slice = "héllo"[1] == "é" and "hello"[0 - 1] == "o" and "hello".slice((1..3)) == "ell" and "héllo".length() == 5
  ok_trims = "  x  ".trimmed() == "x" and "  x  ".ltrimmed() == "x  " and "  x  ".rtrimmed() == "  x" and "  x ".lstripped() == "x " and " x  ".rstripped() == " x"
  ok_replaced = "a-b-c".replaced("-", "+") == "a+b+c"
  if ok_repeat and ok_just and ok_index and ok_lines and ok_count and ok_slice and ok_trims and ok_replaced:
    42
  else:
    0
