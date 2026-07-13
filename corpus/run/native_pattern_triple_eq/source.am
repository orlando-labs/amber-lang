class PatternParent:
  def marker():
    1

class PatternChild < PatternParent:
  def marker():
    2

class PatternOther:
  def marker():
    3

def parent_score(value):
  case value:
    when PatternParent:
      10
    else:
      100

def exact_score(value):
  case value:
    when PatternChild:
      1
    when PatternParent:
      2
    else:
      3

def class_score(value):
  case value:
    when PatternParent:
      5
    else:
      50

def main():
  inherited = parent_score(PatternChild())
  child = exact_score(PatternChild())
  parent = exact_score(PatternParent())
  other = exact_score(PatternOther())
  class_object = class_score(PatternParent)
  inherited + child + parent + other + class_object
