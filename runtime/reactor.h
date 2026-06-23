#pragma once

// RuntimeReactor: a single epoll/kqueue-backed readiness loop for the VM's
// cooperative async machine. A dedicated reactor thread owns the kqueue (macOS)
// / epoll (Linux) instance and a self-pipe used to wake it for control changes
// (new registrations, cancellation kicks, fd close, shutdown).
//
// Layer A (this revision): `wait()` blocks the calling worker thread on a
// per-waiter condition variable until the fd is actionable, the deadline
// passes, the task's cancel flag is observed, or the fd is closed. This
// replaces the old per-fd `::poll()` busy-loop in io.cpp (10ms re-poll cadence)
// with an event-driven wait and prompt, cross-thread cancellation/close
// wakeups.
//
// Layer B (cooperative suspension) reuses the same loop: instead of blocking a
// worker, a strand will register interest and yield its coroutine; the reactor
// thread completes the registration and the scheduler resumes the coroutine on
// another worker. The registration model below (one reactor thread, control via
// self-pipe wake) is shaped for that follow-on -- only the waiter's "how do I
// resume" differs (condvar today, coroutine resume later).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace amber::runtime {

enum class ReactorInterest { Read, Write };

enum class ReactorOutcome {
  // The fd is actionable: ready for the requested interest, or it reported
  // EOF/HUP/ERR. The caller should re-attempt the syscall, which yields the
  // precise errno (mirrors the old poll loop, which returned ok on
  // POLLERR/POLLHUP so the read/write surfaced the real error).
  Ready,
  // The supplied deadline elapsed before the fd became actionable.
  TimedOut,
  // The task's cancellation flag was observed set.
  Cancelled,
  // The fd was closed out from under the wait via notify_closed(), or could not
  // be registered because it was already invalid (EBADF).
  Closed,
  // The reactor backend reported an internal error registering/polling the fd.
  Error,
};

struct ReactorStats {
  std::uint64_t waits = 0;
  std::uint64_t ready = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t cancellations = 0;
  std::uint64_t closes = 0;
  std::uint64_t errors = 0;
  std::uint64_t kicks = 0;
  std::uint64_t loop_wakeups = 0;
  std::uint64_t current_waiters = 0;
  std::uint64_t max_waiters = 0;
};

class RuntimeReactor {
public:
  // Process-wide reactor. The thread starts lazily on first construction.
  static RuntimeReactor &instance();

  RuntimeReactor();
  RuntimeReactor(const RuntimeReactor &) = delete;
  RuntimeReactor &operator=(const RuntimeReactor &) = delete;
  ~RuntimeReactor();

  // Block until `fd` is actionable for `interest`, the optional `deadline`
  // passes, `*cancel_flag` is observed true (when non-null), or the fd is
  // closed via notify_closed(). Multiple strands may wait on the same fd
  // concurrently (e.g. a reader strand and a writer strand on one socket).
  ReactorOutcome
  wait(int fd, ReactorInterest interest,
       std::optional<std::chrono::steady_clock::time_point> deadline,
       const std::atomic<bool> *cancel_flag);

  // Asynchronous (non-blocking) registration for the cooperative scheduler
  // (Layer B). Instead of blocking the caller, register interest and have the
  // reactor invoke `completion(outcome)` from the reactor thread when the wait
  // resolves (readiness/timeout/cancel/close). This is the mechanism a parked
  // strand uses: the completion marks the strand runnable and wakes a worker,
  // and the worker re-enters the interpreter to retry the blocking op.
  //
  // The completion runs OUTSIDE the reactor lock, but still on the reactor
  // thread, so it must be cheap and non-blocking: it must not run VM/user code
  // and must not call back into the reactor synchronously. Cancellation of a
  // parked strand is delivered through `cancel_flag` + kick(), same as wait().
  void wait_async(int fd, ReactorInterest interest,
                  std::optional<std::chrono::steady_clock::time_point> deadline,
                  const std::atomic<bool> *cancel_flag,
                  std::function<void(ReactorOutcome)> completion);

  // Wake any waiters on `fd` with Closed. Call this from a resource's close()
  // path *before* ::close(fd) so a strand blocked on the fd is released
  // promptly and deterministically rather than waiting for a timeout.
  void notify_closed(int fd);

  // Force the reactor to re-scan cancel flags now. Call from a cancellation
  // path (e.g. scheduler cancel) so a blocked IO wait is cancelled promptly
  // instead of at the next safety re-scan.
  void kick();

  ReactorStats stats() const;

private:
  class Impl;
  Impl *impl_;
};

} // namespace amber::runtime
