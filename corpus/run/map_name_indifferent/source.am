package conformance.run

# Ordinary Map / HashMap are name-indifferent (spec v20.7/v20.8): a Symbol key
# and a Str key with the same text are the same key for lookup, dedup, and
# named-key pattern matching, while each entry keeps its original key Value.
def probe():
  sym = {user_id: 1}
  str = {"user_id": 2, 1: 0}
  dup = {"k": 9, k: 7}
  matched = 0
  case str:
    when {user_id: uid}:
      matched = uid
    else:
      matched = -1
  if sym["user_id"] == 1 and sym[:user_id] == 1 and str[:user_id] == 2 and str["user_id"] == 2 and matched == 2 and dup.count() == 1 and dup[:k] == 7 and dup["k"] == 7:
    42
  else:
    0
