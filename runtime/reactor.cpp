#include "runtime/reactor.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__FreeBSD__)
#define AMBER_REACTOR_KQUEUE 1
#include <sys/event.h>
#include <sys/types.h>
#elif defined(__linux__)
#define AMBER_REACTOR_EPOLL 1
#include <sys/epoll.h>
#else
#define AMBER_REACTOR_POLL 1
#include <poll.h>
#endif

namespace amber::runtime {

namespace {

using Clock = std::chrono::steady_clock;

// How long the reactor will sit in its poll() call when at least one waiter
// carries a cancellation flag but no explicit kick() arrives. Cancellation is
// normally delivered promptly via kick() from the cancelling thread; this is a
// safety re-scan bound so a missed kick degrades to bounded latency rather than
// a hang. With no cancel-flagged waiters the reactor blocks indefinitely until
// an fd event, a deadline, or a control wake.
constexpr int kCancelSafetyMs = 250;

void set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

// A single fd that the backend reported actionable this loop. `error` folds in
// EOF/HUP/ERR conditions: like the old poll loop, those are surfaced as Ready
// so the caller re-attempts the syscall and observes the precise errno.
struct ReadyFd {
  int fd = -1;
  bool readable = false;
  bool writable = false;
  bool error = false;
};

// Platform readiness backend. Owned and touched only by the reactor thread
// (except wake(), which is thread-safe via the self-pipe write). Tracks the
// currently-registered interest per fd and diffs against the desired set.
class PollBackend {
public:
  PollBackend() {
    if (::pipe(wake_pipe_) != 0) {
      wake_pipe_[0] = wake_pipe_[1] = -1;
    } else {
      set_nonblocking(wake_pipe_[0]);
      set_nonblocking(wake_pipe_[1]);
    }
    init_backend();
  }

  ~PollBackend() {
    close_backend();
    if (wake_pipe_[0] >= 0) {
      ::close(wake_pipe_[0]);
    }
    if (wake_pipe_[1] >= 0) {
      ::close(wake_pipe_[1]);
    }
  }

  // Wake the reactor out of poll(). Safe to call from any thread.
  void wake() {
    if (wake_pipe_[1] < 0) {
      return;
    }
    const char byte = 1;
    ssize_t rc = ::write(wake_pipe_[1], &byte, 1);
    (void)rc; // a full pipe already means "wake pending"
  }

  // Bring the registered interest set in line with `desired`. Returns the fds
  // that could not be registered (e.g. EBADF) so the reactor can fail their
  // waiters with Closed.
  std::vector<int> sync(const std::map<int, std::pair<bool, bool>> &desired);

  // Block up to timeout_ms (<0 = infinite) for readiness. Drains the wake pipe
  // internally and never reports it as a ready fd.
  void poll(int timeout_ms, std::vector<ReadyFd> &out);

private:
  void init_backend();
  void close_backend();
  void drain_wake_pipe() {
    char buf[256];
    while (::read(wake_pipe_[0], buf, sizeof(buf)) > 0) {
    }
  }

