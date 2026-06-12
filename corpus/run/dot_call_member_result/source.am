package conformance.run

class Adder:
  def call(x):
    x + 1

class Factory:
  prop provider: Adder()

  def make():
    Adder()

def probe():
  factory = Factory()
  factory.provider.(20) + factory.make.(20)
