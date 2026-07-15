package bench.vm.task_local_get

import task

export main

# Hot-path microbenchmark for task-local lookup. The slot is allocated and set
# once; the loop measures repeated reads from the current logical task context.
def main():
  current_request = task.local(inherit: false)
  current_request.set!(17)
  checksum = 0
  i = 0
  while i < 300000:
    checksum = checksum + current_request.get()
    i = i + 1
  checksum

main()