  int wake_pipe_[2] = {-1, -1};
  std::unordered_map<int, std::pair<bool, bool>> registered_; // fd -> (r, w)

#if defined(AMBER_REACTOR_KQUEUE)
  int kq_ = -1;
#elif defined(AMBER_REACTOR_EPOLL)
  int ep_ = -1;
#endif
};

#if defined(AMBER_REACTOR_KQUEUE)

void PollBackend::init_backend() {
  kq_ = ::kqueue();
  if (kq_ >= 0 && wake_pipe_[0] >= 0) {
    struct kevent change;
    EV_SET(&change, wake_pipe_[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
    ::kevent(kq_, &change, 1, nullptr, 0, nullptr);
  }
}

void PollBackend::close_backend() {
  if (kq_ >= 0) {
    ::close(kq_);
    kq_ = -1;
  }
}

// Returns false only on EBADF (the fd is dead). EV_DELETE of a filter that was
// never registered reports ENOENT, which is benign and ignored.
static bool kq_apply(int kq, int fd, int16_t filter, uint16_t flags,
                     bool *bad_fd) {
  struct kevent change;
  struct kevent result;
  EV_SET(&change, fd, filter, flags | EV_RECEIPT, 0, 0, nullptr);
  const int n = ::kevent(kq, &change, 1, &result, 1, nullptr);
  if (n > 0 && (result.flags & EV_ERROR) != 0 && result.data != 0) {
    if (result.data == EBADF) {
      *bad_fd = true;
    }
    return result.data == ENOENT; // deleting an absent filter is fine
  }
  return true;
}

std::vector<int>
PollBackend::sync(const std::map<int, std::pair<bool, bool>> &desired) {
  std::vector<int> failed;
  for (const auto &[fd, want] : desired) {
    const auto cur = registered_.find(fd);
    const bool cr = cur != registered_.end() && cur->second.first;
    const bool cw = cur != registered_.end() && cur->second.second;
    bool bad = false;
    if (want.first && !cr) {
      kq_apply(kq_, fd, EVFILT_READ, EV_ADD, &bad);
    } else if (!want.first && cr) {
      kq_apply(kq_, fd, EVFILT_READ, EV_DELETE, &bad);
    }
    if (want.second && !cw) {
      kq_apply(kq_, fd, EVFILT_WRITE, EV_ADD, &bad);
    } else if (!want.second && cw) {
      kq_apply(kq_, fd, EVFILT_WRITE, EV_DELETE, &bad);
    }
    if (bad) {
      failed.push_back(fd);
      registered_.erase(fd);
    } else {
      registered_[fd] = want;
    }
  }
  for (auto it = registered_.begin(); it != registered_.end();) {
    if (desired.find(it->first) == desired.end()) {
      bool bad = false;
      if (it->second.first) {
        kq_apply(kq_, it->first, EVFILT_READ, EV_DELETE, &bad);
      }
      if (it->second.second) {
        kq_apply(kq_, it->first, EVFILT_WRITE, EV_DELETE, &bad);
      }
      it = registered_.erase(it);
    } else {
      ++it;
    }
  }
  return failed;
}

void PollBackend::poll(int timeout_ms, std::vector<ReadyFd> &out) {
  struct kevent events[64];
  struct timespec ts;
  struct timespec *tp = nullptr;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
    tp = &ts;
  }
  const int n = ::kevent(kq_, nullptr, 0, events, 64, tp);
  for (int i = 0; i < n; ++i) {
    const int fd = static_cast<int>(events[i].ident);
    if (fd == wake_pipe_[0]) {
      drain_wake_pipe();
      continue;
    }
    ReadyFd rf;
    rf.fd = fd;
    rf.readable = events[i].filter == EVFILT_READ;
    rf.writable = events[i].filter == EVFILT_WRITE;
    rf.error = (events[i].flags & (EV_EOF | EV_ERROR)) != 0;
    out.push_back(rf);
  }
}

#elif defined(AMBER_REACTOR_EPOLL)

void PollBackend::init_backend() {
  ep_ = ::epoll_create1(0);
  if (ep_ >= 0 && wake_pipe_[0] >= 0) {
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wake_pipe_[0];
    ::epoll_ctl(ep_, EPOLL_CTL_ADD, wake_pipe_[0], &ev);
  }
}

void PollBackend::close_backend() {
  if (ep_ >= 0) {
    ::close(ep_);
    ep_ = -1;
  }
}

std::vector<int>
PollBackend::sync(const std::map<int, std::pair<bool, bool>> &desired) {
  std::vector<int> failed;
  for (const auto &[fd, want] : desired) {
    struct epoll_event ev{};
    ev.events = (want.first ? EPOLLIN : 0u) | (want.second ? EPOLLOUT : 0u);
    ev.data.fd = fd;
    const auto cur = registered_.find(fd);
    const int op = cur == registered_.end() ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    if (::epoll_ctl(ep_, op, fd, &ev) != 0) {
      if (errno == EBADF || errno == EPERM || errno == ENOENT) {
        failed.push_back(fd);
        registered_.erase(fd);
        continue;
      }
    }
    registered_[fd] = want;
  }
  for (auto it = registered_.begin(); it != registered_.end();) {
    if (desired.find(it->first) == desired.end()) {
      ::epoll_ctl(ep_, EPOLL_CTL_DEL, it->first, nullptr);
      it = registered_.erase(it);
    } else {
      ++it;
    }
  }
  return failed;
}

void PollBackend::poll(int timeout_ms, std::vector<ReadyFd> &out) {
  struct epoll_event events[64];
  const int n = ::epoll_wait(ep_, events, 64, timeout_ms);
  for (int i = 0; i < n; ++i) {
    const int fd = events[i].data.fd;
    if (fd == wake_pipe_[0]) {
      drain_wake_pipe();
      continue;
    }
    ReadyFd rf;
    rf.fd = fd;
    rf.readable = (events[i].events & EPOLLIN) != 0;
    rf.writable = (events[i].events & EPOLLOUT) != 0;
    rf.error = (events[i].events & (EPOLLERR | EPOLLHUP)) != 0;
    out.push_back(rf);
  }
}

#else // AMBER_REACTOR_POLL

void PollBackend::init_backend() {}
void PollBackend::close_backend() {}

std::vector<int>
PollBackend::sync(const std::map<int, std::pair<bool, bool>> &desired) {
  registered_.clear();
  registered_.insert(desired.begin(), desired.end());
  return {};
}

void PollBackend::poll(int timeout_ms, std::vector<ReadyFd> &out) {
  std::vector<struct pollfd> fds;
  fds.push_back(pollfd{wake_pipe_[0], POLLIN, 0});
  for (const auto &[fd, want] : registered_) {
    short events = 0;
    if (want.first) {
      events |= POLLIN;
    }
    if (want.second) {
      events |= POLLOUT;
    }
    fds.push_back(pollfd{fd, events, 0});
  }
  const int n = ::poll(fds.data(), fds.size(), timeout_ms);
  if (n <= 0) {
    return;
  }
  for (const auto &pfd : fds) {
    if (pfd.revents == 0) {
      continue;
    }
    if (pfd.fd == wake_pipe_[0]) {
      drain_wake_pipe();
      continue;
    }
    ReadyFd rf;
    rf.fd = pfd.fd;
    rf.readable = (pfd.revents & POLLIN) != 0;
    rf.writable = (pfd.revents & POLLOUT) != 0;
    rf.error = (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
    out.push_back(rf);
  }
}

#endif

} // namespace

class RuntimeReactor::Impl {
public:
  Impl() : thread_([this]() { run(); }) {}

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
    }
    backend_.wake();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  ReactorOutcome wait(int fd, ReactorInterest interest,
                      std::optional<Clock::time_point> deadline,
                      const std::atomic<bool> *cancel_flag) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_) {
      return ReactorOutcome::Error;
    }
    const std::uint64_t id = next_wait_id_++;
    Waiter &w = waiters_[id];
    w.fd = fd;
    w.want_read = interest == ReactorInterest::Read;
    w.want_write = interest == ReactorInterest::Write;
    w.deadline = deadline;
    w.cancel_flag = cancel_flag;
    ++stats_.waits;
    if (waiters_.size() > stats_.max_waiters) {
      stats_.max_waiters = waiters_.size();
    }
    backend_.wake();
    w.cv.wait(lock, [&w]() { return w.done; });

