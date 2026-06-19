package conformance.run

def probe():
  parser = ArgParser(cmdline: ["--count", "bad"])
  parser.arg("--count", type: Int)
  result = parser.try_parse()
  error = result.error()
  bridged = false
  try:
    result.or_raise
  rescue ArgParser.InvalidValue |caught|:
    bridged = caught.option == "--count" and caught.value == "bad"
  constructed = ArgParser.InvalidValue(
    "manual",
    option: "--manual",
    value: "bad"
  )
  if result.err?() and ArgParser.InvalidValue === error and ArgParser.ParseError === error and bridged and ArgParser.InvalidValue === constructed and (ArgParser.UnknownOption === constructed) == false:
    42
  else:
    0
