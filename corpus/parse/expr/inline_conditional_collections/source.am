{
  status: if ready? then :ready else :idle,
  flags: [
    :base,
    :extra if enabled?,
    :legacy unless modern?,
  ],
  permissions: {
    :read,
    :write if can_write?,
  },
  mode: :debug if debug?,
}