    const ReactorOutcome outcome = w.outcome;
    waiters_.erase(id);
    tally_outcome_locked(outcome);
    return outcome;
  }

  void wait_async(int fd, ReactorInterest interest,
                  std::optional<Clock::time_point> deadline,
                  const std::atomic<bool> *cancel_flag,
                  std::function<void(ReactorOutcome)> completion) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        // Reactor is shutting down: resolve immediately so the caller is not
        // left waiting on a completion that will never arrive.
        completion(ReactorOutcome::Error);
        return;
      }
      const std::uint64_t id = next_wait_id_++;
      Waiter &w = waiters_[id];
      w.fd = fd;
      w.want_read = interest == ReactorInterest::Read;
      w.want_write = interest == ReactorInterest::Write;
      w.deadline = deadline;
      w.cancel_flag = cancel_flag;
      w.completion = std::move(completion);
      ++stats_.waits;
      if (waiters_.size() > stats_.max_waiters) {
        stats_.max_waiters = waiters_.size();
      }
    }
    backend_.wake();
  }

  void notify_closed(int fd) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto &[id, w] : waiters_) {
        (void)id;
        if (w.fd == fd && !w.done) {
          w.pending_closed = true;
        }
      }
    }
    backend_.wake();
  }

  void kick() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.kicks;
    }
    backend_.wake();
  }

  ReactorStats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ReactorStats out = stats_;
    out.current_waiters = waiters_.size();
    return out;
  }

