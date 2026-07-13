def collection_score(value):
  case value:
    when Array:
      1
    when Tuple:
      2
    when Set:
      4
    when StrictMap:
      8
    when Map:
      16
    else:
      32

def main():
  score = collection_score([1])
  score += collection_score((1,))
  score += collection_score({1})
  score += collection_score(StrictMap{name: 1})
  score += collection_score({name: 1})
  case Map:
    when Map:
      score + 64
    else:
      score
