def stringify(value):
  value.to_str

def text_of(value):
  when Str:
    value
  else:
    stringify(value)

def main():
  text_of("x").length + text_of(12).length
