class Callable:
  def init(@base)

  def call(value, offset: 0, &blk):
    result = @base + value + offset
    return blk(result) if blk
    result

def main():
  callable = Callable(3)
  return 0 unless callable
  return 1 unless callable.present?
  return 2 if callable == Callable(3)
  return 3 unless callable == callable
  callable(4, offset: 2) |value|:
    value * 2
