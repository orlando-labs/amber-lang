def probe():
  hits = 0
  try:
    {a: 1}[:missing]
  rescue KeyError:
    hits = hits + 1
  try:
    {a: 1}.fetch(:missing)
  rescue KeyError:
    hits = hits + 10
  try:
    [1, 2][9]
  rescue IndexError:
    hits = hits + 100
  try:
    "ab"[9]
  rescue IndexError:
    hits = hits + 1000
  after = {a: 1}[?:missing] == null and [1, 2][?9] == null
  if hits == 1111 and after:
    42
  else:
    0
