def probe():
  decorated = ["a", "b"].map |x|: x + "!"
  joined = decorated[0] + decorated[1]
  nested = [["x"]].map |inner|:
    inner.map |x|: x + "?"
  joined + nested[0][0]
