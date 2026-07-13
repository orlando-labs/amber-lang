def main():
  calls = 0
  values = {present: false, nullable: null}
  missing = values.get_or_set!(:missing, 7)
  present = values.get_or_set!(:present, 9)
  nullable = values.get_or_set!(:nullable, 10)
  lazy_first = values.get_or_set!(:lazy):
    calls += 1
    11
  lazy_second = values.get_or_set!(:lazy):
    calls += 10
    12
  values.get_or_set!(:group, []).push!(21)

  strict = StrictMap{name: 1}
  strict.get_or_set!("name", 2)

  score = missing + lazy_first + lazy_second + calls
  score += values[:group][0] + strict[:name] + strict["name"] + strict.count
  unless present:
    score += 3
  if nullable == null:
    score += 5
  score
