def main():
  data = Bytes.new("abc")
  key = Hex.decode("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")
  s256 = "4e2919cf137ed41ec4fb6270c61826cc4fffb660341e0af3688cd0626d23b481"
  s512 = "28156e28317da7c98f4fe2bed6b542d0dab85bb224445fcedaf75d46e26d7eb8d5997f3e0915dd6b7f0aab08d9c8beb0d8c64bae2ab8b3c8c6bc53b3bf0db728"
  ok = Digest.crc32(data).hex() == "352441c2"
  ok = ok and Digest.md5(data).hex() == "900150983cd24fb0d6963f7d28e17f72"
  ok = ok and Digest.sha1(data).hex() == "a9993e364706816aba3e25717850c26c9cd0d89d"
  ok = ok and Digest.sha256(data).hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
  ok = ok and Digest.hmac_sha256(key, Bytes.new("Hi There")).hex() == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
  ok = ok and Digest.streebog256(data).hex() == s256
  ok = ok and Digest.gost256(data).hex() == s256
  ok = ok and Digest.гост256(data).hex() == s256
  ok = ok and Digest.стрибог256(data).hex() == s256
  ok = ok and Digest.streebog512(data).hex() == s512
  ok = ok and Digest.gost512(data).hex() == s512
  ok = ok and Digest.гост512(data).hex() == s512
  ok = ok and Digest.стрибог512(data).hex() == s512
  if ok:
    42
  else:
    0
