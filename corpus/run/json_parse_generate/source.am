package conformance.run

# Json.parse -> name-indifferent Map (dual symbol/string access + named-key
# pattern matching, spec §8), round-trips through Json.generate, and types
# numbers as Int vs Float.
def probe():
  p = Json.parse("{\"user_id\": 42, \"items\": [1, 2, 3], \"name\": \"Ada\", \"ratio\": 0.5}")
  round = Json.parse(Json.generate(p))
  matched = 0
  case p:
    when {user_id: id, name: n}:
      matched = id
    else:
      matched = -1
  if p[:user_id] == 42 and p["name"] == "Ada" and p[:items][1] == 2 and p["ratio"] == 0.5 and round[:user_id] == 42 and matched == 42 and Json.generate([1, 2]) == "[1,2]":
    42
  else:
    0
