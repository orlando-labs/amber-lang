u = Url.parse("HTTP://Example.COM/a?x=1&x=2&space=a+b")
q = u["query_map"]
built = Url.build(u)
encoded = Url.percent_encode("a b/!")
decoded = Url.percent_decode(encoded)
query = Url.build_query(q)
parsed = Url.parse_query("q=a+b&q=x%2Fy")
nested = Url.parse_query("?map[a]=1&map[b][x]=2&c[]=1&c[]=2")
round_query = Url.build_query(parsed)

if u["scheme"] == "http" and u["host"] == "example.com" and q["x"][1] == "2" and built == "http://example.com/a?x=1&x=2&space=a+b" and encoded == "a%20b%2F%21" and decoded == "a b/!" and query == "x[]=1&x[]=2&space=a+b" and parsed["q"][1] == "x/y" and nested["map"]["b"]["x"] == "2" and nested["c"][1] == "2" and round_query == "q[]=a+b&q[]=x%2Fy":
  42
else:
  0
