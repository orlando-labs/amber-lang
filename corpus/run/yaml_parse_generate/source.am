package conformance.run

# Yaml.parse -> name-indifferent Map (dual symbol/string access + named-key
# pattern matching, spec §8) over a block mapping/sequence document, round-trips
# through Yaml.generate, and types scalars per the core schema (Int vs Float vs
# Str vs Bool vs null).
def probe():
  cfg = Yaml.parse("name: Ada\nage: 42\nactive: true\nratio: 0.5\nscores:\n  - 10\n  - 20\nmeta:\n  role: admin\ntags: [a, b]\n")
  round = Yaml.parse(Yaml.generate(cfg))
  matched = 0
  case cfg:
    when {name: n, age: a}:
      matched = a
    else:
      matched = -1
  if cfg[:name] == "Ada" and cfg["age"] == 42 and cfg[:ratio] == 0.5 and cfg[:scores][1] == 20 and cfg[:active] == true and cfg[:meta][:role] == "admin" and cfg[:tags][0] == "a" and round[:name] == "Ada" and round[:scores][1] == 20 and matched == 42 and Yaml.generate([1, 2]) == "- 1\n- 2\n":
    42
  else:
    0
