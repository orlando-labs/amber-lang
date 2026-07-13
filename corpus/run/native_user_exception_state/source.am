class ApplicationError < Exception:
  attr message

  def init(@message)

class ChildError < ApplicationError

def main():
  caught = 0
  try:
    raise ChildError("boom")
  rescue Exception |error|:
    if ApplicationError === error and error.message == "boom":
      caught = 21
  caught
