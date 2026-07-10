package physics.core
import math.constants as consts
from lab.units import Meter as Метр, Second
export Particle, Метр

mixin Timestamped:
  def touch!()

class Particle < Entity:
  include Timestamped
  extend Serializable
  class_method def find(id):
    id
  def init(@масса, α = 1)
