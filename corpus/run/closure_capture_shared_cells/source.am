package conformance.run

x = 1

def read_x():
  x

before = read_x()
x = 2
after = read_x()

def loop_blocks():
  i = 0
  total = 0
  while i < 3:
    vals = [0].map |unused|: i * 10
    total = total + vals[0]
    i = i + 1
  total

def probe():
  "#{before}:#{after}:#{read_x()}:#{loop_blocks()}"
