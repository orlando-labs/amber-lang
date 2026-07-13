class CallShape:
  def init(@base = 1, @scale: 2, **options):
    @bonus = options[?:bonus] or 0

  def total(head, *middle, tail, offset: 1, &blk):
    value = (@base + head + middle.count + tail + offset + @bonus) * @scale
    if blk:
      blk(value)
    else:
      value

  def function_shape(head, *middle, tail, scale: 2, **options):
    head + middle.count * 10 + tail + scale + (options[?:bonus] or 0)

  def positional_default(value = 7):
    value

def main():
  shape = CallShape(*[], **{scale: 3, "bonus": 4})
  method_value = shape.total(2, 7, 8, 9, offset: 5) |value|:
    value + 1
  function_value = shape.function_shape(1, *[2, 3], 4, **{scale: 5, "bonus": 6})
  method_value + function_value + shape.positional_default() + shape.positional_default(2)
