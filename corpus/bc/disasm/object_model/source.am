package physics.core

export Timestamped, Particle

mixin Timestamped:
  def touch!()

class Particle < Entity:
  include Timestamped
  extend Serializable
  class_method def find(id):
    id
  def init(@масса, α = 1)
