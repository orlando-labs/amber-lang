class NativeExceptionProbe:
  def explode():
    raise "boom"

  def rescued():
    value = 2
    try:
      explode
    rescue:
      value = value + 3
    value

  def ensured():
    value = 1
    try:
      try:
        explode
      ensure:
        value = 4
    rescue:
      value = value + 3
    value

  def built_in():
    try:
      raise ArgumentError("bad")
    rescue ArgumentError |error|:
      if error.message == "bad":
        8
      else:
        0

  def result_value():
    mapped = Ok(3).map: _1 + 1
    recovered = Err(2).or_else: _1 + 3
    fallback = Err("x").or(6)
    caught = 0
    try:
      Err("boom").or_raise
    rescue:
      caught = 1
    if mapped.ok? and mapped.value == 4 and recovered == 5 and fallback == 6 and caught == 1 and Err(0).error == 0:
      9
    else:
      0

  def result():
    rescued * 10 + ensured + built_in + result_value

def main():
  NativeExceptionProbe().result
