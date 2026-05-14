def classify(shape, x):
  case shape:
    when whole as (Point(^x, y) | Rect(x: ^x, y: y)):
      y
    when {a:, **rest}:
      rest
    when pattern(route("/users/:id")) with {id:, **null}:
      id
    else:
      shape
