package conformance.run

# StrictMap / StrictHashMap preserve exact-key semantics (spec v20.7/v20.8):
# Symbol and Str keys stay distinct, name-key duplicates are not collapsed, and
# named-key patterns match only exact Symbol keys (so the Str-keyed `stronly`
# falls to the else branch).
def probe():
  s = StrictMap{name: 1, "name": 2, 1: 0}
  symonly = StrictMap{user_id: 5}
  stronly = StrictMap{"user_id": 6, 1: 0}
  matched = 0
  case stronly:
    when {user_id: id}:
      matched = 1
    else:
      matched = -1
  if s.count() == 3 and symonly[:user_id] == 5 and symonly.has_key?("user_id") == false and stronly["user_id"] == 6 and stronly.has_key?(:user_id) == false and matched == -1:
    42
  else:
    0
