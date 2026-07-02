package native.text_value_stdlib_core

export main

def main():
  score = 0
  bytes = Bytes.new("hello")
  if Base64.encode(bytes) == "aGVsbG8=":
    score = score + 1
  if Base64.encode(bytes, padding: false) == "aGVsbG8":
    score = score + 1
  if Base64.decode("aGVsbG8=").to_str() == "hello":
    score = score + 1
  if Base64.decode("aG Vs\nbG8=", mode: :lenient).to_str() == "hello":
    score = score + 1
  url_bytes = Hex.decode("fbff")
  if Base64Url.encode(url_bytes) == "-_8":
    score = score + 1
  if Base64Url.encode(url_bytes, padding: true) == "-_8=":
    score = score + 1
  if Base64Url.decode("-_8").hex() == "fbff":
    score = score + 1
  if Hex.encode(bytes) == "68656c6c6f":
    score = score + 1
  if Hex.decode("68 65:6C-6c 6F", mode: :lenient).to_str() == "hello":
    score = score + 1

  data = Bytes.new("abc")
  key = Hex.decode("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")
  if Digest.crc32(data).hex() == "352441c2":
    score = score + 1
  if Digest.md5(data).hex() == "900150983cd24fb0d6963f7d28e17f72":
    score = score + 1
  if Digest.sha1(data).hex() == "a9993e364706816aba3e25717850c26c9cd0d89d":
    score = score + 1
  if Digest.sha256(data).hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad":
    score = score + 1
  if Digest.hmac_sha256(key, Bytes.new("Hi There")).hex() == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7":
    score = score + 1
  if Digest.streebog256(data).hex() == "4e2919cf137ed41ec4fb6270c61826cc4fffb660341e0af3688cd0626d23b481":
    score = score + 1
  if Digest.streebog512(data).hex() == "28156e28317da7c98f4fe2bed6b542d0dab85bb224445fcedaf75d46e26d7eb8d5997f3e0915dd6b7f0aab08d9c8beb0d8c64bae2ab8b3c8c6bc53b3bf0db728":
    score = score + 1

  uuid = Uuid.parse("550e8400-e29b-41d4-a716-446655440000")
  if Uuid === uuid and UUID === uuid and uuid.version == 4:
    score = score + 1
  if uuid.to_str == "550e8400-e29b-41d4-a716-446655440000" and uuid.inspect == uuid.to_str:
    score = score + 1
  if uuid.to_json == "\"550e8400-e29b-41d4-a716-446655440000\"":
    score = score + 1

  u = Url.parse("HTTP://Example.COM/a?x=1&x=2&space=a+b")
  q = u["query_map"]
  if u["scheme"] == "http" and u["host"] == "example.com" and q["x"][1] == "2":
    score = score + 1
  if Url.build(u) == "http://example.com/a?x=1&x=2&space=a+b":
    score = score + 1
  if Url.build_query(q) == "x[]=1&x[]=2&space=a+b":
    score = score + 1
  encoded = Url.percent_encode("a b/!")
  if encoded == "a%20b%2F%21" and Url.percent_decode(encoded) == "a b/!":
    score = score + 1
  parsed = Url.parse_query("q=a+b&q=x%2Fy")
  nested = Url.parse_query("?map[a]=1&map[b][x]=2&c[]=1&c[]=2")
  if parsed["q"][1] == "x/y" and nested["map"]["b"]["x"] == "2":
    score = score + 1
  if Url.build_query(parsed) == "q[]=a+b&q[]=x%2Fy":
    score = score + 1
  score