private:
  struct Waiter {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    std::optional<Clock::time_point> deadline;
    const std::atomic<bool> *cancel_flag = nullptr;
    bool pending_closed = false;
    bool done = false;
    ReactorOutcome outcome = ReactorOutcome::Error;
    std::condition_variable cv;
    // When set, this is an async (non-blocking) waiter: on completion the
    // reactor runs `completion(outcome)` (outside the lock) and erases the
    // waiter itself, rather than signalling cv for a blocked caller.
    std::function<void(ReactorOutcome)> completion;
  };

  void complete_locked(Waiter &w, ReactorOutcome outcome) {
    if (w.done) {
      return;
    }
    w.outcome = outcome;
    w.done = true;
    w.cv.notify_one(); // no-op for async waiters (no blocked caller)
  }

  void tally_outcome_locked(ReactorOutcome outcome) {
    switch (outcome) {
    case ReactorOutcome::Ready:
      ++stats_.ready;
      break;
    case ReactorOutcome::TimedOut:
      ++stats_.timeouts;
      break;
    case ReactorOutcome::Cancelled:
      ++stats_.cancellations;
      break;
    case ReactorOutcome::Closed:
      ++stats_.closes;
      break;
    case ReactorOutcome::Error:
      ++stats_.errors;
      break;
    }
  }

  // Move out the completion callbacks of finished async waiters and erase them.
  // The callbacks are run by the caller *after* releasing the reactor mutex:
  // a completion typically marks a strand runnable and wakes a scheduler
  // worker, so running it under the reactor lock would invert the
  // scheduler->reactor lock order taken by kick() and could deadlock.
  void harvest_async_locked(
      std::vector<std::pair<std::function<void(ReactorOutcome)>,
                            ReactorOutcome>> &out) {
    std::vector<std::uint64_t> finished;
    for (auto &[id, w] : waiters_) {
      if (w.done && w.completion) {
        out.emplace_back(std::move(w.completion), w.outcome);
        tally_outcome_locked(w.outcome);
        finished.push_back(id);
      }
    }
    for (const std::uint64_t id : finished) {
      waiters_.erase(id);
    }
  }

  void run() {
    std::vector<std::pair<std::function<void(ReactorOutcome)>, ReactorOutcome>>
        completions;
    while (true) {
      int timeout_ms = -1;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!running_) {
          return;
        }

        const Clock::time_point now = Clock::now();
        bool any_cancel = false;
        std::optional<Clock::time_point> nearest;
        std::map<int, std::pair<bool, bool>> desired;

        for (auto &[id, w] : waiters_) {
          (void)id;
          if (w.done) {
            continue;
          }
          if (w.pending_closed) {
            complete_locked(w, ReactorOutcome::Closed);
            continue;
          }
          if (w.cancel_flag != nullptr && w.cancel_flag->load()) {
            complete_locked(w, ReactorOutcome::Cancelled);
            continue;
          }
          if (w.deadline.has_value() && now >= *w.deadline) {
            complete_locked(w, ReactorOutcome::TimedOut);
            continue;
          }
          auto &entry = desired[w.fd];
          entry.first = entry.first || w.want_read;
          entry.second = entry.second || w.want_write;
          if (w.cancel_flag != nullptr) {
            any_cancel = true;
          }
          if (w.deadline.has_value() &&
              (!nearest.has_value() || *w.deadline < *nearest)) {
            nearest = w.deadline;
          }
        }

        const std::vector<int> failed = backend_.sync(desired);
        for (const int fd : failed) {
          for (auto &[id, w] : waiters_) {
            (void)id;
            if (w.fd == fd && !w.done) {
              complete_locked(w, ReactorOutcome::Closed);
            }
          }
        }

        if (nearest.has_value()) {
          const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              *nearest - now)
                              .count();
          timeout_ms = ms <= 0 ? 0 : static_cast<int>(ms);
        }
        if (any_cancel) {
          timeout_ms = timeout_ms < 0 ? kCancelSafetyMs
                                      : std::min(timeout_ms, kCancelSafetyMs);
        }
        harvest_async_locked(completions);
      }
      for (auto &[fn, oc] : completions) {
        fn(oc);
      }
      completions.clear();

      std::vector<ReadyFd> ready;
      backend_.poll(timeout_ms, ready);

      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.loop_wakeups;
        for (const ReadyFd &rf : ready) {
          for (auto &[id, w] : waiters_) {
            (void)id;
            if (w.done || w.fd != rf.fd) {
              continue;
            }
            const bool hit = (w.want_read && (rf.readable || rf.error)) ||
                             (w.want_write && (rf.writable || rf.error));
            if (hit) {
              complete_locked(w, ReactorOutcome::Ready);
            }
          }
        }
        harvest_async_locked(completions);
      }
      for (auto &[fn, oc] : completions) {
        fn(oc);
      }
      completions.clear();
    }
  }

  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, Waiter> waiters_;
  std::uint64_t next_wait_id_ = 1;
  bool running_ = true;
  ReactorStats stats_;
  PollBackend backend_;
  std::thread thread_;
};

RuntimeReactor::RuntimeReactor() : impl_(new Impl()) {}

RuntimeReactor::~RuntimeReactor() { delete impl_; }

RuntimeReactor &RuntimeReactor::instance() {
  static RuntimeReactor reactor;
  return reactor;
}

ReactorOutcome RuntimeReactor::wait(
    int fd, ReactorInterest interest,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    const std::atomic<bool> *cancel_flag) {
  return impl_->wait(fd, interest, deadline, cancel_flag);
}

void RuntimeReactor::wait_async(
    int fd, ReactorInterest interest,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    const std::atomic<bool> *cancel_flag,
    std::function<void(ReactorOutcome)> completion) {
  impl_->wait_async(fd, interest, deadline, cancel_flag, std::move(completion));
}

void RuntimeReactor::notify_closed(int fd) { impl_->notify_closed(fd); }

void RuntimeReactor::kick() { impl_->kick(); }

ReactorStats RuntimeReactor::stats() const { return impl_->stats(); }

} // namespace amber::runtime
