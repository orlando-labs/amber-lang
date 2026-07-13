from sync import Mutex

class SharedState:
  def init():
    @mutex = Mutex.new()
    @active = 0
    @refreshing = false
    @refresh_task = null

  def started():
    @mutex.synchronize:
      @active += 1
      unless @refreshing:
        @refreshing = true
        @refresh_task = task.spawn:
          self.refresh()

  def finished():
    @mutex.synchronize:
      @active -= 1
      if @active == 0:
        @refreshing = false
        if @refresh_task:
          @refresh_task.cancel()
          @refresh_task = null

  def refresh():
    try:
      while true:
        task.sleep(0.001)
        keep_running = @mutex.synchronize:
          @refreshing
        unless keep_running:
          break
    rescue CancelledError:
      null

  def active():
    @mutex.synchronize:
      @active

def main():
  state = SharedState()
  first = task.spawn:
    state.started()
    task.sleep(0.01)
    state.finished()
  second = task.spawn:
    state.started()
    task.sleep(0.01)
    state.finished()
  first.wait()
  second.wait()
  state.active()
