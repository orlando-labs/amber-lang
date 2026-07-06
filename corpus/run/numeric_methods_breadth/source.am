def probe():
  ok_abs = (0 - 5).abs() == 5 and 5.abs == 5 and (0 - 2.5).abs() == 2.5
  ok_parity = 4.even? and 4.odd? == false and 3.odd? and 3.even? == false
  ok_divmod = 7.divmod(2) == [3, 1] and (0 - 7).divmod(2) == [-4, 1] and 7.5.divmod(2) == [3.0, 1.5]
  ok_gcd = 12.gcd(18) == 6 and 0.gcd(0) == 0 and (0 - 12).gcd(18) == 6
  ok_lcm = 4.lcm(6) == 12 and 0.lcm(5) == 0
  ok_clamp = 5.clamp(1, 3) == 3 and 0.clamp(1, 3) == 1 and 2.clamp(1, 3) == 2 and 2.5.clamp(1, 2) == 2
  ok_upto = 1.upto(4) == [1, 2, 3, 4] and 3.upto(1) == []
  ok_downto = 3.downto(1) == [3, 2, 1]
  acc = 0
  1.upto(3) |i|:
    acc = acc + i
  ok_upto_block = acc == 6
  rescued = false
  try:
    7.divmod(0)
  rescue ZeroDivisionError:
    rescued = true
  if ok_abs and ok_parity and ok_divmod and ok_gcd and ok_lcm and ok_clamp and ok_upto and ok_downto and ok_upto_block and rescued:
    42
  else:
    0
