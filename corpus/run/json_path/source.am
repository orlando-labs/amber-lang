package conformance.run

def probe():
  payload = Json.parse("{\"items\":[{\"id\":1},{\"id\":2},{\"id\":3}],\"kind\":\"demo\"}")
  ids = Json.paths(payload, "$.items[*].id")
  first = payload.path("$.items[*].id")
  stopped = payload.paths("$.items[*]", 0) |el, acc|:
    if el[:id] == 2:
      Json.stop(acc + 20)
    acc + el[:id]
  strict = StrictMap{name: 1, "name": 2}
  if first == 1 and ids.count() == 3 and ids[2] == 3 and stopped == 21 and strict.path("$.name") == 2:
    42
  else:
    0
