#include "runtime/vm.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>

namespace amber::runtime {

namespace {

thread_local std::uint64_t tls_runtime_worker_id = 0;
thread_local std::uint64_t tls_runtime_strand_id = 0;
thread_local std::uint64_t tls_runtime_task_id = 0;
thread_local const std::atomic<bool> *tls_runtime_task_cancel_flag = nullptr;

class RuntimeTaskScope {
public:
  RuntimeTaskScope(std::uint64_t task_id, const std::atomic<bool> *cancel_flag)
      : previous_task_id_(tls_runtime_task_id),
        previous_cancel_flag_(tls_runtime_task_cancel_flag) {
    tls_runtime_task_id = task_id;
    tls_runtime_task_cancel_flag = cancel_flag;
  }

  RuntimeTaskScope(const RuntimeTaskScope &) = delete;
  RuntimeTaskScope &operator=(const RuntimeTaskScope &) = delete;

  ~RuntimeTaskScope() {
    tls_runtime_task_id = previous_task_id_;
    tls_runtime_task_cancel_flag = previous_cancel_flag_;
  }

private:
  std::uint64_t previous_task_id_ = 0;
  const std::atomic<bool> *previous_cancel_flag_ = nullptr;
};

std::uint64_t current_runtime_owner_strand_id() {
  return tls_runtime_strand_id != 0 ? tls_runtime_strand_id
                                    : tls_runtime_worker_id;
}

void increment_kind_allocation(RuntimeHeapStats &stats, HeapObjectKind kind) {
  switch (kind) {
  case HeapObjectKind::Instance:
    ++stats.instance_allocations;
    return;
  case HeapObjectKind::List:
  case HeapObjectKind::Tuple:
    ++stats.array_allocations;
    return;
  case HeapObjectKind::Map:
    ++stats.map_allocations;
    return;
  case HeapObjectKind::Closure:
    ++stats.closure_allocations;
    return;
  }
}

bool value_has_heap_payload_tag(const Value &value);
const ObjHeader *heap_header_from_value(const Value &value);
ObjHeader *mutable_heap_header_from_value(const Value &value);
bool header_is_deallocated(const ObjHeader &header);
bool header_is_destroyed(const ObjHeader &header);
std::optional<std::string> lifecycle_access_error_name(const ObjHeader &header);
std::string lifecycle_access_error_message(const std::string &error_name);

struct RuntimeSyncBoundaryError {
  std::string error_name;
  std::string message;
};

std::optional<RuntimeSyncBoundaryError>
runtime_value_shareability_error(const Value &value);

} // namespace

std::uint64_t current_runtime_worker_id() { return tls_runtime_worker_id; }

std::uint64_t current_runtime_strand_id() { return tls_runtime_strand_id; }

std::uint64_t current_runtime_task_id() { return tls_runtime_task_id; }

bool current_runtime_task_cancel_requested() {
  return tls_runtime_task_cancel_flag != nullptr &&
         tls_runtime_task_cancel_flag->load();
}

RuntimeTaskFailure::RuntimeTaskFailure(std::string error_name,
                                       std::string message)
    : error_name_(std::move(error_name)), message_(std::move(message)),
      what_(error_name_ + ": " + message_) {}

const char *RuntimeTaskFailure::what() const noexcept { return what_.c_str(); }

const std::string &RuntimeTaskFailure::error_name() const {
  return error_name_;
}

const std::string &RuntimeTaskFailure::message() const { return message_; }

RuntimeTaskCancelled::RuntimeTaskCancelled() = default;

const char *RuntimeTaskCancelled::what() const noexcept {
  return "CancelledError: task cancelled";
}

void throw_if_runtime_task_cancelled() {
  if (current_runtime_task_cancel_requested()) {
    throw RuntimeTaskCancelled();
  }
}

RuntimeWorkerScope::RuntimeWorkerScope(std::uint64_t worker_id)
    : previous_worker_id_(tls_runtime_worker_id) {
  tls_runtime_worker_id = worker_id;
}

RuntimeWorkerScope::~RuntimeWorkerScope() {
  tls_runtime_worker_id = previous_worker_id_;
}

RuntimeStrandScope::RuntimeStrandScope(std::uint64_t strand_id)
    : previous_strand_id_(tls_runtime_strand_id) {
  tls_runtime_strand_id = strand_id;
}

RuntimeStrandScope::~RuntimeStrandScope() {
  tls_runtime_strand_id = previous_strand_id_;
}

namespace {

bool runtime_header_is_shareable_or_sync(const ObjHeader &header) {
  return header.owner.kind == OwnerTokenKind::Shareable ||
         header.owner.kind == OwnerTokenKind::Sync ||
         header.generation == ObjectGeneration::Shared ||
         (header.flags & kObjectFlagShareable) != 0U;
}

RuntimeSyncBoundaryError runtime_isolation_error() {
  return RuntimeSyncBoundaryError{"IsolationError",
                                  "channel payload must be shareable"};
}

std::optional<RuntimeSyncBoundaryError> runtime_value_shareability_error_impl(
    const Value &value, std::unordered_set<std::uint64_t> *visited) {
  if (!value_has_heap_payload_tag(value)) {
    return std::nullopt;
  }

  const ObjHeader *header = heap_header_from_value(value);
  if (header == nullptr) {
    return RuntimeSyncBoundaryError{"TypeError",
                                    "heap object reference is null"};
  }
  const std::optional<std::string> lifecycle_error =
      lifecycle_access_error_name(*header);
  if (lifecycle_error.has_value()) {
    return RuntimeSyncBoundaryError{
        *lifecycle_error, lifecycle_access_error_message(*lifecycle_error)};
  }
  if (header->allocation_id != 0 &&
      !visited->insert(header->allocation_id).second) {
    return std::nullopt;
  }

  if (value.is_closure()) {
    const std::shared_ptr<ClosureValue> closure = value.as_closure();
    if (closure == nullptr) {
      return RuntimeSyncBoundaryError{"TypeError", "closure value is null"};
    }
    for (const Value &capture : closure->captures) {
      std::optional<RuntimeSyncBoundaryError> error =
          runtime_value_shareability_error_impl(capture, visited);
      if (error.has_value()) {
        return error;
      }
    }
    if (!closure->self.is_null()) {
      std::optional<RuntimeSyncBoundaryError> error =
          runtime_value_shareability_error_impl(closure->self, visited);
      if (error.has_value()) {
        return error;
      }
    }
    return std::nullopt;
  }

  if (!runtime_header_is_shareable_or_sync(*header)) {
    return runtime_isolation_error();
  }

  if (value.is_list()) {
    const std::shared_ptr<ListValue> list = value.as_list();
    if (list == nullptr) {
      return RuntimeSyncBoundaryError{"TypeError", "list value is null"};
    }
    if (!list->frozen) {
      return runtime_isolation_error();
    }
    for (const Value &item : list->items) {
      std::optional<RuntimeSyncBoundaryError> error =
          runtime_value_shareability_error_impl(item, visited);
      if (error.has_value()) {
        return error;
      }
    }
    return std::nullopt;
  }

  if (value.is_tuple()) {
    const std::shared_ptr<TupleValue> tuple = value.as_tuple();
    if (tuple == nullptr) {
      return RuntimeSyncBoundaryError{"TypeError", "tuple value is null"};
    }
    for (const Value &item : tuple->items) {
      std::optional<RuntimeSyncBoundaryError> error =
          runtime_value_shareability_error_impl(item, visited);
      if (error.has_value()) {
        return error;
      }
    }
    return std::nullopt;
  }

  if (value.is_map()) {
    const std::shared_ptr<MapValue> map = value.as_map();
    if (map == nullptr) {
      return RuntimeSyncBoundaryError{"TypeError", "map value is null"};
    }
    if (!map->frozen) {
      return runtime_isolation_error();
    }
    for (const MapEntry &entry : map->entries) {
      std::optional<RuntimeSyncBoundaryError> error =
          runtime_value_shareability_error_impl(entry.value, visited);
      if (error.has_value()) {
        return error;
      }
    }
    return std::nullopt;
  }

  if (value.is_instance_object()) {
    const std::shared_ptr<InstanceValue> instance = value.as_instance_object();
    if (instance == nullptr) {
      return RuntimeSyncBoundaryError{"TypeError", "instance value is null"};
    }
    for (const Value &slot : instance->ivar_storage) {
      std::optional<RuntimeSyncBoundaryError> error =
          runtime_value_shareability_error_impl(slot, visited);
      if (error.has_value()) {
        return error;
      }
    }
    for (const auto &[name, ivar] : instance->ivars) {
      (void)name;
      std::optional<RuntimeSyncBoundaryError> error =
          runtime_value_shareability_error_impl(ivar, visited);
      if (error.has_value()) {
        return error;
      }
    }
  }

  return std::nullopt;
}

std::optional<RuntimeSyncBoundaryError>
runtime_value_shareability_error(const Value &value) {
  std::unordered_set<std::uint64_t> visited;
  return runtime_value_shareability_error_impl(value, &visited);
}

RuntimeSyncBoundaryError runtime_move_error(const std::string &message) {
  return RuntimeSyncBoundaryError{"MoveError", message};
}

bool runtime_header_is_shared_or_sync(const ObjHeader &header) {
  return header.owner.kind == OwnerTokenKind::Shareable ||
         header.owner.kind == OwnerTokenKind::Sync ||
         header.generation == ObjectGeneration::Shared ||
         (header.flags & kObjectFlagShareable) != 0U;
}

void runtime_append_child_values(const Value &value,
                                 std::vector<Value> *children) {
  if (children == nullptr) {
    return;
  }
  if (value.is_closure()) {
    const std::shared_ptr<ClosureValue> closure = value.as_closure();
    if (closure == nullptr) {
      return;
    }
    children->insert(children->end(), closure->captures.begin(),
                     closure->captures.end());
    if (!closure->self.is_null()) {
      children->push_back(closure->self);
    }
    return;
  }
  if (value.is_list()) {
    const std::shared_ptr<ListValue> list = value.as_list();
    if (list != nullptr) {
      children->insert(children->end(), list->items.begin(), list->items.end());
    }
    return;
  }
  if (value.is_tuple()) {
    const std::shared_ptr<TupleValue> tuple = value.as_tuple();
    if (tuple != nullptr) {
      children->insert(children->end(), tuple->items.begin(),
                       tuple->items.end());
    }
    return;
  }
  if (value.is_map()) {
    const std::shared_ptr<MapValue> map = value.as_map();
    if (map != nullptr) {
      for (const MapEntry &entry : map->entries) {
        children->push_back(entry.value);
      }
    }
    return;
  }
  if (value.is_instance_object()) {
    const std::shared_ptr<InstanceValue> instance = value.as_instance_object();
    if (instance == nullptr) {
      return;
    }
    children->insert(children->end(), instance->ivar_storage.begin(),
                     instance->ivar_storage.end());
    for (const auto &[name, ivar] : instance->ivars) {
      (void)name;
      children->push_back(ivar);
    }
  }
}

std::optional<RuntimeSyncBoundaryError>
runtime_value_move_error_impl(const Value &value, bool top_level,
                              std::unordered_set<std::uint64_t> *visited) {
  if (!value_has_heap_payload_tag(value)) {
    if (top_level) {
      return runtime_move_error("move expects a confined heap object");
    }
    return std::nullopt;
  }

  const ObjHeader *header = heap_header_from_value(value);
  if (header == nullptr) {
    return RuntimeSyncBoundaryError{"TypeError",
                                    "heap object reference is null"};
  }
  const std::optional<std::string> lifecycle_error =
      lifecycle_access_error_name(*header);
  if (lifecycle_error.has_value()) {
    return RuntimeSyncBoundaryError{
        *lifecycle_error, lifecycle_access_error_message(*lifecycle_error)};
  }
  if (header->allocation_id != 0 &&
      !visited->insert(header->allocation_id).second) {
    return std::nullopt;
  }
  if (runtime_header_is_shared_or_sync(*header)) {
    if (top_level) {
      return runtime_move_error(
          "move is only valid for strand-confined heap objects");
    }
    return std::nullopt;
  }
  if (header->owner.kind == OwnerTokenKind::Confined &&
      header->owner.strand_id != current_runtime_owner_strand_id()) {
    return RuntimeSyncBoundaryError{"IsolationError",
                                    "move must run on the owner strand"};
  }

  std::vector<Value> children;
  runtime_append_child_values(value, &children);
  for (const Value &child : children) {
    std::optional<RuntimeSyncBoundaryError> error =
        runtime_value_move_error_impl(child, false, visited);
    if (error.has_value()) {
      return error;
    }
  }
  return std::nullopt;
}

std::optional<RuntimeSyncBoundaryError>
runtime_value_move_error(const Value &value) {
  std::unordered_set<std::uint64_t> visited;
  return runtime_value_move_error_impl(value, true, &visited);
}

void runtime_reown_move_graph_impl(const Value &value, OwnerTokenKind kind,
                                   std::uint64_t strand_id,
                                   std::unordered_set<std::uint64_t> *visited) {
  ObjHeader *header = mutable_heap_header_from_value(value);
  if (header == nullptr) {
    return;
  }
  if (header->allocation_id != 0 &&
      !visited->insert(header->allocation_id).second) {
    return;
  }
  if (header->owner.kind == OwnerTokenKind::Shareable ||
      header->generation == ObjectGeneration::Shared ||
      (header->flags & kObjectFlagShareable) != 0U) {
    return;
  }
  header->owner.kind = kind;
  header->owner.strand_id = strand_id;

  std::vector<Value> children;
  runtime_append_child_values(value, &children);
  for (const Value &child : children) {
    runtime_reown_move_graph_impl(child, kind, strand_id, visited);
  }
}

void runtime_mark_moved_value_in_transit(const Value &value) {
  std::unordered_set<std::uint64_t> visited;
  runtime_reown_move_graph_impl(value, OwnerTokenKind::Sync, 0, &visited);
}

void runtime_adopt_moved_value(const Value &value) {
  std::unordered_set<std::uint64_t> visited;
  runtime_reown_move_graph_impl(value, OwnerTokenKind::Confined,
                                current_runtime_owner_strand_id(), &visited);
}

std::optional<std::chrono::steady_clock::time_point>
runtime_sync_deadline(std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max()) {
    return std::nullopt;
  }
  if (timeout <= std::chrono::milliseconds(0)) {
    return std::chrono::steady_clock::now();
  }
  return std::chrono::steady_clock::now() + timeout;
}

bool runtime_sync_deadline_expired(
    const std::optional<std::chrono::steady_clock::time_point> &deadline) {
  return deadline.has_value() && std::chrono::steady_clock::now() >= *deadline;
}

std::chrono::steady_clock::duration runtime_sync_wait_duration(
    const std::optional<std::chrono::steady_clock::time_point> &deadline) {
  const auto one_millisecond = std::chrono::milliseconds(1);
  if (!deadline.has_value()) {
    return one_millisecond;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now >= *deadline) {
    return std::chrono::steady_clock::duration::zero();
  }
  const auto remaining = *deadline - now;
  return remaining < one_millisecond ? remaining : one_millisecond;
}

std::uint64_t runtime_sync_owner_id() {
  if (tls_runtime_task_id != 0) {
    return (tls_runtime_task_id << 2U) | 1U;
  }
  if (tls_runtime_strand_id != 0) {
    return (tls_runtime_strand_id << 2U) | 2U;
  }
  if (tls_runtime_worker_id != 0) {
    return (tls_runtime_worker_id << 2U) | 3U;
  }
  const std::size_t thread_hash =
      std::hash<std::thread::id>{}(std::this_thread::get_id());
  return (static_cast<std::uint64_t>(thread_hash) << 2U) | 3U;
}

RuntimeChannelResult runtime_channel_closed_result() {
  RuntimeChannelResult result;
  result.closed = true;
  result.error_name = "ChannelClosedError";
  result.message = "channel is closed";
  return result;
}

RuntimeChannelResult runtime_channel_timeout_result(bool send) {
  RuntimeChannelResult result;
  result.timed_out = true;
  result.error_name = "TimeoutError";
  result.message = send ? "channel send timed out" : "channel recv timed out";
  return result;
}

RuntimeChannelResult runtime_channel_cancelled_result(bool send) {
  RuntimeChannelResult result;
  result.cancelled = true;
  result.error_name = "CancelledError";
  result.message = send ? "channel send cancelled" : "channel recv cancelled";
  return result;
}

RuntimeMutexResult runtime_mutex_timeout_result() {
  RuntimeMutexResult result;
  result.timed_out = true;
  result.error_name = "TimeoutError";
  result.message = "mutex lock timed out";
  return result;
}

RuntimeMutexResult runtime_mutex_cancelled_result() {
  RuntimeMutexResult result;
  result.cancelled = true;
  result.error_name = "CancelledError";
  result.message = "mutex lock cancelled";
  return result;
}

} // namespace

bool runtime_value_is_shareable(const Value &value) {
  return !runtime_value_shareability_error(value).has_value();
}

class RuntimeAwaitable::Impl {
public:
  RuntimeAwaitableResult await(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    ++stats_.waits;
    const std::optional<std::chrono::steady_clock::time_point> deadline =
        runtime_sync_deadline(timeout);

    while (true) {
      if (state_ != RuntimeAwaitableState::Pending) {
        return terminal_result_locked();
      }
      if (current_runtime_task_cancel_requested()) {
        mark_cancelled_locked("awaitable await cancelled");
        cv_.notify_all();
        return terminal_result_locked();
      }
      if (refresh_native_wait_locked()) {
        cv_.notify_all();
        return terminal_result_locked();
      }
      if (runtime_sync_deadline_expired(deadline)) {
        ++stats_.timeouts;
        return timeout_result_locked();
      }

      const std::chrono::steady_clock::duration wait_duration =
          runtime_sync_wait_duration(deadline);
      if (wait_duration <= std::chrono::steady_clock::duration::zero()) {
        continue;
      }
      cv_.wait_for(lock, wait_duration);
    }
  }

  RuntimeAwaitableResult poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.polls;
    if (state_ != RuntimeAwaitableState::Pending) {
      return terminal_result_locked();
    }
    if (refresh_native_wait_locked()) {
      cv_.notify_all();
      return terminal_result_locked();
    }
    return timeout_result_locked();
  }

  bool complete(Value value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RuntimeAwaitableState::Pending) {
      return false;
    }
    if (refresh_native_wait_locked()) {
      cv_.notify_all();
      return false;
    }
    std::string error_name;
    std::string message;
    if (!finish_native_wait_locked(&error_name, &message)) {
      mark_failed_locked(std::move(error_name), std::move(message));
      cv_.notify_all();
      return false;
    }
    state_ = RuntimeAwaitableState::Ready;
    value_ = std::move(value);
    ++stats_.completions;
    cv_.notify_all();
    return true;
  }

  bool fail(std::string error_name, std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RuntimeAwaitableState::Pending) {
      return false;
    }
    (void)finish_native_wait_locked(nullptr, nullptr);
    mark_failed_locked(std::move(error_name), std::move(message));
    cv_.notify_all();
    return true;
  }

  bool cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RuntimeAwaitableState::Pending) {
      return false;
    }
    mark_cancelled_locked("awaitable cancelled");
    cv_.notify_all();
    return true;
  }

  void attach_native_wait(RuntimeHeap &heap, const RuntimePinToken &token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RuntimeAwaitableState::Pending || native_backed_) {
      return;
    }
    native_heap_ = &heap;
    RuntimeNativeWaitResult wait = heap.register_native_wait(token);
    if (!wait.ok) {
      native_heap_ = nullptr;
      mark_failed_locked(wait.error_name, wait.message);
      return;
    }
    native_backed_ = true;
    native_handle_ = wait.handle;
    stats_.native_backed = true;
  }

  RuntimeAwaitableState state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  RuntimeAwaitableStats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeAwaitableStats out = stats_;
    out.state = state_;
    out.native_backed = native_backed_;
    return out;
  }

private:
  RuntimeAwaitableResult terminal_result_locked() const {
    RuntimeAwaitableResult result;
    result.state = state_;
    result.value = value_;
    result.error_name = error_name_;
    result.message = message_;
    switch (state_) {
    case RuntimeAwaitableState::Ready:
      result.ok = true;
      result.ready = true;
      break;
    case RuntimeAwaitableState::Failed:
      result.failed = true;
      break;
    case RuntimeAwaitableState::Cancelled:
      result.cancelled = true;
      break;
    case RuntimeAwaitableState::Pending:
      break;
    }
    return result;
  }

  RuntimeAwaitableResult timeout_result_locked() const {
    RuntimeAwaitableResult result;
    result.state = state_;
    result.timed_out = true;
    result.error_name = "TimeoutError";
    result.message = "awaitable is not ready";
    return result;
  }

  void mark_failed_locked(std::string error_name, std::string message) {
    if (state_ != RuntimeAwaitableState::Pending) {
      return;
    }
    state_ = RuntimeAwaitableState::Failed;
    error_name_ = error_name.empty() ? "RuntimeError" : std::move(error_name);
    message_ = message.empty() ? "awaitable failed" : std::move(message);
    ++stats_.failures;
  }

  void mark_cancelled_locked(std::string message) {
    if (state_ != RuntimeAwaitableState::Pending) {
      return;
    }
    std::string error_name;
    std::string native_message;
    if (!cancel_native_wait_locked(&error_name, &native_message)) {
      mark_failed_locked(std::move(error_name), std::move(native_message));
      return;
    }
    state_ = RuntimeAwaitableState::Cancelled;
    error_name_ = "CancelledError";
    message_ = std::move(message);
    ++stats_.cancellations;
  }

  bool refresh_native_wait_locked() {
    if (!native_backed_ || native_heap_ == nullptr || !native_handle_.active) {
      return false;
    }
    RuntimeNativeWaitResult poll =
        native_heap_->poll_native_wait(native_handle_);
    ++stats_.native_polls;
    if (!poll.ok) {
      (void)finish_native_wait_locked(nullptr, nullptr);
      mark_failed_locked(std::move(poll.error_name), std::move(poll.message));
      return true;
    }
    native_handle_ = poll.handle;
    if (poll.cancelled) {
      (void)finish_native_wait_locked(nullptr, nullptr);
      state_ = RuntimeAwaitableState::Cancelled;
      error_name_ = "CancelledError";
      message_ = "native wait cancelled";
      ++stats_.cancellations;
      return true;
    }
    return false;
  }

  bool cancel_native_wait_locked(std::string *error_name,
                                 std::string *message) {
    if (!native_backed_ || native_heap_ == nullptr || !native_handle_.active) {
      return true;
    }
    RuntimeNativeWaitResult cancel =
        native_heap_->cancel_native_wait(&native_handle_);
    if (!cancel.ok) {
      if (error_name != nullptr) {
        *error_name = cancel.error_name;
      }
      if (message != nullptr) {
        *message = cancel.message;
      }
      return false;
    }
    native_handle_ = cancel.handle;
    ++stats_.native_cancellations;
    return finish_native_wait_locked(error_name, message);
  }

  bool finish_native_wait_locked(std::string *error_name,
                                 std::string *message) {
    if (!native_backed_ || native_heap_ == nullptr || !native_handle_.active) {
      return true;
    }
    RuntimeNativeWaitResult finish =
        native_heap_->finish_native_wait(&native_handle_);
    if (!finish.ok) {
      if (error_name != nullptr) {
        *error_name = finish.error_name;
      }
      if (message != nullptr) {
        *message = finish.message;
      }
      return false;
    }
    native_handle_ = finish.handle;
    ++stats_.native_finishes;
    return true;
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  RuntimeAwaitableState state_ = RuntimeAwaitableState::Pending;
  Value value_ = Value::null();
  std::string error_name_;
  std::string message_;
  RuntimeHeap *native_heap_ = nullptr;
  RuntimeNativeWaitHandle native_handle_;
  bool native_backed_ = false;
  RuntimeAwaitableStats stats_;
};

RuntimeAwaitable::RuntimeAwaitable() : impl_(std::make_shared<Impl>()) {}

RuntimeAwaitable::RuntimeAwaitable(RuntimeAwaitable &&) noexcept = default;

RuntimeAwaitable &
RuntimeAwaitable::operator=(RuntimeAwaitable &&) noexcept = default;

RuntimeAwaitable::~RuntimeAwaitable() = default;

RuntimeAwaitable RuntimeAwaitable::ready(Value value) {
  RuntimeAwaitable awaitable;
  (void)awaitable.complete(std::move(value));
  return awaitable;
}

RuntimeAwaitable
RuntimeAwaitable::from_native_wait(RuntimeHeap &heap,
                                   const RuntimePinToken &token) {
  RuntimeAwaitable awaitable;
  awaitable.impl_->attach_native_wait(heap, token);
  return awaitable;
}

bool RuntimeAwaitable::complete(Value value) {
  return impl_->complete(std::move(value));
}

bool RuntimeAwaitable::fail(std::string error_name, std::string message) {
  return impl_->fail(std::move(error_name), std::move(message));
}

bool RuntimeAwaitable::cancel() { return impl_->cancel(); }

RuntimeAwaitableResult
RuntimeAwaitable::await(std::chrono::milliseconds timeout) {
  return impl_->await(timeout);
}

RuntimeAwaitableResult RuntimeAwaitable::poll() { return impl_->poll(); }

RuntimeAwaitableState RuntimeAwaitable::state() const { return impl_->state(); }

RuntimeAwaitableStats RuntimeAwaitable::stats() const { return impl_->stats(); }

namespace {

RuntimeMoveResult runtime_move_result_error(RuntimeMoveSlotState state,
                                            std::string error_name,
                                            std::string message) {
  RuntimeMoveResult result;
  result.state = state;
  result.error_name = std::move(error_name);
  result.message = std::move(message);
  result.moved = state == RuntimeMoveSlotState::Moved;
  return result;
}

} // namespace

class RuntimeMoveSlot::Impl {
public:
  explicit Impl(Value value) : value_(std::move(value)) {}

  RuntimeMoveResult read() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == RuntimeMoveSlotState::Moved) {
      return runtime_move_result_error(state_, "MovedValueError",
                                       "value has already been moved");
    }
    RuntimeMoveResult result;
    result.ok = true;
    result.state = state_;
    result.value = value_;
    result.reserved = state_ == RuntimeMoveSlotState::Reserved;
    return result;
  }

  RuntimeMoveResult reserve_move() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == RuntimeMoveSlotState::Moved) {
      return runtime_move_result_error(state_, "MovedValueError",
                                       "value has already been moved");
    }
    if (state_ == RuntimeMoveSlotState::Reserved) {
      return runtime_move_result_error(state_, "MoveError",
                                       "value already has a pending move");
    }
    std::optional<RuntimeSyncBoundaryError> error =
        runtime_value_move_error(value_);
    if (error.has_value()) {
      return runtime_move_result_error(state_, error->error_name,
                                       error->message);
    }

    RuntimeMoveReservation reservation;
    reservation.reservation_id = next_reservation_id_++;
    reservation.value = value_;
    reservation.active = true;
    active_reservation_id_ = reservation.reservation_id;
    state_ = RuntimeMoveSlotState::Reserved;

    RuntimeMoveResult result;
    result.ok = true;
    result.reserved = true;
    result.state = state_;
    result.reservation = reservation;
    result.value = value_;
    return result;
  }

  RuntimeMoveResult commit_move(RuntimeMoveReservation *reservation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reservation == nullptr || !reservation->active ||
        reservation->reservation_id == 0 ||
        reservation->reservation_id != active_reservation_id_ ||
        state_ != RuntimeMoveSlotState::Reserved) {
      return runtime_move_result_error(state_, "MoveError",
                                       "move reservation is not active");
    }

    runtime_mark_moved_value_in_transit(value_);
    state_ = RuntimeMoveSlotState::Moved;
    active_reservation_id_ = 0;
    reservation->active = false;

    RuntimeMoveResult result;
    result.ok = true;
    result.moved = true;
    result.state = state_;
    result.value = value_;
    return result;
  }

  RuntimeMoveResult release_move(RuntimeMoveReservation *reservation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reservation == nullptr || !reservation->active ||
        reservation->reservation_id == 0 ||
        reservation->reservation_id != active_reservation_id_ ||
        state_ != RuntimeMoveSlotState::Reserved) {
      return runtime_move_result_error(state_, "MoveError",
                                       "move reservation is not active");
    }

    state_ = RuntimeMoveSlotState::Ready;
    active_reservation_id_ = 0;
    reservation->active = false;

    RuntimeMoveResult result;
    result.ok = true;
    result.state = state_;
    result.value = value_;
    return result;
  }

  void reset(Value value) {
    std::lock_guard<std::mutex> lock(mutex_);
    value_ = std::move(value);
    state_ = RuntimeMoveSlotState::Ready;
    active_reservation_id_ = 0;
  }

  RuntimeMoveSlotState state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

private:
  mutable std::mutex mutex_;
  Value value_ = Value::null();
  RuntimeMoveSlotState state_ = RuntimeMoveSlotState::Ready;
  std::uint64_t next_reservation_id_ = 1;
  std::uint64_t active_reservation_id_ = 0;
};

RuntimeMoveSlot::RuntimeMoveSlot(Value value)
    : impl_(std::make_shared<Impl>(std::move(value))) {}

RuntimeMoveSlot::RuntimeMoveSlot(RuntimeMoveSlot &&) noexcept = default;

RuntimeMoveSlot &
RuntimeMoveSlot::operator=(RuntimeMoveSlot &&) noexcept = default;

RuntimeMoveSlot::~RuntimeMoveSlot() = default;

RuntimeMoveResult RuntimeMoveSlot::read() const { return impl_->read(); }

RuntimeMoveResult RuntimeMoveSlot::reserve_move() {
  return impl_->reserve_move();
}

RuntimeMoveResult
RuntimeMoveSlot::commit_move(RuntimeMoveReservation *reservation) {
  return impl_->commit_move(reservation);
}

RuntimeMoveResult
RuntimeMoveSlot::release_move(RuntimeMoveReservation *reservation) {
  return impl_->release_move(reservation);
}

void RuntimeMoveSlot::reset(Value value) { impl_->reset(std::move(value)); }

RuntimeMoveSlotState RuntimeMoveSlot::state() const { return impl_->state(); }

bool RuntimeMoveSlot::moved() const {
  return state() == RuntimeMoveSlotState::Moved;
}

class RuntimeChannel::Impl {
public:
  explicit Impl(std::size_t capacity) : capacity_(capacity) {}

  RuntimeChannelResult send(const Value &value,
                            std::chrono::milliseconds timeout) {
    RuntimeChannelResult result;
    std::optional<RuntimeSyncBoundaryError> shareability_error =
        runtime_value_shareability_error(value);
    if (shareability_error.has_value()) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.isolation_rejections;
      result.error_name = shareability_error->error_name;
      result.message = shareability_error->message;
      return result;
    }

    ChannelPayload payload;
    payload.value = value;
    return send_payload(std::move(payload), nullptr, {}, timeout);
  }

  RuntimeChannelResult send(RuntimeMoveSlot &slot,
                            std::chrono::milliseconds timeout) {
    RuntimeMoveResult reservation_result = slot.reserve_move();
    if (!reservation_result.ok) {
      RuntimeChannelResult result;
      result.error_name = reservation_result.error_name;
      result.message = reservation_result.message;
      return result;
    }

    ChannelPayload payload;
    payload.value = reservation_result.reservation.value;
    payload.moved_transfer = true;
    return send_payload(std::move(payload), &slot,
                        reservation_result.reservation, timeout);
  }

  RuntimeChannelResult recv(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    promote_pending_sends_locked();
    if (recv_waiters_.empty()) {
      std::optional<ChannelPayload> immediate = take_value_locked();
      if (immediate.has_value()) {
        RuntimeChannelResult result;
        result.ok = true;
        result.received = true;
        result.value = adopt_payload(std::move(*immediate));
        ++stats_.receives;
        cv_.notify_all();
        return result;
      }
      if (closed_) {
        return runtime_channel_closed_result();
      }
    }

    const std::uint64_t waiter_id = next_waiter_id_++;
    recv_waiters_.push_back(waiter_id);
    cv_.notify_all();

    const std::optional<std::chrono::steady_clock::time_point> deadline =
        runtime_sync_deadline(timeout);
    while (true) {
      const bool at_front =
          !recv_waiters_.empty() && recv_waiters_.front() == waiter_id;
      if (at_front) {
        promote_pending_sends_locked();
        std::optional<ChannelPayload> payload = take_value_locked();
        if (payload.has_value()) {
          recv_waiters_.pop_front();
          RuntimeChannelResult result;
          result.ok = true;
          result.received = true;
          result.value = adopt_payload(std::move(*payload));
          ++stats_.receives;
          cv_.notify_all();
          return result;
        }
        if (closed_) {
          recv_waiters_.pop_front();
          cv_.notify_all();
          return runtime_channel_closed_result();
        }
      }
      if (current_runtime_task_cancel_requested()) {
        remove_waiter_locked(waiter_id);
        ++stats_.receive_cancellations;
        cv_.notify_all();
        return runtime_channel_cancelled_result(false);
      }
      if (runtime_sync_deadline_expired(deadline)) {
        remove_waiter_locked(waiter_id);
        ++stats_.receive_timeouts;
        cv_.notify_all();
        return runtime_channel_timeout_result(false);
      }
      const std::chrono::steady_clock::duration wait_duration =
          runtime_sync_wait_duration(deadline);
      if (wait_duration <= std::chrono::steady_clock::duration::zero()) {
        continue;
      }
      cv_.wait_for(lock, wait_duration);
    }
  }

  bool close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return false;
    }
    closed_ = true;
    ++stats_.closes;
    cv_.notify_all();
    return true;
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  RuntimeChannelStats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeChannelStats out = stats_;
    out.capacity = static_cast<std::uint64_t>(capacity_);
    out.buffered_values = static_cast<std::uint64_t>(buffer_.size());
    out.pending_senders = static_cast<std::uint64_t>(pending_sends_.size());
    out.pending_receivers = static_cast<std::uint64_t>(recv_waiters_.size());
    out.closed = closed_;
    return out;
  }

private:
  struct ChannelPayload {
    Value value = Value::null();
    bool moved_transfer = false;
  };

  struct PendingSend {
    ChannelPayload payload;
    RuntimeMoveSlot *move_slot = nullptr;
    RuntimeMoveReservation reservation;
    bool consumed = false;
    bool failed = false;
    std::string error_name;
    std::string message;
  };

  RuntimeChannelResult send_payload(ChannelPayload payload,
                                    RuntimeMoveSlot *move_slot,
                                    RuntimeMoveReservation reservation,
                                    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    promote_pending_sends_locked();
    if (closed_) {
      release_move_reservation(move_slot, &reservation);
      return runtime_channel_closed_result();
    }
    if (pending_sends_.empty() && capacity_ > 0 && buffer_.size() < capacity_) {
      if (!commit_payload_move(move_slot, &reservation, &payload)) {
        return move_commit_channel_result(reservation);
      }
      buffer_.push_back(std::move(payload));
      ++stats_.sends;
      cv_.notify_all();
      RuntimeChannelResult result;
      result.ok = true;
      result.sent = true;
      return result;
    }

    const std::shared_ptr<PendingSend> pending =
        std::make_shared<PendingSend>();
    pending->payload = std::move(payload);
    pending->move_slot = move_slot;
    pending->reservation = reservation;
    pending_sends_.push_back(pending);
    cv_.notify_all();

    const std::optional<std::chrono::steady_clock::time_point> deadline =
        runtime_sync_deadline(timeout);
    while (!pending->consumed) {
      if (closed_) {
        remove_pending_send_locked(pending);
        release_move_reservation(move_slot, &pending->reservation);
        cv_.notify_all();
        return runtime_channel_closed_result();
      }
      if (current_runtime_task_cancel_requested()) {
        remove_pending_send_locked(pending);
        release_move_reservation(move_slot, &pending->reservation);
        ++stats_.send_cancellations;
        cv_.notify_all();
        return runtime_channel_cancelled_result(true);
      }
      if (runtime_sync_deadline_expired(deadline)) {
        remove_pending_send_locked(pending);
        release_move_reservation(move_slot, &pending->reservation);
        ++stats_.send_timeouts;
        cv_.notify_all();
        return runtime_channel_timeout_result(true);
      }
      const std::chrono::steady_clock::duration wait_duration =
          runtime_sync_wait_duration(deadline);
      if (wait_duration <= std::chrono::steady_clock::duration::zero()) {
        continue;
      }
      cv_.wait_for(lock, wait_duration);
    }

    if (pending->failed) {
      RuntimeChannelResult result;
      result.error_name = pending->error_name;
      result.message = pending->message;
      return result;
    }

    ++stats_.sends;
    RuntimeChannelResult result;
    result.ok = true;
    result.sent = true;
    return result;
  }

  void remove_pending_send_locked(const std::shared_ptr<PendingSend> &pending) {
    auto found =
        std::find(pending_sends_.begin(), pending_sends_.end(), pending);
    if (found != pending_sends_.end()) {
      pending_sends_.erase(found);
    }
  }

  void remove_waiter_locked(std::uint64_t waiter_id) {
    auto found =
        std::find(recv_waiters_.begin(), recv_waiters_.end(), waiter_id);
    if (found != recv_waiters_.end()) {
      recv_waiters_.erase(found);
    }
  }

  void promote_pending_sends_locked() {
    if (closed_ || capacity_ == 0) {
      return;
    }
    while (buffer_.size() < capacity_ && !pending_sends_.empty()) {
      const std::shared_ptr<PendingSend> pending = pending_sends_.front();
      pending_sends_.pop_front();
      if (pending->consumed) {
        continue;
      }
      if (!commit_pending_move(pending)) {
        continue;
      }
      buffer_.push_back(std::move(pending->payload));
      pending->consumed = true;
    }
  }

  std::optional<ChannelPayload> take_value_locked() {
    if (!buffer_.empty()) {
      ChannelPayload payload = std::move(buffer_.front());
      buffer_.pop_front();
      promote_pending_sends_locked();
      return payload;
    }
    while (!closed_ && !pending_sends_.empty()) {
      const std::shared_ptr<PendingSend> pending = pending_sends_.front();
      pending_sends_.pop_front();
      if (pending->consumed) {
        continue;
      }
      if (!commit_pending_move(pending)) {
        continue;
      }
      pending->consumed = true;
      return std::move(pending->payload);
    }
    return std::nullopt;
  }

  static RuntimeChannelResult
  move_commit_channel_result(const RuntimeMoveReservation &reservation) {
    (void)reservation;
    RuntimeChannelResult result;
    result.error_name = "MoveError";
    result.message = "move reservation could not be committed";
    return result;
  }

  bool commit_payload_move(RuntimeMoveSlot *move_slot,
                           RuntimeMoveReservation *reservation,
                           ChannelPayload *payload) {
    if (move_slot == nullptr || reservation == nullptr ||
        !reservation->active || payload == nullptr) {
      return true;
    }
    RuntimeMoveResult committed = move_slot->commit_move(reservation);
    if (!committed.ok) {
      payload->moved_transfer = false;
      return false;
    }
    payload->value = committed.value;
    payload->moved_transfer = true;
    return true;
  }

  bool commit_pending_move(const std::shared_ptr<PendingSend> &pending) {
    if (pending == nullptr) {
      return false;
    }
    if (!commit_payload_move(pending->move_slot, &pending->reservation,
                             &pending->payload)) {
      pending->consumed = true;
      pending->failed = true;
      pending->error_name = "MoveError";
      pending->message = "move reservation could not be committed";
      cv_.notify_all();
      return false;
    }
    return true;
  }

  static void release_move_reservation(RuntimeMoveSlot *move_slot,
                                       RuntimeMoveReservation *reservation) {
    if (move_slot != nullptr && reservation != nullptr && reservation->active) {
      (void)move_slot->release_move(reservation);
    }
  }

  static Value adopt_payload(ChannelPayload payload) {
    if (payload.moved_transfer) {
      runtime_adopt_moved_value(payload.value);
    }
    return payload.value;
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t capacity_ = 0;
  std::deque<ChannelPayload> buffer_;
  std::deque<std::shared_ptr<PendingSend>> pending_sends_;
  std::deque<std::uint64_t> recv_waiters_;
  RuntimeChannelStats stats_;
  std::uint64_t next_waiter_id_ = 1;
  bool closed_ = false;
};

RuntimeChannel::RuntimeChannel(std::size_t capacity)
    : impl_(std::make_shared<Impl>(capacity)) {}

RuntimeChannel::RuntimeChannel(RuntimeChannel &&) noexcept = default;

RuntimeChannel &RuntimeChannel::operator=(RuntimeChannel &&) noexcept = default;

RuntimeChannel::~RuntimeChannel() = default;

RuntimeChannelResult RuntimeChannel::send(const Value &value,
                                          std::chrono::milliseconds timeout) {
  return impl_->send(value, timeout);
}

RuntimeChannelResult RuntimeChannel::send(RuntimeMoveSlot &slot,
                                          std::chrono::milliseconds timeout) {
  return impl_->send(slot, timeout);
}

RuntimeChannelResult RuntimeChannel::recv(std::chrono::milliseconds timeout) {
  return impl_->recv(timeout);
}

bool RuntimeChannel::close() { return impl_->close(); }

bool RuntimeChannel::closed() const { return impl_->closed(); }

RuntimeChannelStats RuntimeChannel::stats() const { return impl_->stats(); }

RuntimeSelectArm RuntimeSelectArm::recv(RuntimeChannel &channel) {
  RuntimeSelectArm arm;
  arm.kind = RuntimeSelectArmKind::Recv;
  arm.channel = &channel;
  return arm;
}

RuntimeSelectArm RuntimeSelectArm::send(RuntimeChannel &channel, Value value) {
  RuntimeSelectArm arm;
  arm.kind = RuntimeSelectArmKind::Send;
  arm.channel = &channel;
  arm.value = std::move(value);
  return arm;
}

RuntimeSelectArm RuntimeSelectArm::send_moved(RuntimeChannel &channel,
                                              RuntimeMoveSlot &slot) {
  RuntimeSelectArm arm;
  arm.kind = RuntimeSelectArmKind::Send;
  arm.channel = &channel;
  arm.move_slot = &slot;
  return arm;
}

RuntimeSelectArm RuntimeSelectArm::awaitable_arm(RuntimeAwaitable &awaitable) {
  RuntimeSelectArm arm;
  arm.kind = RuntimeSelectArmKind::Await;
  arm.awaitable = &awaitable;
  return arm;
}

RuntimeSelectResult runtime_select(const std::vector<RuntimeSelectArm> &arms,
                                   std::chrono::milliseconds timeout,
                                   bool has_else) {
  static std::atomic<std::uint64_t> select_cursor{0};

  RuntimeSelectResult result;
  if (arms.empty()) {
    if (has_else) {
      result.ok = true;
      result.else_selected = true;
      return result;
    }
    result.error_name = "TypeError";
    result.message = "select expects at least one arm";
    return result;
  }

  const std::optional<std::chrono::steady_clock::time_point> deadline =
      runtime_sync_deadline(timeout);
  while (true) {
    if (current_runtime_task_cancel_requested()) {
      result.cancelled = true;
      result.error_name = "CancelledError";
      result.message = "select cancelled";
      return result;
    }

    const std::size_t start =
        static_cast<std::size_t>(select_cursor.fetch_add(1)) % arms.size();
    for (std::size_t offset = 0; offset < arms.size(); ++offset) {
      const std::size_t index = (start + offset) % arms.size();
      const RuntimeSelectArm &arm = arms[index];
      if (arm.kind == RuntimeSelectArmKind::Await) {
        if (arm.awaitable == nullptr) {
          result.error_name = "TypeError";
          result.message = "select await arm is missing an awaitable";
          return result;
        }
        RuntimeAwaitableResult awaitable_result = arm.awaitable->poll();
        if (!awaitable_result.timed_out) {
          result.ok = awaitable_result.ok;
          result.selected = true;
          result.arm_index = index;
          result.kind = arm.kind;
          result.awaitable_result = std::move(awaitable_result);
          result.error_name = result.awaitable_result.error_name;
          result.message = result.awaitable_result.message;
          result.cancelled = result.awaitable_result.cancelled;
          return result;
        }
        continue;
      }

      if (arm.channel == nullptr) {
        result.error_name = "TypeError";
        result.message = "select arm is missing a channel";
        return result;
      }

      RuntimeChannelResult channel_result;
      if (arm.kind == RuntimeSelectArmKind::Recv) {
        channel_result = arm.channel->recv(std::chrono::milliseconds(0));
      } else if (arm.move_slot != nullptr) {
        channel_result =
            arm.channel->send(*arm.move_slot, std::chrono::milliseconds(0));
      } else {
        channel_result =
            arm.channel->send(arm.value, std::chrono::milliseconds(0));
      }

      if (!channel_result.timed_out) {
        result.ok = channel_result.ok;
        result.selected = true;
        result.arm_index = index;
        result.kind = arm.kind;
        result.channel_result = std::move(channel_result);
        result.error_name = result.channel_result.error_name;
        result.message = result.channel_result.message;
        return result;
      }
    }

    if (has_else) {
      result.ok = true;
      result.else_selected = true;
      return result;
    }
    if (runtime_sync_deadline_expired(deadline)) {
      result.timed_out = true;
      result.error_name = "TimeoutError";
      result.message = "select timed out";
      return result;
    }

    const std::chrono::steady_clock::duration wait_duration =
        runtime_sync_wait_duration(deadline);
    if (wait_duration <= std::chrono::steady_clock::duration::zero()) {
      continue;
    }
    std::this_thread::sleep_for(wait_duration);
  }
}

class RuntimeMutex::Impl {
public:
  RuntimeMutexResult lock(std::chrono::milliseconds timeout) {
    const std::uint64_t owner_id = runtime_sync_owner_id();
    std::unique_lock<std::mutex> lock(mutex_);
    ++stats_.lock_attempts;
    if (locked_ && owner_id_ == owner_id) {
      ++stats_.reentrant_failures;
      RuntimeMutexResult result;
      result.error_name = "DeadlockError";
      result.message = "mutex is non-reentrant";
      return result;
    }
    if (!locked_ && waiters_.empty()) {
      locked_ = true;
      owner_id_ = owner_id;
      ++stats_.locks;
      RuntimeMutexResult result;
      result.ok = true;
      result.locked = true;
      return result;
    }

    ++stats_.contentions;
    const std::uint64_t waiter_id = next_waiter_id_++;
    waiters_.push_back(waiter_id);
    const std::optional<std::chrono::steady_clock::time_point> deadline =
        runtime_sync_deadline(timeout);
    while (true) {
      const bool at_front = !waiters_.empty() && waiters_.front() == waiter_id;
      if (!locked_ && at_front) {
        waiters_.pop_front();
        locked_ = true;
        owner_id_ = owner_id;
        ++stats_.locks;
        cv_.notify_all();
        RuntimeMutexResult result;
        result.ok = true;
        result.locked = true;
        return result;
      }
      if (current_runtime_task_cancel_requested()) {
        remove_waiter_locked(waiter_id);
        ++stats_.lock_cancellations;
        cv_.notify_all();
        return runtime_mutex_cancelled_result();
      }
      if (runtime_sync_deadline_expired(deadline)) {
        remove_waiter_locked(waiter_id);
        ++stats_.lock_timeouts;
        cv_.notify_all();
        return runtime_mutex_timeout_result();
      }
      const std::chrono::steady_clock::duration wait_duration =
          runtime_sync_wait_duration(deadline);
      if (wait_duration <= std::chrono::steady_clock::duration::zero()) {
        continue;
      }
      cv_.wait_for(lock, wait_duration);
    }
  }

  RuntimeMutexResult unlock() {
    const std::uint64_t owner_id = runtime_sync_owner_id();
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeMutexResult result;
    if (!locked_) {
      result.error_name = "RuntimeError";
      result.message = "mutex is not locked";
      return result;
    }
    if (owner_id_ != owner_id) {
      result.error_name = "RuntimeError";
      result.message = "mutex unlock by non-owner";
      return result;
    }
    locked_ = false;
    owner_id_ = 0;
    ++stats_.unlocks;
    result.ok = true;
    result.unlocked = true;
    cv_.notify_all();
    return result;
  }

  bool locked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return locked_;
  }

  RuntimeMutexStats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeMutexStats out = stats_;
    out.waiting_lockers = static_cast<std::uint64_t>(waiters_.size());
    out.owner_id = owner_id_;
    out.locked = locked_;
    return out;
  }

private:
  void remove_waiter_locked(std::uint64_t waiter_id) {
    auto found = std::find(waiters_.begin(), waiters_.end(), waiter_id);
    if (found != waiters_.end()) {
      waiters_.erase(found);
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::uint64_t> waiters_;
  RuntimeMutexStats stats_;
  std::uint64_t owner_id_ = 0;
  std::uint64_t next_waiter_id_ = 1;
  bool locked_ = false;
};

RuntimeMutex::RuntimeMutex() : impl_(std::make_shared<Impl>()) {}

RuntimeMutex::RuntimeMutex(RuntimeMutex &&) noexcept = default;

RuntimeMutex &RuntimeMutex::operator=(RuntimeMutex &&) noexcept = default;

RuntimeMutex::~RuntimeMutex() = default;

RuntimeMutexResult RuntimeMutex::lock(std::chrono::milliseconds timeout) {
  return impl_->lock(timeout);
}

RuntimeMutexResult RuntimeMutex::unlock() { return impl_->unlock(); }

bool RuntimeMutex::locked() const { return impl_->locked(); }

RuntimeMutexStats RuntimeMutex::stats() const { return impl_->stats(); }

class RuntimeAtomic::Impl {
public:
  explicit Impl(std::int64_t value) : value_(value) {}

  std::int64_t get() const { return value_.load(); }

  void set(std::int64_t value) { value_.store(value); }

  bool compare_and_set(std::int64_t expected, std::int64_t desired) {
    return value_.compare_exchange_strong(expected, desired);
  }

private:
  std::atomic<std::int64_t> value_;
};

RuntimeAtomic::RuntimeAtomic(std::int64_t value)
    : impl_(std::make_shared<Impl>(value)) {}

RuntimeAtomic::RuntimeAtomic(RuntimeAtomic &&) noexcept = default;

RuntimeAtomic &RuntimeAtomic::operator=(RuntimeAtomic &&) noexcept = default;

RuntimeAtomic::~RuntimeAtomic() = default;

std::int64_t RuntimeAtomic::get() const { return impl_->get(); }

void RuntimeAtomic::set(std::int64_t value) { impl_->set(value); }

bool RuntimeAtomic::compare_and_set(std::int64_t expected,
                                    std::int64_t desired) {
  return impl_->compare_and_set(expected, desired);
}

class RuntimeScheduler::Impl {
public:
  explicit Impl(RuntimeSchedulerConfig config)
      : worker_count_(normalize_worker_count(config.worker_count)),
        first_worker_id_(config.first_worker_id == 0 ? 1
                                                     : config.first_worker_id),
        local_queues_(worker_count_) {
    stats_.worker_count = static_cast<std::uint64_t>(worker_count_);
  }

  ~Impl() { shutdown(); }

  void start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || shutdown_requested_) {
      return;
    }
    started_ = true;
    workers_.reserve(worker_count_);
    for (std::size_t index = 0; index < worker_count_; ++index) {
      workers_.emplace_back([this, index]() { worker_loop(index); });
    }
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_requested_) {
        return;
      }
      shutdown_requested_ = true;
    }
    cv_.notify_all();
    for (std::thread &worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

  std::uint64_t spawn_strand(StrandFunction function) {
    return spawn_impl(std::chrono::milliseconds(0), std::move(function), false,
                      RuntimeTaskOptions{});
  }

  std::uint64_t spawn_sleeping_strand(std::chrono::milliseconds delay,
                                      StrandFunction function) {
    return spawn_impl(delay, std::move(function), true, RuntimeTaskOptions{});
  }

  std::uint64_t spawn_task(StrandFunction function) {
    return spawn_impl(std::chrono::milliseconds(0), std::move(function), false,
                      RuntimeTaskOptions{});
  }

  std::uint64_t spawn_task(RuntimeTaskOptions options,
                           StrandFunction function) {
    return spawn_impl(std::chrono::milliseconds(0), std::move(function), false,
                      options);
  }

  std::uint64_t spawn_sleeping_task(std::chrono::milliseconds delay,
                                    StrandFunction function) {
    return spawn_impl(delay, std::move(function), true, RuntimeTaskOptions{});
  }

  std::uint64_t spawn_sleeping_task(std::chrono::milliseconds delay,
                                    RuntimeTaskOptions options,
                                    StrandFunction function) {
    return spawn_impl(delay, std::move(function), true, options);
  }

  bool wake_strand(std::uint64_t strand_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = strands_.find(strand_id);
    if (found == strands_.end()) {
      return false;
    }
    StrandRecord &strand = found->second;
    if (is_terminal_state(strand.state)) {
      return false;
    }
    if (strand.state != RuntimeStrandState::Sleeping || strand.wake_pending ||
        strand.queued) {
      ++stats_.coalesced_wakes;
      return false;
    }

    ++strand.wake_generation;
    strand.wake_pending = true;
    strand.state = RuntimeStrandState::Runnable;
    ++strand.explicit_wakes;
    ++stats_.explicit_wakes;
    enqueue_runnable_locked(strand_id, std::nullopt);
    cv_.notify_one();
    return true;
  }

  bool cancel_task(std::uint64_t task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool cancelled = request_cancel_locked(task_id);
    cv_.notify_all();
    return cancelled;
  }

  bool task_cancel_requested(std::uint64_t task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = strands_.find(task_id);
    return found != strands_.end() &&
           found->second.cancellation_requested->load();
  }

  RuntimeTaskJoinResult join_task(std::uint64_t task_id,
                                  std::chrono::milliseconds timeout) {
    RuntimeTaskJoinResult result;
    result.task_id = task_id;

    std::unique_lock<std::mutex> lock(mutex_);
    if (strands_.find(task_id) == strands_.end()) {
      result.error_name = "LifetimeError";
      result.message = "task handle is not active";
      return result;
    }

    const std::uint64_t caller_id = current_runtime_task_id();
    if (caller_id == task_id) {
      result.error_name = "DeadlockError";
      result.message = "task cannot join itself";
      return result;
    }

    ++stats_.task_joins;
    bool caller_waiting = false;
    if (caller_id != 0) {
      auto caller = strands_.find(caller_id);
      if (caller != strands_.end() &&
          caller->second.state == RuntimeStrandState::Running) {
        caller->second.state = RuntimeStrandState::Waiting;
        caller->second.worker_id = 0;
        caller_waiting = true;
        ++stats_.task_wait_state_entries;
        if (running_count_ > 0) {
          --running_count_;
        }
        cv_.notify_all();
      }
    }

    const auto target_is_terminal = [this, task_id]() {
      const auto found = strands_.find(task_id);
      return found == strands_.end() || is_terminal_state(found->second.state);
    };
    const auto caller_cancelled = [this, caller_id, caller_waiting]() {
      if (!caller_waiting || caller_id == 0) {
        return false;
      }
      const auto found = strands_.find(caller_id);
      return found != strands_.end() &&
             found->second.cancellation_requested->load();
    };
    const auto wait_done = [&]() {
      return target_is_terminal() || caller_cancelled();
    };

    bool completed = false;
    if (timeout == std::chrono::milliseconds::max()) {
      cv_.wait(lock, wait_done);
      completed = target_is_terminal();
    } else {
      completed =
          cv_.wait_for(lock, timeout, wait_done) && target_is_terminal();
    }

    if (caller_waiting) {
      auto caller = strands_.find(caller_id);
      if (caller != strands_.end() &&
          caller->second.state == RuntimeStrandState::Waiting) {
        caller->second.state = RuntimeStrandState::Running;
        caller->second.worker_id = current_runtime_worker_id();
        ++running_count_;
      }
    }

    if (!completed) {
      if (caller_cancelled()) {
        result.cancelled = true;
        result.error_name = "CancelledError";
        result.message = "task join cancelled";
      } else {
        result.timed_out = true;
        result.error_name = "TimeoutError";
        result.message = "task join timed out";
        ++stats_.task_join_timeouts;
      }
      const auto found = strands_.find(task_id);
      if (found != strands_.end()) {
        result.state = found->second.state;
      }
      return result;
    }

    fill_join_result_locked(task_id, result);
    return result;
  }

  bool wait_until_idle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this]() { return idle_locked(); });
  }

  RuntimeSchedulerStats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeSchedulerStats out = stats_;
    out.runnable_queue_depth = runnable_queue_depth_locked();
    out.sleeping_strands = 0;
    for (const auto &[strand_id, strand] : strands_) {
      (void)strand_id;
      if (strand.state == RuntimeStrandState::Sleeping) {
        ++out.sleeping_strands;
      }
    }
    out.timer_queue_depth = out.sleeping_strands;
    return out;
  }

  std::optional<RuntimeStrandSnapshot>
  strand_snapshot(std::uint64_t strand_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = strands_.find(strand_id);
    if (found == strands_.end()) {
      return std::nullopt;
    }
    const StrandRecord &strand = found->second;
    RuntimeStrandSnapshot snapshot;
    snapshot.strand_id = strand.strand_id;
    snapshot.state = strand.state;
    snapshot.worker_id = strand.worker_id;
    snapshot.wake_generation = strand.wake_generation;
    snapshot.wake_pending = strand.wake_pending;
    snapshot.explicit_wakes = strand.explicit_wakes;
    snapshot.timer_wakes = strand.timer_wakes;
    return snapshot;
  }

  std::optional<RuntimeTaskSnapshot>
  task_snapshot(std::uint64_t task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = strands_.find(task_id);
    if (found == strands_.end()) {
      return std::nullopt;
    }
    const StrandRecord &task = found->second;
    RuntimeTaskSnapshot snapshot;
    snapshot.task_id = task.strand_id;
    snapshot.parent_task_id = task.parent_task_id;
    snapshot.state = task.state;
    snapshot.worker_id = task.worker_id;
    snapshot.cancellation_requested = task.cancellation_requested->load();
    snapshot.total_children =
        static_cast<std::uint64_t>(task.child_task_ids.size());
    snapshot.active_children = active_child_count_locked(task);
    snapshot.error_name = task.error.error_name;
    snapshot.message = task.error.message;
    return snapshot;
  }

private:
  struct TaskError {
    std::string error_name;
    std::string message;
  };

  struct TaskCompletion {
    RuntimeStrandState state = RuntimeStrandState::Done;
    TaskError error;
  };

  struct StrandRecord {
    std::uint64_t strand_id = 0;
    RuntimeStrandState state = RuntimeStrandState::New;
    StrandFunction function;
    std::uint64_t worker_id = 0;
    std::uint64_t wake_generation = 0;
    bool wake_pending = false;
    bool queued = false;
    std::uint64_t explicit_wakes = 0;
    std::uint64_t timer_wakes = 0;
    std::uint64_t parent_task_id = 0;
    std::unordered_set<std::uint64_t> child_task_ids;
    std::vector<std::uint64_t> child_task_order;
    std::shared_ptr<std::atomic<bool>> cancellation_requested =
        std::make_shared<std::atomic<bool>>(false);
    RuntimeStrandState pending_completion_state = RuntimeStrandState::New;
    TaskError pending_error;
    TaskError error;
    std::optional<TaskError> first_child_error;
    RuntimeSupervisorPolicy supervisor_policy =
        RuntimeSupervisorPolicy::CancelScope;
  };

  struct TimerEntry {
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t strand_id = 0;
    std::uint64_t generation = 0;
  };

  struct TimerEntryLater {
    bool operator()(const TimerEntry &left, const TimerEntry &right) const {
      return left.deadline > right.deadline;
    }
  };

  static std::size_t normalize_worker_count(std::size_t worker_count) {
    if (worker_count != 0) {
      return worker_count;
    }
    const unsigned int hardware = std::thread::hardware_concurrency();
    return hardware == 0 ? 2 : static_cast<std::size_t>(hardware);
  }

  static bool is_terminal_state(RuntimeStrandState state) {
    return state == RuntimeStrandState::Done ||
           state == RuntimeStrandState::Failed ||
           state == RuntimeStrandState::Cancelled;
  }

  static TaskError cancelled_error() {
    return TaskError{"CancelledError", "task cancelled"};
  }

  std::uint64_t spawn_impl(std::chrono::milliseconds delay,
                           StrandFunction function, bool may_sleep,
                           RuntimeTaskOptions options) {
    if (!function) {
      function = []() {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t strand_id = next_strand_id_++;
    std::uint64_t parent_task_id = current_runtime_task_id();
    auto parent = strands_.find(parent_task_id);
    if (parent == strands_.end() || is_terminal_state(parent->second.state)) {
      parent_task_id = 0;
    }

    StrandRecord strand;
    strand.strand_id = strand_id;
    strand.parent_task_id = parent_task_id;
    strand.function = std::move(function);
    strand.state = RuntimeStrandState::Runnable;
    strand.supervisor_policy = options.policy;
    strands_[strand_id] = std::move(strand);
    ++stats_.strands_created;
    ++stats_.tasks_created;

    if (parent_task_id != 0) {
      strands_[parent_task_id].child_task_ids.insert(strand_id);
      strands_[parent_task_id].child_task_order.push_back(strand_id);
      ++stats_.structured_child_tasks;
      if (strands_[parent_task_id].cancellation_requested->load()) {
        strands_[strand_id].cancellation_requested->store(true);
      }
    }

    if (may_sleep && delay.count() > 0) {
      StrandRecord &stored = strands_[strand_id];
      stored.state = RuntimeStrandState::Sleeping;
      ++stored.wake_generation;
      timers_.push(TimerEntry{std::chrono::steady_clock::now() + delay,
                              strand_id, stored.wake_generation});
    } else {
      enqueue_runnable_locked(strand_id, std::nullopt);
    }
    cv_.notify_one();
    return strand_id;
  }

  void enqueue_runnable_locked(std::uint64_t strand_id,
                               std::optional<std::size_t> worker_index) {
    auto found = strands_.find(strand_id);
    if (found == strands_.end()) {
      return;
    }
    StrandRecord &strand = found->second;
    if (strand.queued || (strand.state != RuntimeStrandState::Runnable &&
                          strand.state != RuntimeStrandState::Sleeping)) {
      return;
    }
    strand.state = RuntimeStrandState::Runnable;
    strand.queued = true;
    if (worker_index.has_value() && !local_queues_.empty()) {
      local_queues_[*worker_index % local_queues_.size()].push_back(strand_id);
      ++stats_.local_queue_enqueues;
    } else {
      global_queue_.push_back(strand_id);
      ++stats_.global_queue_enqueues;
    }
  }

  void promote_expired_timers_locked(std::size_t worker_index) {
    const auto now = std::chrono::steady_clock::now();
    while (!timers_.empty() && timers_.top().deadline <= now) {
      const TimerEntry entry = timers_.top();
      timers_.pop();
      auto found = strands_.find(entry.strand_id);
      if (found == strands_.end() ||
          found->second.state != RuntimeStrandState::Sleeping ||
          found->second.wake_generation != entry.generation) {
        ++stats_.stale_timer_wakes;
        continue;
      }
      StrandRecord &strand = found->second;
      strand.wake_pending = true;
      ++strand.timer_wakes;
      ++stats_.timer_wakes;
      enqueue_runnable_locked(entry.strand_id, worker_index);
    }
  }

  std::uint64_t next_runnable_locked(std::size_t worker_index) {
    promote_expired_timers_locked(worker_index);
    while (!local_queues_[worker_index].empty()) {
      const std::uint64_t strand_id = local_queues_[worker_index].front();
      local_queues_[worker_index].pop_front();
      if (prepare_to_run_locked(strand_id, worker_index)) {
        return strand_id;
      }
    }
    while (!global_queue_.empty()) {
      const std::uint64_t strand_id = global_queue_.front();
      global_queue_.pop_front();
      if (prepare_to_run_locked(strand_id, worker_index)) {
        return strand_id;
      }
    }
    return 0;
  }

  bool prepare_to_run_locked(std::uint64_t strand_id,
                             std::size_t worker_index) {
    auto found = strands_.find(strand_id);
    if (found == strands_.end() ||
        found->second.state != RuntimeStrandState::Runnable) {
      return false;
    }
    StrandRecord &strand = found->second;
    strand.state = RuntimeStrandState::Running;
    strand.queued = false;
    strand.wake_pending = false;
    strand.worker_id =
        first_worker_id_ + static_cast<std::uint64_t>(worker_index);
    ++running_count_;
    ++stats_.worker_dequeues;
    if (running_count_ > stats_.max_parallel_running) {
      stats_.max_parallel_running = running_count_;
    }
    return true;
  }

  bool has_active_children_locked(const StrandRecord &task) const {
    return active_child_count_locked(task) != 0;
  }

  std::uint64_t active_child_count_locked(const StrandRecord &task) const {
    std::uint64_t active = 0;
    for (const std::uint64_t child_id : task.child_task_ids) {
      const auto child = strands_.find(child_id);
      if (child != strands_.end() && !is_terminal_state(child->second.state)) {
        ++active;
      }
    }
    return active;
  }

  void mark_cancel_requested_locked(StrandRecord &task) {
    if (!task.cancellation_requested->exchange(true)) {
      ++stats_.task_cancellation_requests;
    }
  }

  bool request_cancel_locked(std::uint64_t task_id,
                             std::uint64_t excluded_child_id = 0) {
    auto found = strands_.find(task_id);
    if (found == strands_.end() || is_terminal_state(found->second.state)) {
      return false;
    }

    StrandRecord &task = found->second;
    mark_cancel_requested_locked(task);
    cancel_active_children_locked(task_id, excluded_child_id);

    if (task.state == RuntimeStrandState::Running ||
        (task.state == RuntimeStrandState::Waiting &&
         task.pending_completion_state == RuntimeStrandState::New)) {
      return true;
    }

    TaskCompletion completion;
    completion.state = RuntimeStrandState::Cancelled;
    completion.error = cancelled_error();
    finish_or_wait_locked(task_id, completion);
    return true;
  }

  void cancel_active_children_locked(std::uint64_t parent_task_id,
                                     std::uint64_t excluded_child_id = 0) {
    auto found = strands_.find(parent_task_id);
    if (found == strands_.end()) {
      return;
    }
    std::vector<std::uint64_t> children(found->second.child_task_ids.begin(),
                                        found->second.child_task_ids.end());
    for (const std::uint64_t child_id : children) {
      if (child_id != excluded_child_id) {
        request_cancel_locked(child_id);
      }
    }
  }

  std::uint64_t cancel_children_after_locked(std::uint64_t parent_task_id,
                                             std::uint64_t failed_child_id) {
    auto found = strands_.find(parent_task_id);
    if (found == strands_.end()) {
      return 0;
    }
    bool after_failed = false;
    std::uint64_t cancelled = 0;
    for (const std::uint64_t child_id : found->second.child_task_order) {
      if (child_id == failed_child_id) {
        after_failed = true;
        continue;
      }
      if (after_failed && request_cancel_locked(child_id)) {
        ++cancelled;
      }
    }
    return cancelled;
  }

  void finish_or_wait_locked(std::uint64_t task_id,
                             const TaskCompletion &completion) {
    auto found = strands_.find(task_id);
    if (found == strands_.end() || is_terminal_state(found->second.state)) {
      return;
    }

    StrandRecord &task = found->second;
    task.pending_completion_state = completion.state;
    task.pending_error = completion.error;
    task.queued = false;
    task.wake_pending = false;

    if (completion.state == RuntimeStrandState::Failed ||
        completion.state == RuntimeStrandState::Cancelled) {
      cancel_active_children_locked(task_id);
    }

    if (has_active_children_locked(task)) {
      if (task.state != RuntimeStrandState::Waiting) {
        ++stats_.task_wait_state_entries;
      }
      task.state = RuntimeStrandState::Waiting;
      task.worker_id = 0;
      return;
    }

    finalize_task_locked(task_id);
  }

  void finalize_task_locked(std::uint64_t task_id) {
    auto found = strands_.find(task_id);
    if (found == strands_.end() || is_terminal_state(found->second.state)) {
      return;
    }

    StrandRecord &task = found->second;
    RuntimeStrandState final_state = task.pending_completion_state;
    TaskError final_error = task.pending_error;
    if (final_state == RuntimeStrandState::New) {
      final_state = task.cancellation_requested->load()
                        ? RuntimeStrandState::Cancelled
                        : RuntimeStrandState::Done;
    }
    if (task.first_child_error.has_value() &&
        final_state != RuntimeStrandState::Failed) {
      final_state = RuntimeStrandState::Failed;
      final_error = *task.first_child_error;
    }
    if (final_state == RuntimeStrandState::Cancelled &&
        final_error.error_name.empty()) {
      final_error = cancelled_error();
    }
    if (final_state == RuntimeStrandState::Failed &&
        final_error.error_name.empty()) {
      final_error = TaskError{"RuntimeError", "task failed"};
    }

    task.state = final_state;
    task.error = final_error;
    task.worker_id = 0;
    task.queued = false;
    task.wake_pending = false;

    if (final_state == RuntimeStrandState::Done) {
      ++stats_.strands_completed;
      ++stats_.tasks_completed;
    } else if (final_state == RuntimeStrandState::Cancelled) {
      ++stats_.tasks_cancelled;
    } else if (final_state == RuntimeStrandState::Failed) {
      ++stats_.strands_failed;
      ++stats_.tasks_failed;
    }

    propagate_child_terminal_locked(task_id);
  }

  void propagate_child_terminal_locked(std::uint64_t task_id) {
    auto child = strands_.find(task_id);
    if (child == strands_.end() || child->second.parent_task_id == 0) {
      return;
    }

    const std::uint64_t parent_task_id = child->second.parent_task_id;
    auto parent = strands_.find(parent_task_id);
    if (parent == strands_.end() || is_terminal_state(parent->second.state)) {
      return;
    }

    if (child->second.state == RuntimeStrandState::Failed &&
        !parent->second.first_child_error.has_value()) {
      parent->second.first_child_error = child->second.error;
      switch (parent->second.supervisor_policy) {
      case RuntimeSupervisorPolicy::CancelScope:
        mark_cancel_requested_locked(parent->second);
        ++stats_.first_failure_cancellations;
        cancel_active_children_locked(parent_task_id, task_id);
        break;
      case RuntimeSupervisorPolicy::OneForOne:
        ++stats_.supervisor_one_for_one_failures;
        break;
      case RuntimeSupervisorPolicy::OneForAll:
        ++stats_.first_failure_cancellations;
        cancel_active_children_locked(parent_task_id, task_id);
        ++stats_.supervisor_one_for_all_cancellations;
        break;
      case RuntimeSupervisorPolicy::RestForOne:
        ++stats_.first_failure_cancellations;
        stats_.supervisor_rest_for_one_cancellations +=
            cancel_children_after_locked(parent_task_id, task_id);
        break;
      }
    }

    if (parent->second.state == RuntimeStrandState::Waiting &&
        parent->second.pending_completion_state != RuntimeStrandState::New &&
        !has_active_children_locked(parent->second)) {
      finalize_task_locked(parent_task_id);
    }
  }

  void fill_join_result_locked(std::uint64_t task_id,
                               RuntimeTaskJoinResult &result) const {
    const auto found = strands_.find(task_id);
    if (found == strands_.end()) {
      result.error_name = "LifetimeError";
      result.message = "task handle is not active";
      return;
    }

    const StrandRecord &task = found->second;
    result.joined = true;
    result.state = task.state;
    if (task.state == RuntimeStrandState::Done) {
      result.ok = true;
      return;
    }
    result.ok = false;
    result.cancelled = task.state == RuntimeStrandState::Cancelled;
    result.error_name = task.error.error_name;
    result.message = task.error.message;
    if (result.error_name.empty() && result.cancelled) {
      result.error_name = "CancelledError";
      result.message = "task cancelled";
    }
    if (result.error_name.empty()) {
      result.error_name = "RuntimeError";
      result.message = "task failed";
    }
  }

  void worker_loop(std::size_t worker_index) {
    const std::uint64_t worker_id =
        first_worker_id_ + static_cast<std::uint64_t>(worker_index);
    RuntimeWorkerScope worker_scope(worker_id);
    while (true) {
      std::uint64_t strand_id = 0;
      StrandFunction function;
      std::shared_ptr<std::atomic<bool>> cancellation_requested;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!shutdown_requested_) {
          strand_id = next_runnable_locked(worker_index);
          if (strand_id != 0) {
            function = strands_[strand_id].function;
            cancellation_requested = strands_[strand_id].cancellation_requested;
            break;
          }
          if (timers_.empty()) {
            cv_.wait(lock);
          } else {
            cv_.wait_until(lock, timers_.top().deadline);
          }
        }
        if (shutdown_requested_) {
          return;
        }
      }

      TaskCompletion completion;
      completion.state = RuntimeStrandState::Done;
      {
        RuntimeStrandScope strand_scope(strand_id);
        RuntimeTaskScope task_scope(strand_id, cancellation_requested.get());
        try {
          function();
        } catch (const RuntimeTaskCancelled &) {
          completion.state = RuntimeStrandState::Cancelled;
          completion.error = cancelled_error();
        } catch (const RuntimeTaskFailure &failure) {
          completion.state = RuntimeStrandState::Failed;
          completion.error = TaskError{failure.error_name(), failure.message()};
        } catch (const std::exception &error) {
          completion.state = RuntimeStrandState::Failed;
          completion.error = TaskError{"RuntimeError", error.what()};
        } catch (...) {
          completion.state = RuntimeStrandState::Failed;
          completion.error = TaskError{"RuntimeError", "task failed"};
        }
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = strands_.find(strand_id);
        if (found != strands_.end()) {
          found->second.worker_id = 0;
        }
        if (running_count_ > 0) {
          --running_count_;
        }
        finish_or_wait_locked(strand_id, completion);
      }
      cv_.notify_all();
    }
  }

  bool idle_locked() const {
    if (running_count_ != 0 || !global_queue_.empty()) {
      return false;
    }
    for (const auto &queue : local_queues_) {
      if (!queue.empty()) {
        return false;
      }
    }
    return stats_.strands_completed + stats_.strands_failed +
               stats_.tasks_cancelled ==
           stats_.strands_created;
  }

  std::uint64_t runnable_queue_depth_locked() const {
    std::uint64_t depth = static_cast<std::uint64_t>(global_queue_.size());
    for (const auto &queue : local_queues_) {
      depth += static_cast<std::uint64_t>(queue.size());
    }
    return depth;
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t worker_count_ = 0;
  std::uint64_t first_worker_id_ = 1;
  std::vector<std::thread> workers_;
  std::vector<std::deque<std::uint64_t>> local_queues_;
  std::deque<std::uint64_t> global_queue_;
  std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerEntryLater>
      timers_;
  std::unordered_map<std::uint64_t, StrandRecord> strands_;
  RuntimeSchedulerStats stats_;
  std::uint64_t next_strand_id_ = 1;
  std::uint64_t running_count_ = 0;
  bool started_ = false;
  bool shutdown_requested_ = false;
};

RuntimeScheduler::RuntimeScheduler(std::size_t worker_count)
    : RuntimeScheduler(RuntimeSchedulerConfig{worker_count, 1}) {}

RuntimeScheduler::RuntimeScheduler(RuntimeSchedulerConfig config)
    : impl_(std::make_shared<Impl>(config)) {
  impl_->start();
}

RuntimeScheduler::RuntimeScheduler(RuntimeScheduler &&) noexcept = default;

RuntimeScheduler &
RuntimeScheduler::operator=(RuntimeScheduler &&) noexcept = default;

RuntimeScheduler::~RuntimeScheduler() = default;

void RuntimeScheduler::start() { impl_->start(); }

void RuntimeScheduler::shutdown() { impl_->shutdown(); }

std::uint64_t RuntimeScheduler::spawn_strand(StrandFunction function) {
  return impl_->spawn_strand(std::move(function));
}

std::uint64_t
RuntimeScheduler::spawn_sleeping_strand(std::chrono::milliseconds delay,
                                        StrandFunction function) {
  return impl_->spawn_sleeping_strand(delay, std::move(function));
}

bool RuntimeScheduler::wake_strand(std::uint64_t strand_id) {
  return impl_->wake_strand(strand_id);
}

std::uint64_t RuntimeScheduler::spawn_task(StrandFunction function) {
  return impl_->spawn_task(std::move(function));
}

std::uint64_t RuntimeScheduler::spawn_task(RuntimeTaskOptions options,
                                           StrandFunction function) {
  return impl_->spawn_task(options, std::move(function));
}

std::uint64_t
RuntimeScheduler::spawn_sleeping_task(std::chrono::milliseconds delay,
                                      StrandFunction function) {
  return impl_->spawn_sleeping_task(delay, std::move(function));
}

std::uint64_t
RuntimeScheduler::spawn_sleeping_task(std::chrono::milliseconds delay,
                                      RuntimeTaskOptions options,
                                      StrandFunction function) {
  return impl_->spawn_sleeping_task(delay, options, std::move(function));
}

bool RuntimeScheduler::cancel_task(std::uint64_t task_id) {
  return impl_->cancel_task(task_id);
}

bool RuntimeScheduler::task_cancel_requested(std::uint64_t task_id) const {
  return impl_->task_cancel_requested(task_id);
}

RuntimeTaskJoinResult
RuntimeScheduler::join_task(std::uint64_t task_id,
                            std::chrono::milliseconds timeout) {
  return impl_->join_task(task_id, timeout);
}

bool RuntimeScheduler::wait_until_idle(std::chrono::milliseconds timeout) {
  return impl_->wait_until_idle(timeout);
}

RuntimeSchedulerStats RuntimeScheduler::stats() const { return impl_->stats(); }

std::optional<RuntimeStrandSnapshot>
RuntimeScheduler::strand_snapshot(std::uint64_t strand_id) const {
  return impl_->strand_snapshot(strand_id);
}

std::optional<RuntimeTaskSnapshot>
RuntimeScheduler::task_snapshot(std::uint64_t task_id) const {
  return impl_->task_snapshot(task_id);
}

class RuntimeHeap::Impl
    : public std::enable_shared_from_this<RuntimeHeap::Impl> {
public:
  struct RemoteFree {
    void *ptr = nullptr;
    void (*destroy)(void *) = nullptr;
    std::uint64_t owner_worker_id = 0;
    HeapObjectKind kind = HeapObjectKind::Instance;
    std::uint64_t allocation_id = 0;
  };

  struct ArenaState {
    std::uint64_t allocations = 0;
    std::uint64_t live_objects = 0;
    std::deque<RemoteFree> remote_frees;
  };

  struct ObjectRecord {
    void *ptr = nullptr;
    HeapObjectKind kind = HeapObjectKind::Instance;
    std::uint64_t owner_worker_id = 0;
    std::uint64_t allocation_id = 0;
    std::size_t allocation_size = 0;
    bool logical_live = true;
  };

  struct PinRecord {
    std::uint64_t pin_id = 0;
    std::uint64_t pin_epoch = 0;
    std::uint64_t allocation_id = 0;
    RuntimePinViewKind view_kind = RuntimePinViewKind::Opaque;
    RuntimePinPermission permissions = RuntimePinPermission::ReadOnly;
    OwnerToken owner;
    ObjectGeneration generation = ObjectGeneration::Young;
    Value value = Value::null();
    bool active = false;
  };

  struct OpaqueHandleRecord {
    std::uint64_t handle_id = 0;
    std::uint64_t pin_id = 0;
    std::uint64_t pin_epoch = 0;
    std::uint64_t allocation_id = 0;
    HeapObjectKind kind = HeapObjectKind::Instance;
    bool active = false;
  };

  struct NativeWaitRecord {
    std::uint64_t wait_id = 0;
    std::uint64_t pin_id = 0;
    std::uint64_t pin_epoch = 0;
    bool active = false;
    bool cancellation_requested = false;
  };

  ~Impl() { drain_all_remote_frees(); }

  template <typename T, typename Init>
  std::shared_ptr<T> allocate(HeapObjectKind kind, Init init) {
    auto *raw = new T();
    const std::uint64_t worker_id = current_runtime_worker_id();
    const std::size_t allocation_size = sizeof(T);
    const std::uint64_t allocation_id = reserve_allocation_id();
    raw->header.kind = kind;
    raw->header.owner.strand_id = current_runtime_owner_strand_id();
    raw->header.allocation_id = allocation_id;
    raw->header.arena_worker_id = worker_id;
    raw->header.allocation_size = allocation_size;
    raw->header.generation = ObjectGeneration::Young;
    init(*raw);

    std::shared_ptr<Impl> impl = shared_from_this();
    std::shared_ptr<T> handle(
        raw, [impl, worker_id, kind, allocation_id](T *ptr) {
          impl->release({ptr, destroy<T>, worker_id, kind, allocation_id});
        });
    record_allocation(worker_id, kind, allocation_size, allocation_id, raw);
    return handle;
  }

  template <typename T> std::shared_ptr<T> allocate(HeapObjectKind kind) {
    return allocate<T>(kind, [](T &) {});
  }

  std::uint64_t drain_remote_frees(std::uint64_t worker_id) {
    std::deque<RemoteFree> pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ArenaState &arena = arena_for_worker(worker_id);
      pending.swap(arena.remote_frees);
      const std::uint64_t count = static_cast<std::uint64_t>(pending.size());
      if (count == 0) {
        return 0;
      }
      stats_.remote_queue_depth -= count;
      stats_.remote_frees_drained += count;
      for (const RemoteFree &entry : pending) {
        const auto record = objects_.find(entry.allocation_id);
        if (record == objects_.end() || record->second.logical_live) {
          decrement_live_locked(arena, 1);
        }
        objects_.erase(entry.allocation_id);
        remove_remembered_edges_for_locked(entry.allocation_id);
      }
    }

    RuntimeWorkerScope owner_scope(worker_id);
    for (const RemoteFree &entry : pending) {
      entry.destroy(entry.ptr);
    }
    return static_cast<std::uint64_t>(pending.size());
  }

  RuntimeWriteBarrierResult write_barrier(const Value &owner,
                                          const Value &value) {
    RuntimeWriteBarrierResult out;
    const ObjHeader *owner_header = heap_header_from_value(owner);
    const ObjHeader *value_header = heap_header_from_value(value);
    if (owner_header == nullptr) {
      return out;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.write_barriers;

    const std::optional<std::string> owner_error =
        lifecycle_access_error_name(*owner_header);
    if (owner_error.has_value()) {
      ++stats_.write_barrier_rejected_lifetime;
      out.ok = false;
      out.error_name = *owner_error;
      out.message = lifecycle_access_error_message(*owner_error);
      return out;
    }
    if (value_header == nullptr) {
      return out;
    }

    const std::optional<std::string> value_error =
        lifecycle_access_error_name(*value_header);
    if (value_error.has_value()) {
      ++stats_.write_barrier_rejected_lifetime;
      out.ok = false;
      out.error_name = *value_error;
      out.message = lifecycle_access_error_message(*value_error);
      return out;
    }

    const bool owner_is_shared =
        owner_header->generation == ObjectGeneration::Shared ||
        owner_header->owner.kind == OwnerTokenKind::Shareable ||
        (owner_header->flags & kObjectFlagShareable) != 0U;
    const bool value_is_confined =
        value_header->owner.kind == OwnerTokenKind::Confined &&
        value_header->generation != ObjectGeneration::Shared;
    if (owner_is_shared && value_is_confined) {
      ++stats_.write_barrier_rejected_isolation;
      out.ok = false;
      out.error_name = "IsolationError";
      out.message = "shared object cannot reference confined object";
      return out;
    }

    if (owner_header->generation == ObjectGeneration::Mature &&
        value_header->generation == ObjectGeneration::Young &&
        owner_header->allocation_id != 0 && value_header->allocation_id != 0) {
      remembered_set_[owner_header->allocation_id].insert(
          value_header->allocation_id);
      ++stats_.write_barrier_remembered;
      out.remembered = true;
    }
    return out;
  }

  RuntimeGcResult collect_garbage(const std::vector<Value> &roots,
                                  RuntimeGcCycle cycle, bool from_safepoint) {
    RuntimeGcResult result;
    result.cycle = cycle;
    result.roots = static_cast<std::uint64_t>(roots.size());
    std::vector<Value> deferred_payload_release;

    std::lock_guard<std::mutex> lock(mutex_);
    pending_gc_cycle_.reset();
    ++stats_.gc_cycles;
    if (from_safepoint) {
      ++stats_.gc_safepoint_collections;
    }
    switch (cycle) {
    case RuntimeGcCycle::Young:
      ++stats_.gc_young_cycles;
      break;
    case RuntimeGcCycle::Full:
      ++stats_.gc_full_cycles;
      break;
    case RuntimeGcCycle::Shared:
      ++stats_.gc_shared_cycles;
      break;
    }

    const std::uint64_t mark_epoch = ++gc_epoch_;
    result.cycle_id = mark_epoch;
    std::vector<std::uint64_t> stack;
    std::unordered_set<std::uint64_t> visited;

    auto enqueue_id = [&](std::uint64_t allocation_id) {
      if (allocation_id == 0) {
        return;
      }
      const auto found = objects_.find(allocation_id);
      if (found == objects_.end() || !found->second.logical_live ||
          found->second.ptr == nullptr) {
        return;
      }
      if (visited.insert(allocation_id).second) {
        stack.push_back(allocation_id);
      }
    };

    auto enqueue_value = [&](const Value &value) {
      const ObjHeader *header = heap_header_from_value(value);
      if (header == nullptr) {
        return;
      }
      enqueue_id(header->allocation_id);
    };

    for (const Value &root : roots) {
      enqueue_value(root);
    }

    for (const auto &[pin_id, pin] : pins_) {
      (void)pin_id;
      if (pin.active) {
        ++result.roots;
        enqueue_value(pin.value);
      }
    }

    if (cycle == RuntimeGcCycle::Young) {
      for (const auto &[owner_id, children] : remembered_set_) {
        (void)owner_id;
        for (std::uint64_t child_id : children) {
          enqueue_id(child_id);
        }
      }
    }

    while (!stack.empty()) {
      const std::uint64_t allocation_id = stack.back();
      stack.pop_back();
      auto found = objects_.find(allocation_id);
      if (found == objects_.end() || !found->second.logical_live ||
          found->second.ptr == nullptr) {
        continue;
      }
      ObjectRecord &record = found->second;
      ObjHeader *header = header_for_record(record);
      if (header == nullptr) {
        continue;
      }
      header->gc_mark_epoch = mark_epoch;
      ++result.marked;
      if (header_is_deallocated(*header)) {
        continue;
      }
      std::vector<Value> children;
      append_child_values(record, &children);
      for (const Value &child : children) {
        enqueue_value(child);
      }
    }

    std::vector<std::uint64_t> reclaim_ids;
    for (const auto &[allocation_id, record] : objects_) {
      if (!record.logical_live) {
        continue;
      }
      const ObjHeader *header = header_for_record(record);
      if (header == nullptr) {
        continue;
      }
      if (!cycle_collects_header(cycle, *header)) {
        continue;
      }
      if (active_pin_counts_.find(allocation_id) != active_pin_counts_.end()) {
        continue;
      }
      if (visited.find(allocation_id) == visited.end()) {
        reclaim_ids.push_back(allocation_id);
      }
    }

    for (std::uint64_t allocation_id : reclaim_ids) {
      auto found = objects_.find(allocation_id);
      if (found == objects_.end() || !found->second.logical_live) {
        continue;
      }
      reclaim_record_locked(found->second, &deferred_payload_release);
      ++result.reclaimed;
    }

    for (auto &[allocation_id, record] : objects_) {
      (void)allocation_id;
      if (!record.logical_live ||
          visited.find(record.allocation_id) == visited.end()) {
        continue;
      }
      ObjHeader *header = header_for_record(record);
      if (header == nullptr || header_is_deallocated(*header)) {
        continue;
      }
      if (header->generation == ObjectGeneration::Young &&
          cycle != RuntimeGcCycle::Shared) {
        ++header->gc_age;
        header->generation = ObjectGeneration::Mature;
        ++result.promoted;
      }
    }

    cleanup_remembered_set_locked();
    result.remembered_entries = remembered_entry_count_locked();
    stats_.gc_marked_objects += result.marked;
    stats_.gc_reclaimed_objects += result.reclaimed;
    stats_.gc_promoted_objects += result.promoted;
    return result;
  }

  void request_garbage_collection(RuntimeGcCycle cycle) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_gc_cycle_ = cycle;
    ++stats_.gc_requested;
  }

  std::optional<RuntimeGcCycle> pending_gc_request() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_gc_cycle_;
  }

  RuntimePinResult pin(const Value &value, RuntimePinViewKind view_kind,
                       RuntimePinPermission permissions) {
    RuntimePinResult out;
    ObjHeader *header = mutable_heap_header_from_value(value);
    if (!value_has_heap_payload_tag(value) || header == nullptr) {
      out.ok = false;
      out.error_name = "TypeError";
      out.message = "pin expects a heap object";
      return out;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::optional<std::string> lifecycle_error =
        lifecycle_access_error_name(*header);
    if (lifecycle_error.has_value()) {
      out.ok = false;
      out.error_name = *lifecycle_error;
      out.message = lifecycle_access_error_message(*lifecycle_error);
      return out;
    }
    const auto object = objects_.find(header->allocation_id);
    if (header->allocation_id == 0 || object == objects_.end() ||
        !object->second.logical_live) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "pin expects a live heap-owned object";
      return out;
    }
    if (view_kind == RuntimePinViewKind::ValueBuffer && !value.is_list() &&
        !value.is_tuple()) {
      out.ok = false;
      out.error_name = "TypeError";
      out.message = "buffer pin expects list or tuple storage";
      return out;
    }

    const std::uint64_t pin_id = next_pin_id_++;
    const std::uint64_t pin_epoch = next_pin_epoch_++;
    RuntimePinToken token;
    token.pin_id = pin_id;
    token.pin_epoch = pin_epoch;
    token.allocation_id = header->allocation_id;
    token.view_kind = view_kind;
    token.permissions = permissions;
    token.owner = header->owner;
    token.generation = header->generation;
    token.active = true;

    pins_[pin_id] =
        PinRecord{pin_id,      pin_epoch,     header->allocation_id, view_kind,
                  permissions, header->owner, header->generation,    value,
                  true};
    active_pins_by_object_[header->allocation_id].insert(pin_id);
    active_pin_counts_[header->allocation_id] += 1;
    header->pin_count += 1;
    header->pin_epoch = pin_epoch;
    header->flags |= kObjectFlagPinned;
    ++stats_.active_pins;
    ++stats_.pin_tokens_created;
    out.token = token;
    return out;
  }

  RuntimeUnpinResult unpin(RuntimePinToken *token) {
    RuntimeUnpinResult out;
    if (token == nullptr || token->pin_id == 0 || !token->active) {
      out.stale = true;
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.pin_stale_unpins;
      return out;
    }

    Value deferred_release = Value::null();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto found = pins_.find(token->pin_id);
      if (found == pins_.end() || !found->second.active ||
          found->second.pin_epoch != token->pin_epoch ||
          found->second.allocation_id != token->allocation_id) {
        out.stale = true;
        ++stats_.pin_stale_unpins;
        token->active = false;
        return out;
      }

      PinRecord &pin = found->second;
      ObjHeader *header = mutable_heap_header_from_value(pin.value);
      if (header != nullptr && header->allocation_id == pin.allocation_id) {
        if (header->pin_count > 0) {
          header->pin_count -= 1;
        }
        if (header->pin_count == 0) {
          header->flags &= ~kObjectFlagPinned;
        }
      }

      auto active_count = active_pin_counts_.find(pin.allocation_id);
      if (active_count != active_pin_counts_.end()) {
        if (active_count->second <= 1) {
          active_pin_counts_.erase(active_count);
        } else {
          active_count->second -= 1;
        }
      }
      auto active_set = active_pins_by_object_.find(pin.allocation_id);
      if (active_set != active_pins_by_object_.end()) {
        active_set->second.erase(pin.pin_id);
        if (active_set->second.empty()) {
          active_pins_by_object_.erase(active_set);
        }
      }
      pin.active = false;
      deferred_release = std::move(pin.value);
      pin.value = Value::null();
      if (stats_.active_pins > 0) {
        --stats_.active_pins;
      }
      ++stats_.pin_unpins;
      token->active = false;
      out.unpinned = true;
    }
    (void)deferred_release;
    return out;
  }

  std::uint64_t pin_count(const Value &value) const {
    const ObjHeader *header = heap_header_from_value(value);
    if (header == nullptr || header->allocation_id == 0) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = active_pin_counts_.find(header->allocation_id);
    return found == active_pin_counts_.end() ? 0 : found->second;
  }

  bool is_pinned(const Value &value) const { return pin_count(value) > 0; }

  RuntimeOpaqueHandleResult opaque_handle_for(const RuntimePinToken &token) {
    RuntimeOpaqueHandleResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    PinRecord *pin =
        active_pin_for_token_locked(token, &out.error_name, &out.message);
    if (pin == nullptr) {
      out.ok = false;
      return out;
    }
    if (pin->view_kind != RuntimePinViewKind::Opaque) {
      out.ok = false;
      out.error_name = "TypeError";
      out.message = "opaque handle requires an opaque pin";
      return out;
    }
    const ObjHeader *header = heap_header_from_value(pin->value);
    if (header == nullptr) {
      out.ok = false;
      out.error_name = "UseAfterFreeError";
      out.message = "pinned object is not available";
      return out;
    }
    const std::uint64_t handle_id = next_opaque_handle_id_++;
    RuntimeOpaqueHandle handle;
    handle.handle_id = handle_id;
    handle.pin_id = token.pin_id;
    handle.pin_epoch = token.pin_epoch;
    handle.allocation_id = token.allocation_id;
    handle.kind = header->kind;
    handle.active = true;
    opaque_handles_[handle_id] =
        OpaqueHandleRecord{handle_id,           token.pin_id, token.pin_epoch,
                           token.allocation_id, header->kind, true};
    ++stats_.opaque_handles_created;
    ++stats_.active_opaque_handles;
    out.handle = handle;
    return out;
  }

  RuntimeOpaqueHandleResult release_opaque_handle(RuntimeOpaqueHandle *handle) {
    RuntimeOpaqueHandleResult out;
    if (handle == nullptr || handle->handle_id == 0 || !handle->active) {
      out.ok = true;
      out.released = false;
      out.error_name = "LifetimeError";
      out.message = "opaque handle is stale";
      return out;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = opaque_handles_.find(handle->handle_id);
    if (found == opaque_handles_.end() || !found->second.active ||
        found->second.pin_epoch != handle->pin_epoch) {
      handle->active = false;
      out.ok = true;
      out.released = false;
      out.error_name = "LifetimeError";
      out.message = "opaque handle is stale";
      return out;
    }
    found->second.active = false;
    handle->active = false;
    if (stats_.active_opaque_handles > 0) {
      --stats_.active_opaque_handles;
    }
    out.released = true;
    return out;
  }

  RuntimeOpaqueHandleResult
  resolve_opaque_handle(const RuntimeOpaqueHandle &handle) const {
    RuntimeOpaqueHandleResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto handle_record = opaque_handles_.find(handle.handle_id);
    if (handle.handle_id == 0 || !handle.active ||
        handle_record == opaque_handles_.end() ||
        !handle_record->second.active ||
        handle_record->second.pin_epoch != handle.pin_epoch) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "opaque handle is stale";
      return out;
    }
    const auto pin = pins_.find(handle.pin_id);
    if (pin == pins_.end() || !pin->second.active ||
        pin->second.pin_epoch != handle.pin_epoch ||
        pin->second.allocation_id != handle.allocation_id) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "opaque handle pin is not active";
      return out;
    }
    out.value = pin->second.value;
    out.handle = handle;
    return out;
  }

  RuntimeValueBufferViewResult value_buffer_view(const RuntimePinToken &token) {
    RuntimeValueBufferViewResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    PinRecord *pin =
        active_pin_for_token_locked(token, &out.error_name, &out.message);
    if (pin == nullptr) {
      out.ok = false;
      return out;
    }
    if (pin->view_kind != RuntimePinViewKind::ValueBuffer) {
      out.ok = false;
      out.error_name = "TypeError";
      out.message = "value buffer view requires a buffer pin";
      return out;
    }

    RuntimeValueBufferView view;
    view.pin_id = token.pin_id;
    view.read_only = token.permissions == RuntimePinPermission::ReadOnly;
    view.active = true;
    if (pin->value.is_list()) {
      const std::shared_ptr<ListValue> list = pin->value.as_list();
      if (list == nullptr ||
          lifecycle_access_error_name(list->header).has_value()) {
        out.ok = false;
        out.error_name = "UseAfterFreeError";
        out.message = "pinned list buffer is not live";
        return out;
      }
      view.data = list->items.empty() ? nullptr : list->items.data();
      view.size = list->items.size();
    } else if (pin->value.is_tuple()) {
      const std::shared_ptr<TupleValue> tuple = pin->value.as_tuple();
      if (tuple == nullptr ||
          lifecycle_access_error_name(tuple->header).has_value()) {
        out.ok = false;
        out.error_name = "UseAfterFreeError";
        out.message = "pinned tuple buffer is not live";
        return out;
      }
      view.data = tuple->items.empty() ? nullptr : tuple->items.data();
      view.size = tuple->items.size();
    } else {
      out.ok = false;
      out.error_name = "TypeError";
      out.message = "buffer pin target has no contiguous value storage";
      return out;
    }
    ++stats_.buffer_views_created;
    out.view = view;
    return out;
  }

  RuntimeNativeWaitResult register_native_wait(const RuntimePinToken &token) {
    RuntimeNativeWaitResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    PinRecord *pin =
        active_pin_for_token_locked(token, &out.error_name, &out.message);
    if (pin == nullptr) {
      out.ok = false;
      return out;
    }
    (void)pin;
    const std::uint64_t wait_id = next_native_wait_id_++;
    RuntimeNativeWaitHandle handle;
    handle.wait_id = wait_id;
    handle.pin_id = token.pin_id;
    handle.pin_epoch = token.pin_epoch;
    handle.active = true;
    native_waits_[wait_id] =
        NativeWaitRecord{wait_id, token.pin_id, token.pin_epoch, true, false};
    ++stats_.native_waits_created;
    out.handle = handle;
    return out;
  }

  RuntimeNativeWaitResult cancel_native_wait(RuntimeNativeWaitHandle *handle) {
    RuntimeNativeWaitResult out;
    if (handle == nullptr || handle->wait_id == 0 || !handle->active) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait handle is stale";
      return out;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = native_waits_.find(handle->wait_id);
    if (found == native_waits_.end() || !found->second.active ||
        found->second.pin_epoch != handle->pin_epoch) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait handle is stale";
      return out;
    }
    found->second.cancellation_requested = true;
    handle->cancellation_requested = true;
    ++stats_.native_wait_cancellations;
    out.cancelled = true;
    out.handle = *handle;
    return out;
  }

  RuntimeNativeWaitResult
  poll_native_wait(const RuntimeNativeWaitHandle &handle) const {
    RuntimeNativeWaitResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto wait = native_waits_.find(handle.wait_id);
    if (handle.wait_id == 0 || !handle.active || wait == native_waits_.end() ||
        !wait->second.active || wait->second.pin_epoch != handle.pin_epoch) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait handle is stale";
      return out;
    }
    const auto pin = pins_.find(handle.pin_id);
    if (pin == pins_.end() || !pin->second.active ||
        pin->second.pin_epoch != handle.pin_epoch) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait pin is not active";
      return out;
    }
    out.cancelled = wait->second.cancellation_requested;
    out.handle = handle;
    out.handle.cancellation_requested = out.cancelled;
    return out;
  }

  RuntimeNativeWaitResult finish_native_wait(RuntimeNativeWaitHandle *handle) {
    RuntimeNativeWaitResult out;
    if (handle == nullptr || handle->wait_id == 0 || !handle->active) {
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait handle is stale";
      return out;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = native_waits_.find(handle->wait_id);
    if (found == native_waits_.end() || !found->second.active ||
        found->second.pin_epoch != handle->pin_epoch) {
      handle->active = false;
      out.ok = false;
      out.error_name = "LifetimeError";
      out.message = "native wait handle is stale";
      return out;
    }
    found->second.active = false;
    handle->active = false;
    out.finished = true;
    out.cancelled = found->second.cancellation_requested;
    out.handle = *handle;
    return out;
  }

  RuntimeHeapStats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeHeapStats out = stats_;
    out.arenas.clear();
    out.worker_count = static_cast<std::uint64_t>(arenas_.size());
    out.young_objects = 0;
    out.mature_objects = 0;
    out.shared_objects = 0;
    for (const auto &[allocation_id, record] : objects_) {
      (void)allocation_id;
      if (!record.logical_live) {
        continue;
      }
      const ObjHeader *header = header_for_record(record);
      if (header == nullptr) {
        continue;
      }
      switch (header->generation) {
      case ObjectGeneration::Young:
        ++out.young_objects;
        break;
      case ObjectGeneration::Mature:
        ++out.mature_objects;
        break;
      case ObjectGeneration::Shared:
        ++out.shared_objects;
        break;
      }
    }
    out.remembered_set_objects =
        static_cast<std::uint64_t>(remembered_set_.size());
    out.remembered_set_entries = remembered_entry_count_locked();
    out.pinned_objects = static_cast<std::uint64_t>(active_pin_counts_.size());
    for (const auto &[worker_id, arena] : arenas_) {
      out.arenas.push_back(RuntimeArenaStats{
          worker_id, arena.allocations, arena.live_objects,
          static_cast<std::uint64_t>(arena.remote_frees.size())});
    }
    return out;
  }

private:
  PinRecord *active_pin_for_token_locked(const RuntimePinToken &token,
                                         std::string *error_name,
                                         std::string *message) {
    if (token.pin_id == 0 || !token.active) {
      *error_name = "LifetimeError";
      *message = "pin token is stale";
      return nullptr;
    }
    auto found = pins_.find(token.pin_id);
    if (found == pins_.end() || !found->second.active ||
        found->second.pin_epoch != token.pin_epoch ||
        found->second.allocation_id != token.allocation_id) {
      *error_name = "LifetimeError";
      *message = "pin token is stale";
      return nullptr;
    }
    return &found->second;
  }

  template <typename T> static void destroy(void *ptr) {
    delete static_cast<T *>(ptr);
  }

  static ObjHeader *header_for_record(const ObjectRecord &record) {
    switch (record.kind) {
    case HeapObjectKind::Instance:
      return &static_cast<InstanceValue *>(record.ptr)->header;
    case HeapObjectKind::List:
      return &static_cast<ListValue *>(record.ptr)->header;
    case HeapObjectKind::Tuple:
      return &static_cast<TupleValue *>(record.ptr)->header;
    case HeapObjectKind::Map:
      return &static_cast<MapValue *>(record.ptr)->header;
    case HeapObjectKind::Closure:
      return &static_cast<ClosureValue *>(record.ptr)->header;
    }
    return nullptr;
  }

  ArenaState &arena_for_worker(std::uint64_t worker_id) {
    return arenas_[worker_id];
  }

  std::uint64_t reserve_allocation_id() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_allocation_id_++;
  }

  void record_allocation(std::uint64_t worker_id, HeapObjectKind kind,
                         std::size_t allocation_size,
                         std::uint64_t allocation_id, void *ptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    ArenaState &arena = arena_for_worker(worker_id);
    ++arena.allocations;
    ++arena.live_objects;
    ++stats_.allocations;
    ++stats_.live_objects;
    increment_kind_allocation(stats_, kind);
    objects_[allocation_id] = ObjectRecord{
        ptr, kind, worker_id, allocation_id, allocation_size, true};
  }

  void release(RemoteFree entry) {
    if (entry.ptr == nullptr || entry.destroy == nullptr) {
      return;
    }

    if (current_runtime_worker_id() == entry.owner_worker_id) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ArenaState &arena = arena_for_worker(entry.owner_worker_id);
        ++stats_.local_frees;
        const auto record = objects_.find(entry.allocation_id);
        if (record == objects_.end() || record->second.logical_live) {
          decrement_live_locked(arena, 1);
        }
        objects_.erase(entry.allocation_id);
        remove_remembered_edges_for_locked(entry.allocation_id);
      }
      entry.destroy(entry.ptr);
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ArenaState &arena = arena_for_worker(entry.owner_worker_id);
    arena.remote_frees.push_back(entry);
    ++stats_.remote_frees_queued;
    ++stats_.remote_queue_depth;
  }

  bool cycle_collects_header(RuntimeGcCycle cycle,
                             const ObjHeader &header) const {
    switch (cycle) {
    case RuntimeGcCycle::Young:
      return header.generation == ObjectGeneration::Young;
    case RuntimeGcCycle::Full:
      return true;
    case RuntimeGcCycle::Shared:
      return header.generation == ObjectGeneration::Shared;
    }
    return true;
  }

  void append_child_values(const ObjectRecord &record,
                           std::vector<Value> *out) const {
    const ObjHeader *header = header_for_record(record);
    if (header == nullptr || header_is_deallocated(*header)) {
      return;
    }
    switch (record.kind) {
    case HeapObjectKind::Instance: {
      const auto *instance = static_cast<const InstanceValue *>(record.ptr);
      out->insert(out->end(), instance->ivar_storage.begin(),
                  instance->ivar_storage.end());
      for (const auto &[name, value] : instance->ivars) {
        (void)name;
        out->push_back(value);
      }
      return;
    }
    case HeapObjectKind::List: {
      const auto *list = static_cast<const ListValue *>(record.ptr);
      out->insert(out->end(), list->items.begin(), list->items.end());
      return;
    }
    case HeapObjectKind::Tuple: {
      const auto *tuple = static_cast<const TupleValue *>(record.ptr);
      out->insert(out->end(), tuple->items.begin(), tuple->items.end());
      return;
    }
    case HeapObjectKind::Map: {
      const auto *map = static_cast<const MapValue *>(record.ptr);
      for (const MapEntry &entry : map->entries) {
        out->push_back(entry.value);
      }
      return;
    }
    case HeapObjectKind::Closure: {
      const auto *closure = static_cast<const ClosureValue *>(record.ptr);
      out->insert(out->end(), closure->captures.begin(),
                  closure->captures.end());
      out->push_back(closure->self);
      return;
    }
    }
  }

  std::shared_ptr<ShapeDescriptor> dead_shape_locked() {
    if (dead_shape_ == nullptr) {
      dead_shape_ = std::make_shared<ShapeDescriptor>();
      dead_shape_->dead = true;
    }
    return dead_shape_;
  }

  void move_values_for_deferred_release(std::vector<Value> *from,
                                        std::vector<Value> *deferred) {
    deferred->insert(deferred->end(), std::make_move_iterator(from->begin()),
                     std::make_move_iterator(from->end()));
    from->clear();
    from->shrink_to_fit();
  }

  void clear_payload_for_gc_locked(ObjectRecord &record,
                                   std::vector<Value> *deferred) {
    const std::shared_ptr<ShapeDescriptor> dead_shape = dead_shape_locked();
    switch (record.kind) {
    case HeapObjectKind::Instance: {
      auto *instance = static_cast<InstanceValue *>(record.ptr);
      move_values_for_deferred_release(&instance->ivar_storage, deferred);
      for (auto &[name, value] : instance->ivars) {
        (void)name;
        deferred->push_back(std::move(value));
      }
      instance->ivars.clear();
      instance->ivar_shape_version = 0;
      instance->header.shape = dead_shape;
      return;
    }
    case HeapObjectKind::List: {
      auto *list = static_cast<ListValue *>(record.ptr);
      move_values_for_deferred_release(&list->items, deferred);
      list->frozen = false;
      list->header.shape = dead_shape;
      return;
    }
    case HeapObjectKind::Tuple: {
      auto *tuple = static_cast<TupleValue *>(record.ptr);
      move_values_for_deferred_release(&tuple->items, deferred);
      tuple->header.shape = dead_shape;
      return;
    }
    case HeapObjectKind::Map: {
      auto *map = static_cast<MapValue *>(record.ptr);
      for (MapEntry &entry : map->entries) {
        deferred->push_back(std::move(entry.value));
      }
      map->entries.clear();
      map->entries.shrink_to_fit();
      map->frozen = false;
      map->header.shape = dead_shape;
      return;
    }
    case HeapObjectKind::Closure: {
      auto *closure = static_cast<ClosureValue *>(record.ptr);
      move_values_for_deferred_release(&closure->captures, deferred);
      deferred->push_back(std::move(closure->self));
      closure->self = Value::null();
      closure->code_id = 0;
      closure->header.shape = dead_shape;
      return;
    }
    }
  }

  void reclaim_record_locked(ObjectRecord &record,
                             std::vector<Value> *deferred_payload_release) {
    ObjHeader *header = header_for_record(record);
    if (header != nullptr) {
      clear_payload_for_gc_locked(record, deferred_payload_release);
      header->flags &= ~kObjectFlagDestroying;
      header->flags |= kObjectFlagDestroyed | kObjectFlagDead;
      header->lifetime_state = ObjectLifetimeState::Deallocated;
    }
    if (record.logical_live) {
      ArenaState &arena = arena_for_worker(record.owner_worker_id);
      decrement_live_locked(arena, 1);
    }
    record.logical_live = false;
    remove_remembered_edges_for_locked(record.allocation_id);
  }

  void remove_remembered_edges_for_locked(std::uint64_t allocation_id) {
    remembered_set_.erase(allocation_id);
    for (auto it = remembered_set_.begin(); it != remembered_set_.end();) {
      it->second.erase(allocation_id);
      if (it->second.empty()) {
        it = remembered_set_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void cleanup_remembered_set_locked() {
    for (auto it = remembered_set_.begin(); it != remembered_set_.end();) {
      const auto owner = objects_.find(it->first);
      if (owner == objects_.end() || !owner->second.logical_live) {
        it = remembered_set_.erase(it);
        continue;
      }
      const ObjHeader *owner_header = header_for_record(owner->second);
      if (owner_header == nullptr ||
          owner_header->generation != ObjectGeneration::Mature) {
        it = remembered_set_.erase(it);
        continue;
      }
      for (auto child = it->second.begin(); child != it->second.end();) {
        const auto child_record = objects_.find(*child);
        const ObjHeader *child_header =
            child_record == objects_.end()
                ? nullptr
                : header_for_record(child_record->second);
        if (child_record == objects_.end() ||
            !child_record->second.logical_live || child_header == nullptr ||
            child_header->generation != ObjectGeneration::Young) {
          child = it->second.erase(child);
        } else {
          ++child;
        }
      }
      if (it->second.empty()) {
        it = remembered_set_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::uint64_t remembered_entry_count_locked() const {
    std::uint64_t count = 0;
    for (const auto &[owner, children] : remembered_set_) {
      (void)owner;
      count += static_cast<std::uint64_t>(children.size());
    }
    return count;
  }

  void decrement_live_locked(ArenaState &arena, std::uint64_t count) {
    if (arena.live_objects >= count) {
      arena.live_objects -= count;
    } else {
      arena.live_objects = 0;
    }
    if (stats_.live_objects >= count) {
      stats_.live_objects -= count;
    } else {
      stats_.live_objects = 0;
    }
  }

  void drain_all_remote_frees() {
    while (true) {
      std::vector<std::pair<std::uint64_t, std::deque<RemoteFree>>> batches;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &[worker_id, arena] : arenas_) {
          if (arena.remote_frees.empty()) {
            continue;
          }
          std::deque<RemoteFree> pending;
          pending.swap(arena.remote_frees);
          const std::uint64_t count =
              static_cast<std::uint64_t>(pending.size());
          stats_.remote_queue_depth -= count;
          stats_.remote_frees_drained += count;
          for (const RemoteFree &entry : pending) {
            const auto record = objects_.find(entry.allocation_id);
            if (record == objects_.end() || record->second.logical_live) {
              decrement_live_locked(arena, 1);
            }
            objects_.erase(entry.allocation_id);
            remove_remembered_edges_for_locked(entry.allocation_id);
          }
          batches.push_back({worker_id, std::move(pending)});
        }
      }
      if (batches.empty()) {
        return;
      }
      for (const auto &batch : batches) {
        RuntimeWorkerScope owner_scope(batch.first);
        for (const RemoteFree &entry : batch.second) {
          entry.destroy(entry.ptr);
        }
      }
    }
  }

  mutable std::mutex mutex_;
  std::uint64_t next_allocation_id_ = 1;
  std::uint64_t gc_epoch_ = 0;
  std::uint64_t next_pin_id_ = 1;
  std::uint64_t next_pin_epoch_ = 1;
  std::uint64_t next_opaque_handle_id_ = 1;
  std::uint64_t next_native_wait_id_ = 1;
  RuntimeHeapStats stats_;
  std::map<std::uint64_t, ArenaState> arenas_;
  std::unordered_map<std::uint64_t, ObjectRecord> objects_;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>>
      remembered_set_;
  std::unordered_map<std::uint64_t, PinRecord> pins_;
  std::unordered_map<std::uint64_t, std::uint64_t> active_pin_counts_;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>>
      active_pins_by_object_;
  std::unordered_map<std::uint64_t, OpaqueHandleRecord> opaque_handles_;
  std::unordered_map<std::uint64_t, NativeWaitRecord> native_waits_;
  std::shared_ptr<ShapeDescriptor> dead_shape_;
  std::optional<RuntimeGcCycle> pending_gc_cycle_;
};

RuntimeHeap::RuntimeHeap() : impl_(std::make_shared<Impl>()) {}

RuntimeHeap::~RuntimeHeap() = default;

std::shared_ptr<InstanceValue>
RuntimeHeap::make_instance_value(std::uint32_t class_index) {
  return impl_->allocate<InstanceValue>(
      HeapObjectKind::Instance, [class_index](InstanceValue &value) {
        value.class_index = class_index;
        value.header.class_index = class_index;
      });
}

std::shared_ptr<ClosureValue> RuntimeHeap::make_closure_value() {
  return impl_->allocate<ClosureValue>(HeapObjectKind::Closure);
}

Value RuntimeHeap::make_list_value(std::vector<Value> items, bool frozen) {
  auto value = impl_->allocate<ListValue>(
      HeapObjectKind::List, [frozen, &items](ListValue &value) {
        value.header.flags =
            frozen ? kObjectFlagFrozen | kObjectFlagShareable : 0U;
        value.header.owner.kind =
            frozen ? OwnerTokenKind::Shareable : OwnerTokenKind::Confined;
        value.header.generation =
            frozen ? ObjectGeneration::Shared : ObjectGeneration::Young;
        value.items = std::move(items);
        value.frozen = frozen;
      });
  return {std::move(value)};
}

Value RuntimeHeap::make_tuple_value(std::vector<Value> items) {
  auto value = impl_->allocate<TupleValue>(
      HeapObjectKind::Tuple, [&items](TupleValue &value) {
        value.header.flags = kObjectFlagFrozen | kObjectFlagShareable;
        value.header.owner.kind = OwnerTokenKind::Shareable;
        value.header.generation = ObjectGeneration::Shared;
        value.items = std::move(items);
      });
  return {std::move(value)};
}

Value RuntimeHeap::make_symbol_map_value(std::vector<MapEntry> entries,
                                         bool frozen) {
  auto value = impl_->allocate<MapValue>(
      HeapObjectKind::Map, [frozen, &entries](MapValue &value) {
        value.header.flags =
            frozen ? kObjectFlagFrozen | kObjectFlagShareable : 0U;
        value.header.owner.kind =
            frozen ? OwnerTokenKind::Shareable : OwnerTokenKind::Confined;
        value.header.generation =
            frozen ? ObjectGeneration::Shared : ObjectGeneration::Young;
        value.entries = std::move(entries);
        value.frozen = frozen;
      });
  return {std::move(value)};
}

std::uint64_t RuntimeHeap::drain_remote_frees() {
  return drain_remote_frees(current_runtime_worker_id());
}

std::uint64_t RuntimeHeap::drain_remote_frees(std::uint64_t worker_id) {
  return impl_->drain_remote_frees(worker_id);
}

RuntimeWriteBarrierResult RuntimeHeap::write_barrier(const Value &owner,
                                                     const Value &value) {
  return impl_->write_barrier(owner, value);
}

RuntimeGcResult RuntimeHeap::collect_garbage(const std::vector<Value> &roots,
                                             RuntimeGcCycle cycle,
                                             bool from_safepoint) {
  return impl_->collect_garbage(roots, cycle, from_safepoint);
}

void RuntimeHeap::request_garbage_collection(RuntimeGcCycle cycle) {
  impl_->request_garbage_collection(cycle);
}

std::optional<RuntimeGcCycle> RuntimeHeap::pending_gc_request() const {
  return impl_->pending_gc_request();
}

RuntimePinResult RuntimeHeap::pin(const Value &value,
                                  RuntimePinViewKind view_kind,
                                  RuntimePinPermission permissions) {
  return impl_->pin(value, view_kind, permissions);
}

RuntimeUnpinResult RuntimeHeap::unpin(RuntimePinToken *token) {
  return impl_->unpin(token);
}

std::uint64_t RuntimeHeap::pin_count(const Value &value) const {
  return impl_->pin_count(value);
}

bool RuntimeHeap::is_pinned(const Value &value) const {
  return impl_->is_pinned(value);
}

RuntimeOpaqueHandleResult
RuntimeHeap::opaque_handle_for(const RuntimePinToken &token) {
  return impl_->opaque_handle_for(token);
}

RuntimeOpaqueHandleResult
RuntimeHeap::release_opaque_handle(RuntimeOpaqueHandle *handle) {
  return impl_->release_opaque_handle(handle);
}

RuntimeOpaqueHandleResult
RuntimeHeap::resolve_opaque_handle(const RuntimeOpaqueHandle &handle) const {
  return impl_->resolve_opaque_handle(handle);
}

RuntimeValueBufferViewResult
RuntimeHeap::value_buffer_view(const RuntimePinToken &token) {
  return impl_->value_buffer_view(token);
}

RuntimeNativeWaitResult
RuntimeHeap::register_native_wait(const RuntimePinToken &token) {
  return impl_->register_native_wait(token);
}

RuntimeNativeWaitResult
RuntimeHeap::cancel_native_wait(RuntimeNativeWaitHandle *handle) {
  return impl_->cancel_native_wait(handle);
}

RuntimeNativeWaitResult
RuntimeHeap::poll_native_wait(const RuntimeNativeWaitHandle &handle) const {
  return impl_->poll_native_wait(handle);
}

RuntimeNativeWaitResult
RuntimeHeap::finish_native_wait(RuntimeNativeWaitHandle *handle) {
  return impl_->finish_native_wait(handle);
}

RuntimeHeapStats RuntimeHeap::stats() const { return impl_->stats(); }

RuntimePinScope::RuntimePinScope(RuntimeHeap &heap, const Value &value,
                                 RuntimePinViewKind view_kind,
                                 RuntimePinPermission permissions)
    : heap_(&heap), result_(heap.pin(value, view_kind, permissions)) {}

RuntimePinScope::RuntimePinScope(RuntimePinScope &&other) noexcept
    : heap_(other.heap_), result_(other.result_) {
  other.heap_ = nullptr;
  other.result_.token.active = false;
}

RuntimePinScope &RuntimePinScope::operator=(RuntimePinScope &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  (void)unpin();
  heap_ = other.heap_;
  result_ = other.result_;
  other.heap_ = nullptr;
  other.result_.token.active = false;
  return *this;
}

RuntimePinScope::~RuntimePinScope() { (void)unpin(); }

bool RuntimePinScope::active() const {
  return heap_ != nullptr && result_.ok && result_.token.active;
}

const RuntimePinResult &RuntimePinScope::result() const { return result_; }

const RuntimePinToken &RuntimePinScope::token() const { return result_.token; }

RuntimeUnpinResult RuntimePinScope::unpin() {
  if (heap_ == nullptr || !result_.token.active) {
    RuntimeUnpinResult out;
    out.stale = true;
    return out;
  }
  RuntimeUnpinResult out = heap_->unpin(&result_.token);
  if (out.unpinned || out.stale) {
    heap_ = nullptr;
  }
  return out;
}

RuntimeHeap &default_runtime_heap() {
  static RuntimeHeap heap;
  return heap;
}

Value Value::null() { return {std::monostate{}}; }

Value Value::boolean(bool value) { return {value}; }

Value Value::integer(std::int64_t value) { return {value}; }

Value Value::floating(double value) { return {value}; }

Value Value::symbol(std::uint32_t symbol_id) {
  return {SymbolValue{symbol_id}};
}

Value Value::string(std::uint32_t string_id) {
  return {StringValue{string_id}};
}

Value Value::class_object(std::uint32_t class_index) {
  return {ClassObjectValue{class_index}};
}

Value Value::closure(std::shared_ptr<ClosureValue> value) {
  return {std::move(value)};
}

Value Value::instance(std::shared_ptr<InstanceValue> value) {
  return {std::move(value)};
}

bool Value::is_null() const {
  return std::holds_alternative<std::monostate>(payload);
}

bool Value::is_bool() const { return std::holds_alternative<bool>(payload); }

bool Value::is_integer() const {
  return std::holds_alternative<std::int64_t>(payload);
}

bool Value::is_float() const { return std::holds_alternative<double>(payload); }

bool Value::is_symbol() const {
  return std::holds_alternative<SymbolValue>(payload);
}

bool Value::is_string() const {
  return std::holds_alternative<StringValue>(payload);
}

bool Value::is_class_object() const {
  return std::holds_alternative<ClassObjectValue>(payload);
}

bool Value::is_closure() const {
  return std::holds_alternative<std::shared_ptr<ClosureValue>>(payload);
}

bool Value::is_instance_object() const {
  return std::holds_alternative<std::shared_ptr<InstanceValue>>(payload);
}

bool Value::is_list() const {
  return std::holds_alternative<std::shared_ptr<ListValue>>(payload);
}

bool Value::is_tuple() const {
  return std::holds_alternative<std::shared_ptr<TupleValue>>(payload);
}

bool Value::is_map() const {
  return std::holds_alternative<std::shared_ptr<MapValue>>(payload);
}

bool Value::as_bool() const { return std::get<bool>(payload); }

std::int64_t Value::as_integer() const {
  return std::get<std::int64_t>(payload);
}

double Value::as_float() const { return std::get<double>(payload); }

SymbolValue Value::as_symbol() const { return std::get<SymbolValue>(payload); }

StringValue Value::as_string() const { return std::get<StringValue>(payload); }

ClassObjectValue Value::as_class_object() const {
  return std::get<ClassObjectValue>(payload);
}

std::shared_ptr<ClosureValue> Value::as_closure() const {
  return std::get<std::shared_ptr<ClosureValue>>(payload);
}

std::shared_ptr<InstanceValue> Value::as_instance_object() const {
  return std::get<std::shared_ptr<InstanceValue>>(payload);
}

std::shared_ptr<ListValue> Value::as_list() const {
  return std::get<std::shared_ptr<ListValue>>(payload);
}

std::shared_ptr<TupleValue> Value::as_tuple() const {
  return std::get<std::shared_ptr<TupleValue>>(payload);
}

std::shared_ptr<MapValue> Value::as_map() const {
  return std::get<std::shared_ptr<MapValue>>(payload);
}

Value make_list_value(std::vector<Value> items, bool frozen) {
  return default_runtime_heap().make_list_value(std::move(items), frozen);
}

Value make_tuple_value(std::vector<Value> items) {
  return default_runtime_heap().make_tuple_value(std::move(items));
}

Value make_symbol_map_value(std::vector<MapEntry> entries, bool frozen) {
  return default_runtime_heap().make_symbol_map_value(std::move(entries),
                                                      frozen);
}

namespace {

using amber::bytecode::BcCode;
using amber::bytecode::BcModule;
using amber::bytecode::CodeKind;
using amber::bytecode::Constant;
using amber::bytecode::ConstantKind;
using amber::bytecode::Instruction;
using amber::bytecode::Opcode;
using amber::bytecode::SlotLayoutEntry;

constexpr std::uint32_t kMethodFlagInstance = 1U;
constexpr std::uint32_t kMethodFlagClass = 2U;
constexpr std::int64_t kPatternFailModeSoft = 0;
constexpr std::int64_t kPatternFailModeMatchError = 1;

bool value_has_heap_payload_tag(const Value &value) {
  return value.is_closure() || value.is_instance_object() || value.is_list() ||
         value.is_tuple() || value.is_map();
}

const ObjHeader *heap_header_from_value(const Value &value) {
  if (value.is_closure()) {
    const std::shared_ptr<ClosureValue> object = value.as_closure();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_instance_object()) {
    const std::shared_ptr<InstanceValue> object = value.as_instance_object();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_list()) {
    const std::shared_ptr<ListValue> object = value.as_list();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_tuple()) {
    const std::shared_ptr<TupleValue> object = value.as_tuple();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_map()) {
    const std::shared_ptr<MapValue> object = value.as_map();
    return object == nullptr ? nullptr : &object->header;
  }
  return nullptr;
}

ObjHeader *mutable_heap_header_from_value(const Value &value) {
  if (value.is_closure()) {
    const std::shared_ptr<ClosureValue> object = value.as_closure();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_instance_object()) {
    const std::shared_ptr<InstanceValue> object = value.as_instance_object();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_list()) {
    const std::shared_ptr<ListValue> object = value.as_list();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_tuple()) {
    const std::shared_ptr<TupleValue> object = value.as_tuple();
    return object == nullptr ? nullptr : &object->header;
  }
  if (value.is_map()) {
    const std::shared_ptr<MapValue> object = value.as_map();
    return object == nullptr ? nullptr : &object->header;
  }
  return nullptr;
}

bool header_is_deallocated(const ObjHeader &header) {
  return header.lifetime_state == ObjectLifetimeState::Deallocated ||
         (header.flags & kObjectFlagDead) != 0U ||
         (header.shape != nullptr && header.shape->dead);
}

bool header_is_destroyed(const ObjHeader &header) {
  return header.lifetime_state == ObjectLifetimeState::Destroyed ||
         header.lifetime_state == ObjectLifetimeState::Destroying ||
         (header.flags & kObjectFlagDestroyed) != 0U ||
         (header.flags & kObjectFlagDestroying) != 0U;
}

std::optional<std::string>
lifecycle_access_error_name(const ObjHeader &header) {
  if (header_is_deallocated(header)) {
    return "UseAfterFreeError";
  }
  if (header_is_destroyed(header)) {
    return "DestroyedAccessError";
  }
  return std::nullopt;
}

std::string lifecycle_access_error_message(const std::string &error_name) {
  return error_name == "UseAfterFreeError" ? "access to deallocated object"
                                           : "access to destroyed object";
}

std::string lifecycle_debug_label(const ObjHeader &header) {
  if (header_is_deallocated(header)) {
    return "deallocated";
  }
  if (header_is_destroyed(header)) {
    return "destroyed";
  }
  return "";
}

struct PreparedSeqState {
  std::vector<Value> items;
  std::size_t rest_start = 0;
  bool source_was_tuple = false;
};

struct PreparedMapState {
  std::vector<MapEntry> entries;
  std::unordered_map<std::uint32_t, std::size_t> index_by_key;
  std::vector<std::uint32_t> requested_keys;
  bool needs_full = false;
  bool rest_bound = false;
  std::uint32_t fail_pc = 0;
};

struct CoercedSeqState {
  Value value = Value::null();
  std::vector<Value> items;
  bool source_was_tuple = false;
};

struct CoercedMapState {
  Value value = Value::null();
  std::vector<MapEntry> entries;
};

struct CallCacheEntry {
  bool valid = false;
  std::uint32_t receiver_class_index = 0;
  std::uint32_t dispatch_flags = 0;
  std::uint32_t selector_symbol_id = 0;
  std::uint32_t positional_count = 0;
  std::vector<std::uint32_t> keyword_shape;
  bool has_block = false;
  std::uint64_t method_version = 0;
  std::uint64_t world_epoch = 0;
  bytecode::BcMethod method;
};

struct IvarCacheEntry {
  bool valid = false;
  std::uint32_t receiver_class_index = 0;
  std::uint32_t symbol_id = 0;
  std::uint64_t shape_id = 0;
  std::uint64_t shape_version = 0;
  std::uint32_t slot_index = 0;
};

struct Frame {
  const BcCode *code = nullptr;
  std::size_t pc = 0;
  std::vector<Value> regs;
  std::vector<std::uint8_t> initialized;
  std::vector<Value> captures;
  Value self = Value::null();
  Value block = Value::null();
  Value last_result = Value::null();
  std::optional<std::uint32_t> caller_result_reg;
  std::optional<std::uint32_t> active_call_pc;
  std::optional<Value> return_override;
  std::unordered_map<std::uint32_t, PreparedSeqState> prepared_seq_regs;
  std::unordered_map<std::uint32_t, PreparedMapState> prepared_map_regs;
  std::unordered_map<std::uint32_t, Value> pending_pattern_bindings;
};

struct MethodTableDescriptor {
  std::unordered_map<std::uint32_t, bytecode::BcMethod> entries;
};

struct ClassRuntimeState {
  std::unordered_map<std::string, Value> cvars;
  MethodTableDescriptor instance_method_table;
  MethodTableDescriptor class_method_table;
  std::vector<std::uint32_t> direct_include_indices;
  std::vector<std::uint32_t> direct_extend_indices;
  std::uint32_t owner_flags = 0;
  std::uint32_t ivar_schema_id = 0;
  bool has_superclass_ref = false;
  std::uint32_t superclass_ref = 0;
  bool method_range_valid = true;
  std::uint64_t method_version = 1;
};

struct RuntimeState {
  RuntimeHeap heap;
  std::vector<ClassRuntimeState> classes;
  bool owners_initialized = false;
  bool world_frozen = false;
  std::uint64_t world_epoch = 1;
  std::unordered_map<std::uint64_t, CallCacheEntry> call_caches;
  std::uint64_t call_cache_hits = 0;
  std::uint64_t call_cache_misses = 0;
  std::uint64_t call_cache_updates = 0;
  std::unordered_map<std::uint64_t, IvarCacheEntry> ivar_caches;
  std::uint64_t next_shape_id = 1;
  std::vector<std::shared_ptr<ShapeDescriptor>> root_shapes;
  std::unordered_map<std::string, std::shared_ptr<ShapeDescriptor>>
      shape_transitions;
  std::shared_ptr<ShapeDescriptor> dead_shape;

  static std::string shape_transition_key(std::uint64_t parent_id,
                                          const std::string &name) {
    return std::to_string(parent_id) + "\x1f" + name;
  }

  void initialize_for_module(const BcModule &module) {
    if (dead_shape == nullptr) {
      dead_shape = std::make_shared<ShapeDescriptor>();
      dead_shape->shape_id = 0;
      dead_shape->shape_version = 0;
      dead_shape->dead = true;
    }
    if (classes.size() < module.classes.size()) {
      classes.resize(module.classes.size());
    }
    if (root_shapes.size() < module.classes.size()) {
      root_shapes.resize(module.classes.size());
    }
    if (owners_initialized) {
      return;
    }
    for (std::uint32_t index = 0; index < module.classes.size(); ++index) {
      ClassRuntimeState &runtime = classes[index];
      const bytecode::BcClass &owner = module.classes[index];
      runtime.owner_flags = owner.flags;
      runtime.ivar_schema_id = owner.ivar_schema_id;
      runtime.has_superclass_ref = owner.has_superclass_ref;
      runtime.superclass_ref = owner.superclass_ref;
      runtime.method_range_valid =
          owner.method_range_start + owner.method_range_count <=
          module.methods.size();
      if (!runtime.method_range_valid) {
        continue;
      }
      for (std::uint32_t offset = 0; offset < owner.method_range_count;
           ++offset) {
        bytecode::BcMethod method =
            module.methods[owner.method_range_start + offset];
        MethodTableDescriptor &table = method.flags == kMethodFlagClass
                                           ? runtime.class_method_table
                                           : runtime.instance_method_table;
        table.entries[method.selector_sym_id] = std::move(method);
      }
    }
    owners_initialized = true;
  }

  std::shared_ptr<const ShapeDescriptor>
  root_shape_for_class(std::uint32_t class_index) {
    if (root_shapes.size() <= class_index) {
      root_shapes.resize(static_cast<std::size_t>(class_index) + 1U);
    }
    if (root_shapes[class_index] == nullptr) {
      auto shape = std::make_shared<ShapeDescriptor>();
      shape->shape_id = next_shape_id++;
      shape->shape_version = shape->shape_id;
      root_shapes[class_index] = shape;
    }
    return root_shapes[class_index];
  }

  std::shared_ptr<const ShapeDescriptor>
  transition_shape(std::shared_ptr<const ShapeDescriptor> parent,
                   const std::string &name) {
    if (parent == nullptr || parent->dead) {
      return parent;
    }
    if (parent->ivar_slots.find(name) != parent->ivar_slots.end()) {
      return parent;
    }
    const std::string key = shape_transition_key(parent->shape_id, name);
    const auto found = shape_transitions.find(key);
    if (found != shape_transitions.end()) {
      return found->second;
    }
    auto next = std::make_shared<ShapeDescriptor>();
    next->shape_id = next_shape_id++;
    next->shape_version = next->shape_id;
    next->ivar_slots = parent->ivar_slots;
    next->slot_names = parent->slot_names;
    next->parent_shape = std::move(parent);
    next->ivar_slots[name] =
        static_cast<std::uint32_t>(next->slot_names.size());
    next->slot_names.push_back(name);
    shape_transitions[key] = next;
    return next;
  }

  void invalidate_dispatch_owner(std::uint32_t class_index) {
    if (class_index >= classes.size()) {
      return;
    }
    ++classes[class_index].method_version;
    ++world_epoch;
  }

  void replace_module_runtime_state(const BcModule &module) {
    std::vector<ClassRuntimeState> previous_classes = std::move(classes);
    classes.clear();
    classes.resize(module.classes.size());
    if (root_shapes.size() < module.classes.size()) {
      root_shapes.resize(module.classes.size());
    }

    for (std::uint32_t index = 0; index < module.classes.size(); ++index) {
      ClassRuntimeState &runtime = classes[index];
      if (index < previous_classes.size()) {
        runtime.cvars = std::move(previous_classes[index].cvars);
        runtime.direct_include_indices =
            std::move(previous_classes[index].direct_include_indices);
        runtime.direct_extend_indices =
            std::move(previous_classes[index].direct_extend_indices);
        runtime.method_version = previous_classes[index].method_version + 1U;
      }

      const bytecode::BcClass &owner = module.classes[index];
      runtime.owner_flags = owner.flags;
      runtime.ivar_schema_id = owner.ivar_schema_id;
      runtime.has_superclass_ref = owner.has_superclass_ref;
      runtime.superclass_ref = owner.superclass_ref;
      runtime.method_range_valid =
          owner.method_range_start + owner.method_range_count <=
          module.methods.size();
      if (!runtime.method_range_valid) {
        continue;
      }
      for (std::uint32_t offset = 0; offset < owner.method_range_count;
           ++offset) {
        bytecode::BcMethod method =
            module.methods[owner.method_range_start + offset];
        MethodTableDescriptor &table = method.flags == kMethodFlagClass
                                           ? runtime.class_method_table
                                           : runtime.instance_method_table;
        table.entries[method.selector_sym_id] = std::move(method);
      }
    }

    owners_initialized = true;
    call_caches.clear();
    ivar_caches.clear();
    ++world_epoch;
  }
};

const BcCode *find_code(const BcModule &module, std::uint32_t code_id) {
  for (const BcCode &code : module.code_objects) {
    if (code.code_id == code_id) {
      return &code;
    }
  }
  return nullptr;
}

bool is_truthy(const Value &value) {
  return !value.is_null() && !(value.is_bool() && !value.as_bool());
}

bool value_equals(const Value &lhs, const Value &rhs) {
  if (lhs.payload.index() != rhs.payload.index()) {
    return false;
  }
  if (lhs.is_null()) {
    return true;
  }
  if (lhs.is_bool()) {
    return lhs.as_bool() == rhs.as_bool();
  }
  if (lhs.is_integer()) {
    return lhs.as_integer() == rhs.as_integer();
  }
  if (lhs.is_float()) {
    return lhs.as_float() == rhs.as_float();
  }
  if (lhs.is_symbol()) {
    return lhs.as_symbol().symbol_id == rhs.as_symbol().symbol_id;
  }
  if (lhs.is_string()) {
    return lhs.as_string().string_id == rhs.as_string().string_id;
  }
  if (lhs.is_class_object()) {
    return lhs.as_class_object().class_index ==
           rhs.as_class_object().class_index;
  }
  if (lhs.is_closure()) {
    return lhs.as_closure() == rhs.as_closure();
  }
  if (lhs.is_instance_object()) {
    return lhs.as_instance_object() == rhs.as_instance_object();
  }
  if (lhs.is_list()) {
    const std::shared_ptr<ListValue> left = lhs.as_list();
    const std::shared_ptr<ListValue> right = rhs.as_list();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (left->items.size() != right->items.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left->items.size(); ++i) {
      if (!value_equals(left->items[i], right->items[i])) {
        return false;
      }
    }
    return true;
  }
  if (lhs.is_tuple()) {
    const std::shared_ptr<TupleValue> left = lhs.as_tuple();
    const std::shared_ptr<TupleValue> right = rhs.as_tuple();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (left->items.size() != right->items.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left->items.size(); ++i) {
      if (!value_equals(left->items[i], right->items[i])) {
        return false;
      }
    }
    return true;
  }
  if (lhs.is_map()) {
    const std::shared_ptr<MapValue> left = lhs.as_map();
    const std::shared_ptr<MapValue> right = rhs.as_map();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (left->entries.size() != right->entries.size()) {
      return false;
    }
    std::unordered_map<std::uint32_t, std::size_t> right_index;
    for (std::size_t i = 0; i < right->entries.size(); ++i) {
      right_index[right->entries[i].symbol_id] = i;
    }
    for (const MapEntry &entry : left->entries) {
      const auto right_it = right_index.find(entry.symbol_id);
      if (right_it == right_index.end()) {
        return false;
      }
      if (!value_equals(entry.value, right->entries[right_it->second].value)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

enum class SendStatus { Matched, NotHandled, Faulted };

struct BoundMethodArg {
  bool present = false;
  Value value = Value::null();
};

struct CallPacket {
  std::uint32_t dst = 0;
  Value callee = Value::null();
  std::vector<Value> pos_args;
  std::vector<std::pair<std::uint32_t, Value>> kw_args;
  Value block = Value::null();
  std::optional<std::uint32_t> site_id;
};

struct NestedExecution {
  Value value = Value::null();
  std::vector<Value> regs;
  std::vector<std::uint8_t> initialized;
  std::optional<Fault> fault;

  bool ok() const { return !fault.has_value(); }
};

class Vm {
public:
  explicit Vm(const BcModule &module,
              std::shared_ptr<RuntimeState> state = nullptr,
              std::string module_id = {})
      : module_(module),
        state_(state == nullptr ? std::make_shared<RuntimeState>()
                                : std::move(state)),
        module_id_(std::move(module_id)) {
    state_->initialize_for_module(module_);
  }

  ExecutionResult execute(std::uint32_t code_id, const std::vector<Value> &args,
                          Value self, Value block) {
    const BcCode *entry = find_code(module_, code_id);
    if (entry == nullptr) {
      return fail("VMError", "unknown code id", code_id, 0);
    }
    push_frame(*entry, args, {}, std::move(self), std::move(block),
               std::nullopt);
    while (fault_ == std::nullopt && !frames_.empty()) {
      step();
    }
    state_->heap.drain_remote_frees();
    if (fault_.has_value()) {
      return {Value::null(), fault_};
    }
    return {final_value_, std::nullopt, completed_locals_for(*entry)};
  }

private:
  ExecutionResult fail(const std::string &error_name,
                       const std::string &message, std::uint32_t code_id,
                       std::uint32_t pc) {
    return {Value::null(), Fault{error_name, message, code_id, pc}};
  }

  std::string string_or_empty(std::uint32_t string_id) const {
    if (string_id >= module_.strings.size()) {
      return "";
    }
    return module_.strings[string_id];
  }

  std::vector<ExecutionLocal> completed_locals_for(const BcCode &code) const {
    std::vector<ExecutionLocal> locals;
    locals.reserve(code.local_layout.size());
    for (const SlotLayoutEntry &entry : code.local_layout) {
      ExecutionLocal local;
      local.slot = entry.slot;
      local.name = string_or_empty(entry.name_str_id);
      local.role = string_or_empty(entry.role_str_id);
      local.binding_kind = string_or_empty(entry.binding_kind_str_id);
      local.initialized =
          entry.slot < last_completed_initialized_.size() &&
          last_completed_initialized_[entry.slot] != 0U &&
          entry.slot < last_completed_regs_.size();
      if (local.initialized) {
        local.value = last_completed_regs_[entry.slot];
      }
      locals.push_back(std::move(local));
    }
    return locals;
  }

  bool ensure_lifecycle_access(const Frame &frame, const Value &value) {
    if (!value_has_heap_payload_tag(value)) {
      return true;
    }
    const ObjHeader *header = heap_header_from_value(value);
    if (header == nullptr) {
      set_fault(frame, "TypeError", "heap object reference is null");
      return false;
    }
    const std::optional<std::string> error_name =
        lifecycle_access_error_name(*header);
    if (error_name.has_value()) {
      set_fault(frame, *error_name,
                lifecycle_access_error_message(*error_name));
      return false;
    }
    return true;
  }

  enum class LifecycleTargetStatus { NonHeap, Heap };

  std::optional<LifecycleTargetStatus>
  lifecycle_target_header(const Frame &frame, const Value &value,
                          ObjHeader **out_header) {
    if (!value_has_heap_payload_tag(value)) {
      *out_header = nullptr;
      return LifecycleTargetStatus::NonHeap;
    }
    ObjHeader *header = mutable_heap_header_from_value(value);
    if (header == nullptr) {
      set_fault(frame, "TypeError", "heap object reference is null");
      return std::nullopt;
    }
    *out_header = header;
    return LifecycleTargetStatus::Heap;
  }

  bool check_lifecycle_preconditions(const Frame &frame,
                                     const ObjHeader &header) {
    if (header.owner.kind != OwnerTokenKind::Confined) {
      set_fault(frame, "LifetimeError",
                "explicit lifecycle operation requires confined object");
      return false;
    }
    if (header.owner.strand_id != current_runtime_owner_strand_id()) {
      set_fault(frame, "IsolationError",
                "lifecycle operation must run on the owner strand");
      return false;
    }
    return true;
  }

  bool check_not_pinned_for_lifecycle(const Frame &frame, const Value &value) {
    if (state_->heap.is_pinned(value)) {
      set_fault(frame, "PinnedObjectError",
                "cannot destroy or deallocate pinned object");
      return false;
    }
    return true;
  }

  bool run_destroy_methods(Frame &frame, const Value &value) {
    if (!value.is_instance_object()) {
      return true;
    }
    const std::shared_ptr<InstanceValue> instance = value.as_instance_object();
    if (instance == nullptr ||
        instance->class_index >= module_.classes.size()) {
      return true;
    }

    std::vector<bytecode::BcMethod> methods;
    std::vector<bool> active(module_.classes.size(), false);
    std::uint32_t current = instance->class_index;
    while (current < module_.classes.size()) {
      if (active[current]) {
        set_fault(frame, "VMError", "cycle detected in destructor chain");
        return false;
      }
      active[current] = true;
      const bytecode::BcMethod *method = find_method_in_owner_table(
          frame, current, "destroy!", kMethodFlagInstance);
      if (fault_.has_value()) {
        return false;
      }
      if (method != nullptr) {
        methods.push_back(*method);
      }

      const bytecode::BcClass &klass = module_.classes[current];
      if (!klass.has_superclass_ref) {
        break;
      }
      if (!resolve_class_ref(frame, klass.superclass_ref, &current)) {
        return false;
      }
    }

    for (const bytecode::BcMethod &method : methods) {
      const std::vector<Value> args;
      const std::vector<std::pair<std::uint32_t, Value>> kw_args;
      const std::optional<Value> result = execute_method_to_value(
          frame, method, args, kw_args, value, Value::null());
      (void)result;
      if (fault_.has_value()) {
        return false;
      }
    }
    return true;
  }

  bool perform_destroy(Frame &frame, const Value &value, ObjHeader &header,
                       bool *changed) {
    *changed = false;
    if (header_is_deallocated(header) || header_is_destroyed(header)) {
      return true;
    }
    const bool destructors_ok = run_destroy_methods(frame, value);
    header.flags &= ~kObjectFlagDestroying;
    header.flags |= kObjectFlagDestroyed;
    header.lifetime_state = ObjectLifetimeState::Destroyed;
    *changed = true;
    return destructors_ok && !fault_.has_value();
  }

  bool lifecycle_destroy(Frame &frame, const Value &value, bool *changed) {
    ObjHeader *header = nullptr;
    const std::optional<LifecycleTargetStatus> target =
        lifecycle_target_header(frame, value, &header);
    if (!target.has_value()) {
      return false;
    }
    if (*target == LifecycleTargetStatus::NonHeap) {
      *changed = false;
      return true;
    }
    if (header_is_deallocated(*header) || header_is_destroyed(*header)) {
      *changed = false;
      return true;
    }
    if (!check_lifecycle_preconditions(frame, *header)) {
      return false;
    }
    if (!check_not_pinned_for_lifecycle(frame, value)) {
      return false;
    }
    return perform_destroy(frame, value, *header, changed);
  }

  void release_payload_for_dealloc(const Value &value) {
    if (value.is_instance_object()) {
      const std::shared_ptr<InstanceValue> instance =
          value.as_instance_object();
      if (instance == nullptr) {
        return;
      }
      instance->ivar_storage.clear();
      instance->ivar_storage.shrink_to_fit();
      instance->ivars.clear();
      instance->ivar_shape_version = 0;
      instance->header.shape = state_->dead_shape;
      return;
    }
    if (value.is_list()) {
      const std::shared_ptr<ListValue> list = value.as_list();
      if (list != nullptr) {
        list->items.clear();
        list->items.shrink_to_fit();
        list->frozen = false;
        list->header.shape = state_->dead_shape;
      }
      return;
    }
    if (value.is_tuple()) {
      const std::shared_ptr<TupleValue> tuple = value.as_tuple();
      if (tuple != nullptr) {
        tuple->items.clear();
        tuple->items.shrink_to_fit();
        tuple->header.shape = state_->dead_shape;
      }
      return;
    }
    if (value.is_map()) {
      const std::shared_ptr<MapValue> map = value.as_map();
      if (map != nullptr) {
        map->entries.clear();
        map->entries.shrink_to_fit();
        map->frozen = false;
        map->header.shape = state_->dead_shape;
      }
      return;
    }
    if (value.is_closure()) {
      const std::shared_ptr<ClosureValue> closure = value.as_closure();
      if (closure != nullptr) {
        closure->captures.clear();
        closure->captures.shrink_to_fit();
        closure->self = Value::null();
        closure->code_id = 0;
        closure->header.shape = state_->dead_shape;
      }
    }
  }

  bool lifecycle_dealloc(Frame &frame, const Value &value, bool *changed) {
    ObjHeader *header = nullptr;
    const std::optional<LifecycleTargetStatus> target =
        lifecycle_target_header(frame, value, &header);
    if (!target.has_value()) {
      return false;
    }
    if (*target == LifecycleTargetStatus::NonHeap) {
      *changed = false;
      return true;
    }
    if (header_is_deallocated(*header)) {
      *changed = false;
      return true;
    }
    if (header->lifetime_state == ObjectLifetimeState::Destroying ||
        (header->flags & kObjectFlagDestroying) != 0U) {
      set_fault(frame, "LifetimeError",
                "cannot deallocate object while destroy is active");
      return false;
    }
    if (!check_lifecycle_preconditions(frame, *header)) {
      return false;
    }
    if (!check_not_pinned_for_lifecycle(frame, value)) {
      return false;
    }
    if (!header_is_destroyed(*header)) {
      bool ignored_destroyed = false;
      const bool destroyed =
          perform_destroy(frame, value, *header, &ignored_destroyed);
      if (!destroyed && !fault_.has_value()) {
        return false;
      }
    }

    release_payload_for_dealloc(value);
    header->flags &= ~kObjectFlagDestroying;
    header->flags |= kObjectFlagDestroyed | kObjectFlagDead;
    header->lifetime_state = ObjectLifetimeState::Deallocated;
    header->shape = state_->dead_shape;
    state_->heap.drain_remote_frees();
    *changed = true;
    return !fault_.has_value();
  }

  Value make_list_value(std::vector<Value> items, bool frozen = false) {
    return state_->heap.make_list_value(std::move(items), frozen);
  }

  Value make_tuple_value(std::vector<Value> items) {
    return state_->heap.make_tuple_value(std::move(items));
  }

  Value make_symbol_map_value(std::vector<MapEntry> entries,
                              bool frozen = false) {
    return state_->heap.make_symbol_map_value(std::move(entries), frozen);
  }

  std::shared_ptr<ClosureValue> make_closure_value() {
    return state_->heap.make_closure_value();
  }

  std::shared_ptr<InstanceValue>
  make_instance_value(std::uint32_t class_index) {
    return state_->heap.make_instance_value(class_index);
  }

  std::optional<Value> call_block_to_value(const Frame &frame,
                                           const Value &block,
                                           const std::vector<Value> &args) {
    if (block.is_null()) {
      set_fault(frame, "TypeError", "builtin collection SEND requires block");
      return std::nullopt;
    }
    if (!block.is_closure()) {
      set_fault(frame, "TypeError", "builtin collection block must be closure");
      return std::nullopt;
    }
    if (!ensure_lifecycle_access(frame, block)) {
      return std::nullopt;
    }
    const std::shared_ptr<ClosureValue> closure = block.as_closure();
    if (closure == nullptr) {
      set_fault(frame, "TypeError", "closure value is null");
      return std::nullopt;
    }
    const BcCode *code = find_code(module_, closure->code_id);
    if (code == nullptr) {
      set_fault(frame, "VMError", "closure code id is unknown");
      return std::nullopt;
    }

    Vm nested(module_, state_, module_id_);
    nested.push_frame(*code, args, closure->captures, closure->self,
                      Value::null(), std::nullopt);
    while (nested.fault_ == std::nullopt && !nested.frames_.empty()) {
      nested.step();
    }
    if (nested.fault_.has_value()) {
      fault_ = nested.fault_;
      return std::nullopt;
    }
    return nested.final_value_;
  }

  bool apply_write_barrier(const Frame &frame, const Value &owner,
                           const Value &value) {
    const RuntimeWriteBarrierResult barrier =
        state_->heap.write_barrier(owner, value);
    if (!barrier.ok) {
      set_fault(frame, barrier.error_name, barrier.message);
      return false;
    }
    return true;
  }

  void append_value_root(std::vector<Value> *roots, const Value &value) const {
    if (value_has_heap_payload_tag(value)) {
      roots->push_back(value);
    }
  }

  void append_frame_roots(std::vector<Value> *roots, const Frame &frame) const {
    for (std::size_t index = 0; index < frame.regs.size(); ++index) {
      if (index < frame.initialized.size() && frame.initialized[index] != 0U) {
        append_value_root(roots, frame.regs[index]);
      }
    }
    for (const Value &value : frame.captures) {
      append_value_root(roots, value);
    }
    append_value_root(roots, frame.self);
    append_value_root(roots, frame.block);
    append_value_root(roots, frame.last_result);
    if (frame.return_override.has_value()) {
      append_value_root(roots, *frame.return_override);
    }
    for (const auto &[reg, value] : frame.pending_pattern_bindings) {
      (void)reg;
      append_value_root(roots, value);
    }
    for (const auto &[reg, state] : frame.prepared_seq_regs) {
      (void)reg;
      for (const Value &value : state.items) {
        append_value_root(roots, value);
      }
    }
    for (const auto &[reg, state] : frame.prepared_map_regs) {
      (void)reg;
      for (const MapEntry &entry : state.entries) {
        append_value_root(roots, entry.value);
      }
    }
  }

  std::vector<Value> collect_gc_roots() const {
    std::vector<Value> roots;
    for (const Frame &frame : frames_) {
      append_frame_roots(&roots, frame);
    }
    for (std::size_t index = 0; index < last_completed_regs_.size(); ++index) {
      if (index < last_completed_initialized_.size() &&
          last_completed_initialized_[index] != 0U) {
        append_value_root(&roots, last_completed_regs_[index]);
      }
    }
    append_value_root(&roots, final_value_);
    for (const ClassRuntimeState &klass : state_->classes) {
      for (const auto &[name, value] : klass.cvars) {
        (void)name;
        append_value_root(&roots, value);
      }
    }
    return roots;
  }

  void run_safepoint() {
    state_->heap.drain_remote_frees();
    const std::optional<RuntimeGcCycle> requested =
        state_->heap.pending_gc_request();
    if (!requested.has_value()) {
      return;
    }
    state_->heap.collect_garbage(collect_gc_roots(), *requested, true);
  }

  TraceFrame trace_frame_for(const Frame &frame, std::uint32_t pc) const {
    TraceFrame out;
    out.module_id = module_id_;
    out.code_id = frame.code == nullptr ? 0U : frame.code->code_id;
    out.pc = pc;
    if (frame.code == nullptr) {
      return out;
    }

    for (const bytecode::SourceSpanEntry &entry : frame.code->source_spans) {
      if (entry.pc_from <= pc && pc < entry.pc_to) {
        out.file = entry.span.file;
        out.byte_start = static_cast<std::uint32_t>(entry.span.start.offset);
        out.byte_end = static_cast<std::uint32_t>(entry.span.end.offset);
        out.line = static_cast<std::uint32_t>(entry.span.start.line);
        out.column = static_cast<std::uint32_t>(entry.span.start.col);
        out.line_end = static_cast<std::uint32_t>(entry.span.end.line);
        out.column_end = static_cast<std::uint32_t>(entry.span.end.col);
        out.generated_kind = "direct";
        return out;
      }
    }

    std::uint32_t best_pc = 0;
    for (const bytecode::LineEntry &entry : module_.line_table) {
      if (entry.code_id == frame.code->code_id && entry.pc <= pc &&
          (out.line == 0U || entry.pc >= best_pc)) {
        best_pc = entry.pc;
        out.line = entry.line;
        out.line_end = entry.line;
        out.generated_kind = "line";
      }
    }
    return out;
  }

  std::string format_trace_text(const std::string &error_name,
                                const std::string &message,
                                const std::vector<TraceFrame> &trace) const {
    std::ostringstream out;
    out << error_name << ": " << message;
    for (const TraceFrame &frame : trace) {
      out << "\n  at c" << frame.code_id << ":" << frame.pc;
      if (!frame.file.empty()) {
        out << " (" << frame.file;
        if (frame.line != 0U) {
          out << ":" << frame.line;
          if (frame.column != 0U) {
            out << ":" << frame.column;
          }
        }
        out << ")";
      } else if (frame.line != 0U) {
        out << " (line " << frame.line << ")";
      }
    }
    return out.str();
  }

  Fault make_fault(const Frame &primary, const std::string &error_name,
                   const std::string &message) const {
    const std::uint32_t code_id =
        primary.code == nullptr ? 0U : primary.code->code_id;
    const std::uint32_t pc = static_cast<std::uint32_t>(primary.pc);
    Fault fault{error_name, message, code_id, pc};
    fault.trace.push_back(trace_frame_for(primary, pc));
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
      if (&*it == &primary) {
        continue;
      }
      const std::uint32_t frame_pc =
          it->active_call_pc.value_or(static_cast<std::uint32_t>(it->pc));
      fault.trace.push_back(trace_frame_for(*it, frame_pc));
    }
    fault.trace_text =
        format_trace_text(fault.error_name, fault.message, fault.trace);
    return fault;
  }

  void set_fault(const Frame &frame, const std::string &error_name,
                 const std::string &message) {
    fault_ = make_fault(frame, error_name, message);
  }

  void push_frame(const BcCode &code, const std::vector<Value> &args,
                  std::vector<Value> captures, Value self, Value block,
                  std::optional<std::uint32_t> caller_result_reg) {
    Frame frame;
    frame.code = &code;
    frame.regs.assign(code.reg_count, Value::null());
    frame.initialized.assign(code.reg_count, 0U);
    for (std::size_t i = 0; i < args.size() && i < frame.regs.size(); ++i) {
      frame.regs[i] = args[i];
      frame.initialized[i] = 1U;
    }
    frame.captures = std::move(captures);
    frame.self = std::move(self);
    frame.block = std::move(block);
    frame.caller_result_reg = caller_result_reg;
    frames_.push_back(std::move(frame));
  }

  bool operand_u32(const Frame &frame, const Instruction &insn, std::size_t idx,
                   std::uint32_t *out) {
    if (idx >= insn.operands.size()) {
      set_fault(frame, "VMError", "missing operand");
      return false;
    }
    const std::int64_t value = insn.operands[idx].value;
    if (value < 0) {
      set_fault(frame, "VMError", "negative operand in unsigned slot");
      return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
  }

  bool operand_i64(const Frame &frame, const Instruction &insn, std::size_t idx,
                   std::int64_t *out) {
    if (idx >= insn.operands.size()) {
      set_fault(frame, "VMError", "missing operand");
      return false;
    }
    *out = insn.operands[idx].value;
    return true;
  }

  std::optional<std::uint32_t> optional_operand_u32(const Frame &frame,
                                                    const Instruction &insn,
                                                    std::size_t idx) {
    if (idx >= insn.operands.size()) {
      return std::nullopt;
    }
    std::uint32_t value = 0;
    if (!operand_u32(frame, insn, idx, &value)) {
      return std::nullopt;
    }
    return value;
  }

  bool read_call_packet(Frame &frame, const Instruction &insn,
                        CallPacket *out) {
    std::uint32_t callee_reg = 0;
    std::uint32_t pos_count = 0;
    if (!operand_u32(frame, insn, 0, &out->dst) ||
        !operand_u32(frame, insn, 1, &callee_reg) ||
        !operand_u32(frame, insn, 2, &pos_count)) {
      return false;
    }

    std::size_t operand_index = 3;
    out->pos_args.clear();
    out->pos_args.reserve(pos_count);
    for (std::uint32_t i = 0; i < pos_count; ++i) {
      std::uint32_t reg = 0;
      if (!operand_u32(frame, insn, operand_index++, &reg)) {
        return false;
      }
      out->pos_args.push_back(read_reg(frame, reg));
      if (fault_.has_value()) {
        return false;
      }
    }

    std::uint32_t kw_count = 0;
    if (!operand_u32(frame, insn, operand_index++, &kw_count)) {
      return false;
    }
    out->kw_args.clear();
    out->kw_args.reserve(kw_count);
    for (std::uint32_t i = 0; i < kw_count; ++i) {
      std::uint32_t name_symbol_id = 0;
      std::uint32_t reg = 0;
      if (!operand_u32(frame, insn, operand_index++, &name_symbol_id) ||
          !operand_u32(frame, insn, operand_index++, &reg)) {
        return false;
      }
      out->kw_args.push_back({name_symbol_id, read_reg(frame, reg)});
      if (fault_.has_value()) {
        return false;
      }
    }

    std::int64_t block_reg = -1;
    if (!operand_i64(frame, insn, operand_index++, &block_reg)) {
      return false;
    }
    out->site_id = optional_operand_u32(frame, insn, operand_index++);
    if (fault_.has_value()) {
      return false;
    }
    out->callee = read_reg(frame, callee_reg);
    if (fault_.has_value()) {
      return false;
    }
    out->block = has_optional_reg(block_reg)
                     ? read_reg(frame, static_cast<std::uint32_t>(block_reg))
                     : Value::null();
    return !fault_.has_value();
  }

  Value load_constant(const Frame &frame, std::uint32_t const_id) {
    if (const_id >= module_.const_pool.size()) {
      set_fault(frame, "VMError", "constant ref out of range");
      return Value::null();
    }
    const Constant &constant = module_.const_pool[const_id];
    switch (constant.kind) {
    case ConstantKind::Null:
      return Value::null();
    case ConstantKind::Bool:
      return Value::boolean(constant.bool_value);
    case ConstantKind::Integer:
      return Value::integer(constant.int_value);
    case ConstantKind::Float:
      return Value::floating(constant.float_value);
    case ConstantKind::SymbolRef:
      return Value::symbol(constant.ref_id);
    case ConstantKind::StringRef:
      return Value::string(constant.ref_id);
    case ConstantKind::CodeRef:
    case ConstantKind::Path:
      set_fault(frame, "VMError", "constant kind is not executable in W5.1");
      return Value::null();
    case ConstantKind::KeySet: {
      std::vector<Value> items;
      items.reserve(constant.items.size());
      for (std::uint32_t symbol_id : constant.items) {
        items.push_back(Value::symbol(symbol_id));
      }
      return make_tuple_value(std::move(items));
    }
    }
    set_fault(frame, "VMError", "unknown constant kind");
    return Value::null();
  }

  bool path_segments_from_constant(const Frame &frame, const Constant &constant,
                                   std::vector<std::string> *out) {
    if (constant.kind != ConstantKind::Path) {
      set_fault(frame, "VMError",
                "path lookup expects path constant in current runtime");
      return false;
    }
    if (constant.items.empty()) {
      set_fault(frame, "VMError", "path constant is empty");
      return false;
    }
    out->clear();
    out->reserve(constant.items.size());
    for (std::uint32_t symbol_id : constant.items) {
      if (symbol_id >= module_.symbols.size()) {
        set_fault(frame, "VMError", "path symbol ref is out of range");
        return false;
      }
      out->push_back(module_.symbols[symbol_id]);
    }
    return true;
  }

  std::string join_path_segments(const std::vector<std::string> &segments) {
    std::string out;
    for (std::size_t i = 0; i < segments.size(); ++i) {
      if (i != 0U) {
        out += ".";
      }
      out += segments[i];
    }
    return out;
  }

  bool find_class_by_path_segments(const Frame &frame,
                                   const std::vector<std::string> &segments,
                                   std::uint32_t *out_class_index) {
    if (segments.empty()) {
      set_fault(frame, "VMError", "class path is empty");
      return false;
    }

    const std::string full_path = join_path_segments(segments);
    for (std::uint32_t index = 0; index < module_.classes.size(); ++index) {
      const std::uint32_t symbol_id = module_.classes[index].class_name_sym_id;
      if (symbol_id < module_.symbols.size() &&
          module_.symbols[symbol_id] == full_path) {
        *out_class_index = index;
        return true;
      }
    }

    const std::string &leaf = segments.back();
    std::optional<std::uint32_t> match;
    for (std::uint32_t index = 0; index < module_.classes.size(); ++index) {
      const std::uint32_t symbol_id = module_.classes[index].class_name_sym_id;
      if (symbol_id >= module_.symbols.size()) {
        continue;
      }
      if (module_.symbols[symbol_id] != leaf) {
        continue;
      }
      if (match.has_value()) {
        set_fault(frame, "VMError", "class path ref is ambiguous");
        return false;
      }
      match = index;
    }
    if (match.has_value()) {
      *out_class_index = *match;
      return true;
    }
    set_fault(frame, "VMError", "class path ref target is unknown");
    return false;
  }

  Value lookup_constant(const Frame &frame, std::uint32_t const_id) {
    if (const_id >= module_.const_pool.size()) {
      set_fault(frame, "VMError", "constant ref out of range");
      return Value::null();
    }
    const Constant &constant = module_.const_pool[const_id];
    if (constant.kind != ConstantKind::Path) {
      set_fault(frame, "VMError",
                "LOOKUP_CONST expects path constant in current runtime");
      return Value::null();
    }
    std::vector<std::string> segments;
    if (!path_segments_from_constant(frame, constant, &segments)) {
      return Value::null();
    }
    std::uint32_t class_index = 0;
    if (find_class_by_path_segments(frame, segments, &class_index)) {
      return Value::class_object(class_index);
    }
    return Value::null();
  }

  Value read_reg(const Frame &frame, std::uint32_t reg) {
    if (reg >= frame.regs.size()) {
      set_fault(frame, "VMError", "register out of range");
      return Value::null();
    }
    if (reg >= frame.initialized.size() || frame.initialized[reg] == 0U) {
      set_fault(frame, "NameError", "read of uninitialized local/module cell");
      return Value::null();
    }
    return frame.regs[reg];
  }

  bool write_reg(Frame &frame, std::uint32_t reg, Value value) {
    if (reg >= frame.regs.size()) {
      set_fault(frame, "VMError", "register out of range");
      return false;
    }
    if (frame.initialized.size() < frame.regs.size()) {
      frame.initialized.resize(frame.regs.size(), 0U);
    }
    frame.regs[reg] = std::move(value);
    frame.initialized[reg] = 1U;
    frame.prepared_seq_regs.erase(reg);
    frame.prepared_map_regs.erase(reg);
    frame.pending_pattern_bindings.erase(reg);
    return true;
  }

  void clear_pattern_state(Frame &frame) {
    frame.prepared_seq_regs.clear();
    frame.prepared_map_regs.clear();
    frame.pending_pattern_bindings.clear();
  }

  bool has_active_pattern_state(const Frame &frame) const {
    return !frame.prepared_seq_regs.empty() ||
           !frame.prepared_map_regs.empty() ||
           !frame.pending_pattern_bindings.empty();
  }

  std::optional<std::vector<Value>>
  extract_sequence_items(const Frame &frame, const Value &value,
                         bool *out_source_was_tuple) {
    if (value.is_list()) {
      const std::shared_ptr<ListValue> list = value.as_list();
      if (list == nullptr) {
        set_fault(frame, "TypeError", "list value is null");
        return std::nullopt;
      }
      if (!ensure_lifecycle_access(frame, value)) {
        return std::nullopt;
      }
      *out_source_was_tuple = false;
      return list->items;
    }
    if (value.is_tuple()) {
      const std::shared_ptr<TupleValue> tuple = value.as_tuple();
      if (tuple == nullptr) {
        set_fault(frame, "TypeError", "tuple value is null");
        return std::nullopt;
      }
      if (!ensure_lifecycle_access(frame, value)) {
        return std::nullopt;
      }
      *out_source_was_tuple = true;
      return tuple->items;
    }
    return std::nullopt;
  }

  std::optional<std::vector<MapEntry>> extract_map_entries(const Frame &frame,
                                                           const Value &value) {
    if (!value.is_map()) {
      return std::nullopt;
    }
    const std::shared_ptr<MapValue> map = value.as_map();
    if (map == nullptr) {
      set_fault(frame, "TypeError", "map value is null");
      return std::nullopt;
    }
    if (!ensure_lifecycle_access(frame, value)) {
      return std::nullopt;
    }
    return map->entries;
  }

  Value materialize_sequence_rest(const PreparedSeqState &state) {
    std::vector<Value> rest;
    if (state.rest_start < state.items.size()) {
      rest.assign(state.items.begin() +
                      static_cast<std::ptrdiff_t>(state.rest_start),
                  state.items.end());
    }
    if (state.source_was_tuple) {
      return make_tuple_value(std::move(rest));
    }
    return make_list_value(std::move(rest));
  }

  Value materialize_map_rest(const PreparedMapState &state) {
    std::vector<MapEntry> rest;
    std::unordered_map<std::uint32_t, bool> requested;
    for (std::uint32_t symbol_id : state.requested_keys) {
      requested[symbol_id] = true;
    }
    for (const MapEntry &entry : state.entries) {
      if (requested.find(entry.symbol_id) == requested.end()) {
        rest.push_back(entry);
      }
    }
    return make_symbol_map_value(std::move(rest));
  }

  bool prepared_map_has_extras(const PreparedMapState &state) const {
    if (!state.needs_full) {
      return false;
    }
    std::unordered_map<std::uint32_t, bool> requested;
    for (std::uint32_t symbol_id : state.requested_keys) {
      requested[symbol_id] = true;
    }
    for (const MapEntry &entry : state.entries) {
      if (requested.find(entry.symbol_id) == requested.end()) {
        return true;
      }
    }
    return false;
  }

  bool finalize_pattern_success(Frame &frame, bool allow_pending_bindings) {
    if (!has_active_pattern_state(frame)) {
      return true;
    }
    std::optional<std::uint32_t> fail_pc;
    for (const auto &[reg, state] : frame.prepared_map_regs) {
      (void)reg;
      if (state.needs_full && !state.rest_bound &&
          prepared_map_has_extras(state)) {
        fail_pc = state.fail_pc;
        break;
      }
    }
    if (fail_pc.has_value()) {
      if (*fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return false;
      }
      clear_pattern_state(frame);
      frame.pc = *fail_pc;
      return false;
    }
    if (!allow_pending_bindings && !frame.pending_pattern_bindings.empty()) {
      set_fault(frame, "VMError",
                "pattern bindings reached success boundary without P_COMMIT");
      clear_pattern_state(frame);
      return false;
    }
    frame.prepared_seq_regs.clear();
    frame.prepared_map_regs.clear();
    return true;
  }

  std::optional<std::string>
  selector_text_from_symbol(std::uint32_t symbol_id) {
    if (symbol_id >= module_.symbols.size()) {
      return std::nullopt;
    }
    return module_.symbols[symbol_id];
  }

  std::optional<std::string> string_text_from_id(std::uint32_t string_id) {
    if (string_id >= module_.strings.size()) {
      return std::nullopt;
    }
    return module_.strings[string_id];
  }

  std::optional<std::uint32_t> symbol_id_for_text(const std::string &text) {
    for (std::uint32_t i = 0; i < module_.symbols.size(); ++i) {
      if (module_.symbols[i] == text) {
        return i;
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> selector_text_from_value(const Value &value) {
    if (value.is_symbol()) {
      return selector_text_from_symbol(value.as_symbol().symbol_id);
    }
    if (value.is_string()) {
      const std::uint32_t string_id = value.as_string().string_id;
      if (string_id >= module_.strings.size()) {
        return std::nullopt;
      }
      return module_.strings[string_id];
    }
    return std::nullopt;
  }

  std::optional<std::string> class_name_for_index(std::uint32_t class_index) {
    if (class_index >= module_.classes.size()) {
      return std::nullopt;
    }
    const std::uint32_t symbol_id =
        module_.classes[class_index].class_name_sym_id;
    if (symbol_id >= module_.symbols.size()) {
      return std::nullopt;
    }
    return module_.symbols[symbol_id];
  }

  std::string exception_error_name(const Value &exception) {
    if (exception.is_instance_object()) {
      const std::shared_ptr<InstanceValue> instance =
          exception.as_instance_object();
      if (instance != nullptr) {
        const std::optional<std::string> name =
            class_name_for_index(instance->class_index);
        if (name.has_value()) {
          return *name;
        }
      }
    }
    if (exception.is_class_object()) {
      const std::optional<std::string> name =
          class_name_for_index(exception.as_class_object().class_index);
      if (name.has_value()) {
        return *name;
      }
    }
    if (exception.is_symbol()) {
      const std::optional<std::string> name =
          selector_text_from_symbol(exception.as_symbol().symbol_id);
      if (name.has_value()) {
        return *name;
      }
    }
    if (exception.is_string()) {
      const std::optional<std::string> name =
          string_text_from_id(exception.as_string().string_id);
      if (name.has_value()) {
        return *name;
      }
    }
    return "Exception";
  }

  Value
  selector_value_for_method_missing(const std::string &selector_text,
                                    const std::optional<Value> &original) {
    if (original.has_value() && original->is_symbol()) {
      return *original;
    }
    const std::optional<std::uint32_t> symbol_id =
        symbol_id_for_text(selector_text);
    if (symbol_id.has_value()) {
      return Value::symbol(*symbol_id);
    }
    if (original.has_value()) {
      return *original;
    }
    return Value::null();
  }

  std::uint64_t inline_cache_key(const Frame &frame,
                                 std::uint32_t site_id) const {
    const std::uint64_t code_id =
        frame.code == nullptr ? 0U : frame.code->code_id;
    return (code_id << 32U) | site_id;
  }

  std::vector<std::uint32_t> canonical_keyword_shape(
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args) const {
    std::vector<std::uint32_t> shape;
    shape.reserve(kw_args.size());
    for (const auto &[symbol_id, value] : kw_args) {
      (void)value;
      shape.push_back(symbol_id);
    }
    std::sort(shape.begin(), shape.end());
    return shape;
  }

  const bytecode::BcMethod *probe_call_cache(
      const Frame &frame, std::uint32_t site_id,
      std::uint32_t receiver_class_index, std::uint32_t dispatch_flags,
      std::uint32_t selector_symbol_id, std::uint32_t positional_count,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      const Value &block) {
    const auto found =
        state_->call_caches.find(inline_cache_key(frame, site_id));
    if (found == state_->call_caches.end()) {
      ++state_->call_cache_misses;
      return nullptr;
    }
    const CallCacheEntry &entry = found->second;
    if (!entry.valid || entry.receiver_class_index != receiver_class_index ||
        entry.dispatch_flags != dispatch_flags ||
        entry.selector_symbol_id != selector_symbol_id ||
        entry.positional_count != positional_count ||
        entry.keyword_shape != canonical_keyword_shape(kw_args) ||
        entry.has_block != !block.is_null() ||
        entry.world_epoch != state_->world_epoch ||
        receiver_class_index >= state_->classes.size() ||
        entry.method_version !=
            state_->classes[receiver_class_index].method_version) {
      ++state_->call_cache_misses;
      return nullptr;
    }
    ++state_->call_cache_hits;
    return &entry.method;
  }

  void update_call_cache(
      const Frame &frame, std::uint32_t site_id,
      std::uint32_t receiver_class_index, std::uint32_t dispatch_flags,
      std::uint32_t selector_symbol_id, std::uint32_t positional_count,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      const Value &block, const bytecode::BcMethod &method) {
    if (receiver_class_index >= state_->classes.size()) {
      return;
    }
    CallCacheEntry entry;
    entry.valid = true;
    entry.receiver_class_index = receiver_class_index;
    entry.dispatch_flags = dispatch_flags;
    entry.selector_symbol_id = selector_symbol_id;
    entry.positional_count = positional_count;
    entry.keyword_shape = canonical_keyword_shape(kw_args);
    entry.has_block = !block.is_null();
    entry.method_version = state_->classes[receiver_class_index].method_version;
    entry.world_epoch = state_->world_epoch;
    entry.method = method;
    state_->call_caches[inline_cache_key(frame, site_id)] = entry;
    ++state_->call_cache_updates;
  }

  std::optional<std::uint32_t> probe_ivar_cache(const Frame &frame,
                                                std::uint32_t site_id,
                                                const InstanceValue &instance,
                                                std::uint32_t symbol_id) {
    const auto found =
        state_->ivar_caches.find(inline_cache_key(frame, site_id));
    if (found == state_->ivar_caches.end()) {
      return std::nullopt;
    }
    const IvarCacheEntry &entry = found->second;
    const std::shared_ptr<const ShapeDescriptor> shape = instance.header.shape;
    if (shape == nullptr || shape->dead) {
      return std::nullopt;
    }
    if (!entry.valid || entry.receiver_class_index != instance.class_index ||
        entry.symbol_id != symbol_id || entry.shape_id != shape->shape_id ||
        entry.shape_version != shape->shape_version ||
        entry.slot_index >= instance.ivar_storage.size()) {
      return std::nullopt;
    }
    return entry.slot_index;
  }

  void update_ivar_cache(const Frame &frame, std::uint32_t site_id,
                         const InstanceValue &instance, std::uint32_t symbol_id,
                         std::uint32_t slot_index) {
    const std::shared_ptr<const ShapeDescriptor> shape = instance.header.shape;
    if (shape == nullptr || shape->dead) {
      return;
    }
    IvarCacheEntry entry;
    entry.valid = true;
    entry.receiver_class_index = instance.class_index;
    entry.symbol_id = symbol_id;
    entry.shape_id = shape->shape_id;
    entry.shape_version = shape->shape_version;
    entry.slot_index = slot_index;
    state_->ivar_caches[inline_cache_key(frame, site_id)] = std::move(entry);
  }

  bool has_optional_reg(std::int64_t raw) const {
    return raw >= 0 && raw != static_cast<std::int64_t>(
                                  std::numeric_limits<std::uint32_t>::max());
  }

  bool expect_instance_receiver(const Frame &frame, const Value &receiver,
                                std::shared_ptr<InstanceValue> *out) {
    if (!receiver.is_instance_object()) {
      set_fault(frame, "TypeError",
                "ivar access expects instance receiver in current runtime");
      return false;
    }
    *out = receiver.as_instance_object();
    if (*out == nullptr) {
      set_fault(frame, "TypeError", "instance receiver is null");
      return false;
    }
    if (!ensure_instance_layout(frame, *out)) {
      return false;
    }
    return true;
  }

  bool ensure_instance_layout(const Frame &frame,
                              const std::shared_ptr<InstanceValue> &instance) {
    if (instance == nullptr) {
      set_fault(frame, "TypeError", "instance receiver is null");
      return false;
    }
    if (instance->class_index >= state_->classes.size()) {
      state_->classes.resize(static_cast<std::size_t>(instance->class_index) +
                             1U);
    }
    instance->header.kind = HeapObjectKind::Instance;
    instance->header.class_index = instance->class_index;
    const std::optional<std::string> lifecycle_error =
        lifecycle_access_error_name(instance->header);
    if (lifecycle_error.has_value()) {
      set_fault(frame, *lifecycle_error,
                lifecycle_access_error_message(*lifecycle_error));
      return false;
    }
    if (instance->header.shape == nullptr) {
      instance->header.shape =
          state_->root_shape_for_class(instance->class_index);
      if (!instance->ivars.empty()) {
        std::map<std::string, Value> ordered(instance->ivars.begin(),
                                             instance->ivars.end());
        instance->ivar_storage.clear();
        for (const auto &[name, value] : ordered) {
          if (!store_instance_ivar_slow(frame, instance, name, value)) {
            return false;
          }
        }
      }
    }
    if (instance->header.shape == nullptr) {
      set_fault(frame, "VMError", "instance shape is missing");
      return false;
    }
    if (instance->ivar_storage.size() <
        instance->header.shape->slot_names.size()) {
      instance->ivar_storage.resize(instance->header.shape->slot_names.size(),
                                    Value::null());
    }
    instance->ivar_shape_version = instance->header.shape->shape_version;
    return true;
  }

  bool store_instance_ivar_slow(const Frame &frame,
                                const std::shared_ptr<InstanceValue> &instance,
                                const std::string &name, Value value) {
    if (instance == nullptr) {
      set_fault(frame, "TypeError", "instance receiver is null");
      return false;
    }
    const std::optional<std::string> lifecycle_error =
        lifecycle_access_error_name(instance->header);
    if (lifecycle_error.has_value()) {
      set_fault(frame, *lifecycle_error,
                lifecycle_access_error_message(*lifecycle_error));
      return false;
    }
    if (instance->header.shape == nullptr || instance->header.shape->dead) {
      set_fault(frame, "UseAfterFreeError", "access to deallocated object");
      return false;
    }
    if (!apply_write_barrier(frame, Value::instance(instance), value)) {
      return false;
    }
    std::shared_ptr<const ShapeDescriptor> shape = instance->header.shape;
    const auto existing_slot = shape->ivar_slots.find(name);
    if (existing_slot == shape->ivar_slots.end()) {
      const std::shared_ptr<const ShapeDescriptor> next =
          state_->transition_shape(shape, name);
      if (next == nullptr || next->dead) {
        set_fault(frame, "UseAfterFreeError", "access to deallocated object");
        return false;
      }
      std::vector<Value> storage(next->slot_names.size(), Value::null());
      for (std::size_t i = 0;
           i < instance->ivar_storage.size() && i < storage.size(); ++i) {
        storage[i] = instance->ivar_storage[i];
      }
      instance->ivar_storage = std::move(storage);
      instance->header.shape = next;
      shape = next;
    } else if (instance->ivar_storage.size() < shape->slot_names.size()) {
      instance->ivar_storage.resize(shape->slot_names.size(), Value::null());
    }

    const auto slot = shape->ivar_slots.find(name);
    if (slot == shape->ivar_slots.end() ||
        slot->second >= instance->ivar_storage.size()) {
      set_fault(frame, "VMError", "ivar slot allocation failed");
      return false;
    }
    instance->ivar_storage[slot->second] = value;
    instance->ivars[name] = std::move(value);
    instance->ivar_shape_version = shape->shape_version;
    return true;
  }

  std::optional<std::uint32_t>
  ensure_instance_ivar_slot(const Frame &frame,
                            const std::shared_ptr<InstanceValue> &instance,
                            const std::string &name) {
    if (!ensure_instance_layout(frame, instance)) {
      return std::nullopt;
    }
    const std::shared_ptr<const ShapeDescriptor> shape = instance->header.shape;
    const auto slot = shape->ivar_slots.find(name);
    if (slot != shape->ivar_slots.end()) {
      return slot->second;
    }
    const auto legacy = instance->ivars.find(name);
    if (legacy == instance->ivars.end()) {
      return std::nullopt;
    }
    const Value value = legacy->second;
    if (!store_instance_ivar_slow(frame, instance, name, value)) {
      return std::nullopt;
    }
    const auto imported_slot = instance->header.shape->ivar_slots.find(name);
    if (imported_slot == instance->header.shape->ivar_slots.end()) {
      set_fault(frame, "VMError", "legacy ivar import failed");
      return std::nullopt;
    }
    return imported_slot->second;
  }

  bool store_instance_ivar(const Frame &frame,
                           const std::shared_ptr<InstanceValue> &instance,
                           const std::string &name, Value value,
                           std::uint32_t symbol_id, std::uint32_t site_id) {
    if (!ensure_instance_layout(frame, instance)) {
      return false;
    }
    if (!store_instance_ivar_slow(frame, instance, name, std::move(value))) {
      return false;
    }
    const auto slot = instance->header.shape->ivar_slots.find(name);
    if (slot == instance->header.shape->ivar_slots.end()) {
      set_fault(frame, "VMError", "stored ivar slot is missing");
      return false;
    }
    update_ivar_cache(frame, site_id, *instance, symbol_id, slot->second);
    return true;
  }

  bool expect_class_owner(const Frame &frame, const Value &owner,
                          std::uint32_t *out_class_index) {
    if (owner.is_class_object()) {
      const std::uint32_t class_index = owner.as_class_object().class_index;
      if (class_index >= state_->classes.size()) {
        set_fault(frame, "VMError", "class owner index is out of range");
        return false;
      }
      *out_class_index = class_index;
      return true;
    }
    if (owner.is_instance_object()) {
      const std::shared_ptr<InstanceValue> instance =
          owner.as_instance_object();
      if (instance == nullptr) {
        set_fault(frame, "TypeError", "instance owner is null");
        return false;
      }
      if (!ensure_instance_layout(frame, instance)) {
        return false;
      }
      *out_class_index = instance->class_index;
      return true;
    }
    set_fault(frame, "TypeError",
              "class-variable access expects class or instance owner");
    return false;
  }

  bool value_is_instance_of(const Frame &frame, std::uint32_t value_class_index,
                            std::uint32_t target_class_index) {
    std::vector<bool> active(module_.classes.size(), false);
    std::uint32_t current = value_class_index;
    while (true) {
      if (current >= module_.classes.size()) {
        set_fault(frame, "VMError", "instance class index is out of range");
        return false;
      }
      if (current == target_class_index) {
        return true;
      }
      if (active[current]) {
        set_fault(frame, "VMError", "cycle detected in superclass chain");
        return false;
      }
      active[current] = true;
      const bytecode::BcClass &klass = module_.classes[current];
      if (!klass.has_superclass_ref) {
        return false;
      }
      if (!resolve_class_ref(frame, klass.superclass_ref, &current)) {
        return false;
      }
      if (fault_.has_value()) {
        return false;
      }
    }
  }

  bool pattern_triple_eq(Frame &frame, const Value &matcher,
                         const Value &value, bool *out) {
    if (matcher.is_class_object()) {
      const std::uint32_t target_class_index =
          matcher.as_class_object().class_index;
      if (value.is_instance_object()) {
        if (!ensure_lifecycle_access(frame, value)) {
          return false;
        }
        const std::shared_ptr<InstanceValue> instance =
            value.as_instance_object();
        if (instance == nullptr) {
          set_fault(frame, "TypeError", "instance matcher value is null");
          return false;
        }
        *out = value_is_instance_of(frame, instance->class_index,
                                    target_class_index);
        return !fault_.has_value();
      }
      if (value.is_class_object()) {
        *out = value.as_class_object().class_index == target_class_index;
        return true;
      }
      *out = false;
      return true;
    }

    Value result = Value::null();
    bool handled = false;
    if (!try_apply_protocol_send(frame, matcher, "===", {value}, &result,
                                 &handled)) {
      return false;
    }
    if (handled) {
      if (!result.is_bool()) {
        set_fault(frame, "TypeError", "pattern triple-eq did not return bool");
        return false;
      }
      *out = result.as_bool();
      return true;
    }

    *out = value_equals(matcher, value);
    return true;
  }

  bool load_method_params(const Frame &frame, const bytecode::BcMethod &method,
                          std::vector<bytecode::MethodParamEntry> *out) {
    if (!method.params.empty()) {
      *out = method.params;
      return true;
    }
    if (method.signature_blob_id >= module_.const_pool.size()) {
      set_fault(frame, "VMError", "method signature blob ref is out of range");
      return false;
    }
    const Constant &constant = module_.const_pool[method.signature_blob_id];
    if (constant.kind != ConstantKind::Path) {
      set_fault(frame, "VMError", "method signature blob is not a path record");
      return false;
    }
    out->clear();
    out->reserve(constant.items.size());
    for (std::uint32_t symbol_id : constant.items) {
      bytecode::MethodParamEntry entry;
      entry.external_name_sym_id = symbol_id;
      out->push_back(entry);
    }
    if (method.default_thunk_ids.size() > out->size()) {
      set_fault(frame, "VMError", "default thunk table exceeds param count");
      return false;
    }
    const std::size_t defaults_begin =
        out->size() - method.default_thunk_ids.size();
    for (std::size_t index = defaults_begin; index < out->size(); ++index) {
      (*out)[index].flags |= bytecode::kMethodParamFlagHasDefault;
    }
    return true;
  }

  std::optional<std::uint32_t>
  local_slot_for_name(const Frame &frame, const BcCode &code,
                      std::uint32_t local_name_str_id) {
    for (const bytecode::SlotLayoutEntry &entry : code.local_layout) {
      if (entry.name_str_id == local_name_str_id) {
        return entry.slot;
      }
    }
    set_fault(frame, "VMError", "auto-assign local slot is missing");
    return std::nullopt;
  }

  bool apply_auto_assigns(Frame &frame, const bytecode::BcMethod &method,
                          const BcCode &code) {
    if (method.auto_assign_desc.empty()) {
      return true;
    }
    std::shared_ptr<InstanceValue> instance;
    bool have_instance = false;
    for (const bytecode::AutoAssignEntry &entry : method.auto_assign_desc) {
      const std::optional<std::uint32_t> slot =
          local_slot_for_name(frame, code, entry.local_name_str_id);
      if (!slot.has_value()) {
        return false;
      }
      if (*slot >= frame.regs.size()) {
        set_fault(frame, "VMError", "auto-assign local slot is out of range");
        return false;
      }
      const std::optional<std::string> target =
          string_text_from_id(entry.target_name_str_id);
      if (!target.has_value()) {
        set_fault(frame, "VMError", "auto-assign target string is invalid");
        return false;
      }
      if (target->rfind("@@", 0) == 0) {
        std::uint32_t class_index = 0;
        if (!expect_class_owner(frame, frame.self, &class_index)) {
          return false;
        }
        const Value value = read_reg(frame, *slot);
        if (fault_.has_value()) {
          return false;
        }
        state_->classes[class_index].cvars[target->substr(2)] = value;
        continue;
      }
      if (target->empty() || (*target)[0] != '@') {
        set_fault(frame, "VMError", "auto-assign target is not an ivar");
        return false;
      }
      if (!have_instance) {
        if (!expect_instance_receiver(frame, frame.self, &instance)) {
          return false;
        }
        have_instance = true;
      }
      const std::string ivar_name = target->substr(1);
      const Value value = read_reg(frame, *slot);
      if (fault_.has_value()) {
        return false;
      }
      if (!store_instance_ivar_slow(frame, instance, ivar_name, value)) {
        return false;
      }
    }
    return true;
  }

  bool
  shape_method_call(const Frame &frame, const bytecode::BcMethod &method,
                    const std::vector<Value> &pos_args,
                    const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
                    std::vector<bytecode::MethodParamEntry> *out_params,
                    std::vector<BoundMethodArg> *out_slots) {
    if (!load_method_params(frame, method, out_params)) {
      return false;
    }
    out_slots->assign(out_params->size(), {});

    for (std::size_t i = 0; i < kw_args.size(); ++i) {
      for (std::size_t j = i + 1; j < kw_args.size(); ++j) {
        if (kw_args[i].first == kw_args[j].first) {
          set_fault(frame, "TypeError",
                    "duplicate keyword argument in runtime dispatch");
          return false;
        }
      }
    }

    std::size_t positional_cursor = 0;
    for (std::size_t i = 0; i < out_params->size(); ++i) {
      const bytecode::MethodParamEntry &entry = (*out_params)[i];
      if ((entry.flags & bytecode::kMethodParamFlagKeyword) != 0U) {
        continue;
      }
      if (positional_cursor < pos_args.size()) {
        (*out_slots)[i].present = true;
        (*out_slots)[i].value = pos_args[positional_cursor++];
      }
    }
    if (positional_cursor != pos_args.size()) {
      set_fault(frame, "TypeError", "too many positional arguments");
      return false;
    }

    std::vector<bool> kw_consumed(kw_args.size(), false);
    for (std::size_t i = 0; i < out_params->size(); ++i) {
      const bytecode::MethodParamEntry &entry = (*out_params)[i];
      if ((entry.flags & bytecode::kMethodParamFlagKeyword) == 0U) {
        continue;
      }
      for (std::size_t kw_index = 0; kw_index < kw_args.size(); ++kw_index) {
        if (kw_args[kw_index].first == entry.external_name_sym_id) {
          (*out_slots)[i].present = true;
          (*out_slots)[i].value = kw_args[kw_index].second;
          kw_consumed[kw_index] = true;
          break;
        }
      }
    }

    for (std::size_t kw_index = 0; kw_index < kw_args.size(); ++kw_index) {
      if (!kw_consumed[kw_index]) {
        set_fault(frame, "TypeError", "unknown keyword argument");
        return false;
      }
    }

    for (std::size_t i = 0; i < out_params->size(); ++i) {
      if ((*out_slots)[i].present) {
        continue;
      }
      if (((*out_params)[i].flags & bytecode::kMethodParamFlagHasDefault) ==
          0U) {
        set_fault(frame, "TypeError", "missing required parameter");
        return false;
      }
    }
    return true;
  }

  bool
  materialize_defaults(Frame &frame, const bytecode::BcMethod &method,
                       const std::vector<bytecode::MethodParamEntry> &params,
                       const std::vector<BoundMethodArg> &slots) {
    if (params.size() > frame.regs.size()) {
      set_fault(frame, "VMError", "parameter slots exceed frame register file");
      return false;
    }
    for (std::size_t slot = 0; slot < slots.size(); ++slot) {
      if (slots[slot].present) {
        frame.regs[slot] = slots[slot].value;
        if (frame.initialized.size() < frame.regs.size()) {
          frame.initialized.resize(frame.regs.size(), 0U);
        }
        frame.initialized[slot] = 1U;
      }
    }

    std::size_t thunk_index = 0;
    for (std::size_t slot = 0; slot < params.size(); ++slot) {
      if ((params[slot].flags & bytecode::kMethodParamFlagHasDefault) == 0U) {
        continue;
      }
      if (!slots[slot].present) {
        if (thunk_index >= method.default_thunk_ids.size()) {
          set_fault(frame, "VMError",
                    "missing default thunk for parameter slot");
          return false;
        }
        Vm nested(module_, state_, module_id_);
        const ExecutionResult result =
            nested.execute(method.default_thunk_ids[thunk_index], frame.regs,
                           frame.self, frame.block);
        if (!result.ok()) {
          fault_ = result.fault;
          return false;
        }
        frame.regs[slot] = result.value;
        if (frame.initialized.size() < frame.regs.size()) {
          frame.initialized.resize(frame.regs.size(), 0U);
        }
        frame.initialized[slot] = 1U;
      }
      ++thunk_index;
    }
    return true;
  }

  NestedExecution
  execute_nested_code(std::uint32_t code_id, const std::vector<Value> &regs,
                      const std::vector<std::uint8_t> &initialized,
                      const Value &self, const Value &block) {
    NestedExecution out;
    const BcCode *entry = find_code(module_, code_id);
    if (entry == nullptr) {
      out.fault = Fault{"VMError", "unknown nested code id", code_id, 0};
      return out;
    }

    Vm nested(module_, state_, module_id_);
    nested.push_frame(*entry, {}, {}, self, block, std::nullopt);
    Frame &nested_frame = nested.frames_.back();
    const std::size_t copy_count =
        std::min(regs.size(), nested_frame.regs.size());
    for (std::size_t i = 0; i < copy_count; ++i) {
      nested_frame.regs[i] = regs[i];
      nested_frame.initialized[i] =
          i < initialized.size() ? initialized[i] : 1U;
    }

    while (nested.fault_ == std::nullopt && !nested.frames_.empty()) {
      nested.step();
    }

    out.value = nested.final_value_;
    out.regs = std::move(nested.last_completed_regs_);
    out.initialized = std::move(nested.last_completed_initialized_);
    out.fault = nested.fault_;
    return out;
  }

  NestedExecution execute_prepared_frame(Frame frame) {
    NestedExecution out;
    Vm nested(module_, state_, module_id_);
    nested.frames_.push_back(std::move(frame));
    while (nested.fault_ == std::nullopt && !nested.frames_.empty()) {
      nested.step();
    }
    out.value = nested.final_value_;
    out.regs = std::move(nested.last_completed_regs_);
    out.initialized = std::move(nested.last_completed_initialized_);
    out.fault = nested.fault_;
    return out;
  }

  std::optional<Value> execute_method_to_value(
      Frame &caller, const bytecode::BcMethod &method,
      const std::vector<Value> &pos_args,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      const Value &self, const Value &block) {
    std::vector<bytecode::MethodParamEntry> params;
    std::vector<BoundMethodArg> slots;
    if (!shape_method_call(caller, method, pos_args, kw_args, &params,
                           &slots)) {
      return std::nullopt;
    }
    const BcCode *code = find_code(module_, method.entry_code_id);
    if (code == nullptr) {
      set_fault(caller, "VMError", "method entry code id is unknown");
      return std::nullopt;
    }

    Frame callee;
    callee.code = code;
    callee.regs.assign(code->reg_count, Value::null());
    callee.initialized.assign(code->reg_count, 0U);
    callee.self = self;
    callee.block = block;
    if (!materialize_defaults(callee, method, params, slots) ||
        !apply_auto_assigns(callee, method, *code)) {
      return std::nullopt;
    }

    if (method.clause_table.empty()) {
      NestedExecution body = execute_prepared_frame(std::move(callee));
      if (!body.ok()) {
        fault_ = body.fault;
        return std::nullopt;
      }
      return body.value;
    }

    const std::vector<Value> base_regs = callee.regs;
    const std::vector<std::uint8_t> base_initialized = callee.initialized;
    for (const bytecode::ClauseEntry &entry : method.clause_table) {
      const NestedExecution pattern = execute_nested_code(
          entry.pattern_code_id, base_regs, base_initialized, self, block);
      if (!pattern.ok()) {
        fault_ = pattern.fault;
        return std::nullopt;
      }
      if (!pattern.value.is_bool()) {
        set_fault(caller, "VMError",
                  "clause pattern probe did not return bool");
        return std::nullopt;
      }
      if (!pattern.value.as_bool()) {
        continue;
      }

      std::vector<Value> matched_regs = base_regs;
      std::vector<std::uint8_t> matched_initialized = base_initialized;
      const std::size_t copy_count =
          std::min(matched_regs.size(), pattern.regs.size());
      for (std::size_t i = 0; i < copy_count; ++i) {
        matched_regs[i] = pattern.regs[i];
        matched_initialized[i] =
            i < pattern.initialized.size() ? pattern.initialized[i] : 1U;
      }

      const NestedExecution guard = execute_nested_code(
          entry.guard_code_id, matched_regs, matched_initialized, self, block);
      if (!guard.ok()) {
        fault_ = guard.fault;
        return std::nullopt;
      }
      if (!is_truthy(guard.value)) {
        continue;
      }

      const NestedExecution body = execute_nested_code(
          entry.body_code_id, matched_regs, matched_initialized, self, block);
      if (!body.ok()) {
        fault_ = body.fault;
        return std::nullopt;
      }
      return body.value;
    }

    NestedExecution fallback = execute_nested_code(
        method.entry_code_id, base_regs, base_initialized, self, block);
    if (!fallback.ok()) {
      fault_ = fallback.fault;
      return std::nullopt;
    }
    return fallback.value;
  }

  bool try_apply_protocol_send(Frame &frame, const Value &receiver,
                               const std::string &selector,
                               const std::vector<Value> &args, Value *out,
                               bool *handled) {
    *handled = false;
    const SendStatus scalar_status = try_apply_scalar_send(
        frame, receiver, selector, args, Value::null(), false, out);
    if (scalar_status == SendStatus::Faulted) {
      return false;
    }
    if (scalar_status == SendStatus::Matched) {
      *handled = true;
      return true;
    }

    std::uint32_t class_index = 0;
    std::uint32_t dispatch_flags = 0;
    if (receiver.is_instance_object()) {
      const std::shared_ptr<InstanceValue> instance =
          receiver.as_instance_object();
      if (instance == nullptr) {
        set_fault(frame, "TypeError", "instance receiver is null");
        return false;
      }
      class_index = instance->class_index;
      dispatch_flags = kMethodFlagInstance;
    } else if (receiver.is_class_object()) {
      class_index = receiver.as_class_object().class_index;
      dispatch_flags = kMethodFlagClass;
    } else {
      return true;
    }

    const bytecode::BcMethod *method =
        find_method_for_dispatch(frame, class_index, selector, dispatch_flags);
    if (fault_.has_value()) {
      return false;
    }
    if (method == nullptr) {
      return true;
    }
    const std::vector<std::pair<std::uint32_t, Value>> kw_args;
    const std::optional<Value> value = execute_method_to_value(
        frame, *method, args, kw_args, receiver, Value::null());
    if (!value.has_value()) {
      return false;
    }
    *out = *value;
    *handled = true;
    return true;
  }

  std::optional<CoercedSeqState>
  coerce_sequence_for_pattern(Frame &frame, const Value &source) {
    bool source_was_tuple = false;
    const std::optional<std::vector<Value>> native_items =
        extract_sequence_items(frame, source, &source_was_tuple);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (native_items.has_value()) {
      return CoercedSeqState{source, *native_items, source_was_tuple};
    }

    Value result = Value::null();
    bool handled = false;
    if (!try_apply_protocol_send(frame, source, "deconstruct", {}, &result,
                                 &handled)) {
      return std::nullopt;
    }
    if (!handled || result.is_null()) {
      return std::nullopt;
    }
    bool result_was_tuple = false;
    const std::optional<std::vector<Value>> result_items =
        extract_sequence_items(frame, result, &result_was_tuple);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (!result_items.has_value()) {
      set_fault(frame, "TypeError", "deconstruct returned non-sequence value");
      return std::nullopt;
    }
    return CoercedSeqState{result, *result_items, result_was_tuple};
  }

  std::optional<CoercedMapState> coerce_map_for_pattern(Frame &frame,
                                                        const Value &source,
                                                        const Constant &keyset,
                                                        bool needs_full) {
    const std::optional<std::vector<MapEntry>> native_entries =
        extract_map_entries(frame, source);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (native_entries.has_value()) {
      return CoercedMapState{source, *native_entries};
    }

    Value keys_arg = Value::null();
    if (!needs_full) {
      std::vector<Value> keys;
      keys.reserve(keyset.items.size());
      for (std::uint32_t symbol_id : keyset.items) {
        keys.push_back(Value::symbol(symbol_id));
      }
      keys_arg = make_tuple_value(std::move(keys));
    }

    Value result = Value::null();
    bool handled = false;
    if (!try_apply_protocol_send(frame, source, "deconstruct_keys", {keys_arg},
                                 &result, &handled)) {
      return std::nullopt;
    }
    if (!handled || result.is_null()) {
      return std::nullopt;
    }
    const std::optional<std::vector<MapEntry>> result_entries =
        extract_map_entries(frame, result);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (!result_entries.has_value()) {
      set_fault(frame, "TypeError", "deconstruct_keys returned non-map value");
      return std::nullopt;
    }
    return CoercedMapState{result, *result_entries};
  }

  bool complete_invoke_result(Frame &caller,
                              std::optional<std::uint32_t> caller_result_reg,
                              Value value) {
    if (caller_result_reg.has_value()) {
      if (!write_reg(caller, *caller_result_reg, std::move(value))) {
        return false;
      }
    } else {
      caller.last_result = std::move(value);
    }
    ++caller.pc;
    return true;
  }

  bool execute_clause_method(
      Frame &caller, const bytecode::BcMethod &method, const BcCode &entry_code,
      const std::vector<bytecode::MethodParamEntry> &params,
      const std::vector<BoundMethodArg> &slots, const Value &self,
      const Value &block, std::optional<std::uint32_t> caller_result_reg,
      const std::optional<Value> &return_override) {
    Frame callee;
    callee.code = &entry_code;
    callee.regs.assign(entry_code.reg_count, Value::null());
    callee.initialized.assign(entry_code.reg_count, 0U);
    callee.self = self;
    callee.block = block;

    if (!materialize_defaults(callee, method, params, slots) ||
        !apply_auto_assigns(callee, method, entry_code)) {
      return false;
    }

    const std::vector<Value> base_regs = callee.regs;
    const std::vector<std::uint8_t> base_initialized = callee.initialized;
    for (const bytecode::ClauseEntry &entry : method.clause_table) {
      const NestedExecution pattern = execute_nested_code(
          entry.pattern_code_id, base_regs, base_initialized, self, block);
      if (!pattern.ok()) {
        fault_ = pattern.fault;
        return false;
      }
      if (!pattern.value.is_bool()) {
        set_fault(caller, "VMError",
                  "clause pattern probe did not return bool");
        return false;
      }
      if (!pattern.value.as_bool()) {
        continue;
      }

      std::vector<Value> matched_regs = base_regs;
      std::vector<std::uint8_t> matched_initialized = base_initialized;
      const std::size_t copy_count =
          std::min(matched_regs.size(), pattern.regs.size());
      for (std::size_t i = 0; i < copy_count; ++i) {
        matched_regs[i] = pattern.regs[i];
        matched_initialized[i] =
            i < pattern.initialized.size() ? pattern.initialized[i] : 1U;
      }

      const NestedExecution guard = execute_nested_code(
          entry.guard_code_id, matched_regs, matched_initialized, self, block);
      if (!guard.ok()) {
        fault_ = guard.fault;
        return false;
      }
      if (!is_truthy(guard.value)) {
        continue;
      }

      const NestedExecution body = execute_nested_code(
          entry.body_code_id, matched_regs, matched_initialized, self, block);
      if (!body.ok()) {
        fault_ = body.fault;
        return false;
      }

      Value value = return_override.has_value() ? *return_override : body.value;
      return complete_invoke_result(caller, caller_result_reg,
                                    std::move(value));
    }

    const NestedExecution fallback = execute_nested_code(
        method.entry_code_id, base_regs, base_initialized, self, block);
    if (!fallback.ok()) {
      fault_ = fallback.fault;
      return false;
    }
    Value value =
        return_override.has_value() ? *return_override : fallback.value;
    return complete_invoke_result(caller, caller_result_reg, std::move(value));
  }

  bool
  invoke_method(Frame &caller, const bytecode::BcMethod &method,
                const std::vector<Value> &pos_args,
                const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
                Value self, Value block,
                std::optional<std::uint32_t> caller_result_reg,
                std::optional<Value> return_override = std::nullopt) {
    std::vector<bytecode::MethodParamEntry> params;
    std::vector<BoundMethodArg> slots;
    if (!shape_method_call(caller, method, pos_args, kw_args, &params,
                           &slots)) {
      return false;
    }
    const BcCode *code = find_code(module_, method.entry_code_id);
    if (code == nullptr) {
      set_fault(caller, "VMError", "method entry code id is unknown");
      return false;
    }
    if (!method.clause_table.empty()) {
      return execute_clause_method(caller, method, *code, params, slots, self,
                                   block, caller_result_reg, return_override);
    }
    const std::uint32_t call_pc = static_cast<std::uint32_t>(caller.pc);
    ++caller.pc;
    caller.active_call_pc = call_pc;
    push_frame(*code, {}, {}, std::move(self), std::move(block),
               caller_result_reg);
    Frame &callee = frames_.back();
    callee.return_override = std::move(return_override);
    if (!materialize_defaults(callee, method, params, slots)) {
      return false;
    }
    return apply_auto_assigns(callee, method, *code);
  }

  bool resolve_class_ref(const Frame &frame, std::uint32_t path_ref,
                         std::uint32_t *out_class_index) {
    if (path_ref >= module_.const_pool.size()) {
      set_fault(frame, "VMError", "class path ref is out of range");
      return false;
    }
    const Constant &constant = module_.const_pool[path_ref];
    if (constant.kind != ConstantKind::Path) {
      set_fault(frame, "VMError", "class ref must point to path constant");
      return false;
    }
    std::vector<std::string> segments;
    if (!path_segments_from_constant(frame, constant, &segments)) {
      return false;
    }
    return find_class_by_path_segments(frame, segments, out_class_index);
  }

  const bytecode::BcMethod *
  find_method_in_owner_table(const Frame &frame, std::uint32_t owner_index,
                             const std::string &selector,
                             std::uint32_t method_flags) {
    if (owner_index >= module_.classes.size()) {
      set_fault(frame, "VMError", "class dispatch ref is out of range");
      return nullptr;
    }
    if (owner_index >= state_->classes.size()) {
      set_fault(frame, "VMError", "runtime class state is out of range");
      return nullptr;
    }
    const ClassRuntimeState &runtime_owner = state_->classes[owner_index];
    if (!runtime_owner.method_range_valid) {
      set_fault(frame, "VMError", "class method range is out of range");
      return nullptr;
    }
    const MethodTableDescriptor &method_table =
        method_flags == kMethodFlagClass ? runtime_owner.class_method_table
                                         : runtime_owner.instance_method_table;
    for (const auto &[selector_id, method] : method_table.entries) {
      if (selector_id >= module_.symbols.size()) {
        set_fault(frame, "VMError", "method selector symbol ref is invalid");
        return nullptr;
      }
      if (module_.symbols[selector_id] == selector) {
        return &method;
      }
    }
    return nullptr;
  }

  const bytecode::BcMethod *
  find_method_in_mixin_chain(const Frame &frame, std::uint32_t mixin_index,
                             const std::string &selector,
                             std::vector<bool> *seen_mixins,
                             std::vector<bool> *active_mixins) {
    if (mixin_index >= module_.classes.size()) {
      set_fault(frame, "VMError", "mixin dispatch ref is out of range");
      return nullptr;
    }
    const bytecode::BcClass &mixin = module_.classes[mixin_index];
    if ((mixin.flags & amber::bytecode::kClassFlagMixin) == 0U) {
      set_fault(frame, "TypeError", "include/extend target is not a mixin");
      return nullptr;
    }
    if ((*active_mixins)[mixin_index]) {
      set_fault(frame, "IncludeCycleError",
                "cycle detected in mixin include graph");
      return nullptr;
    }
    if ((*seen_mixins)[mixin_index]) {
      return nullptr;
    }
    (*seen_mixins)[mixin_index] = true;
    (*active_mixins)[mixin_index] = true;

    const bytecode::BcMethod *found = find_method_in_owner_table(
        frame, mixin_index, selector, kMethodFlagInstance);
    if (found != nullptr || fault_.has_value()) {
      (*active_mixins)[mixin_index] = false;
      return found;
    }

    if (mixin_index >= state_->classes.size()) {
      set_fault(frame, "VMError", "runtime mixin state is out of range");
      (*active_mixins)[mixin_index] = false;
      return nullptr;
    }
    found = find_method_in_mixin_indices(
        frame, state_->classes[mixin_index].direct_include_indices, selector,
        seen_mixins, active_mixins);
    if (found != nullptr || fault_.has_value()) {
      (*active_mixins)[mixin_index] = false;
      return found;
    }

    for (auto ref = mixin.direct_include_refs.rbegin();
         ref != mixin.direct_include_refs.rend(); ++ref) {
      std::uint32_t included_index = 0;
      if (!resolve_class_ref(frame, *ref, &included_index)) {
        (*active_mixins)[mixin_index] = false;
        return nullptr;
      }
      found = find_method_in_mixin_chain(frame, included_index, selector,
                                         seen_mixins, active_mixins);
      if (found != nullptr || fault_.has_value()) {
        (*active_mixins)[mixin_index] = false;
        return found;
      }
    }

    (*active_mixins)[mixin_index] = false;
    return nullptr;
  }

  const bytecode::BcMethod *find_method_in_mixin_refs(
      const Frame &frame, const std::vector<std::uint32_t> &refs,
      const std::string &selector, std::vector<bool> *seen_mixins,
      std::vector<bool> *active_mixins) {
    for (auto ref = refs.rbegin(); ref != refs.rend(); ++ref) {
      std::uint32_t mixin_index = 0;
      if (!resolve_class_ref(frame, *ref, &mixin_index)) {
        return nullptr;
      }
      const bytecode::BcMethod *found = find_method_in_mixin_chain(
          frame, mixin_index, selector, seen_mixins, active_mixins);
      if (found != nullptr || fault_.has_value()) {
        return found;
      }
    }
    return nullptr;
  }

  const bytecode::BcMethod *find_method_in_mixin_indices(
      const Frame &frame, const std::vector<std::uint32_t> &indices,
      const std::string &selector, std::vector<bool> *seen_mixins,
      std::vector<bool> *active_mixins) {
    for (auto index = indices.rbegin(); index != indices.rend(); ++index) {
      const bytecode::BcMethod *found = find_method_in_mixin_chain(
          frame, *index, selector, seen_mixins, active_mixins);
      if (found != nullptr || fault_.has_value()) {
        return found;
      }
    }
    return nullptr;
  }

  const bytecode::BcMethod *
  find_method_for_dispatch_impl(const Frame &frame, std::uint32_t class_index,
                                const std::string &selector, bool class_side,
                                std::vector<bool> *seen_mixins,
                                std::vector<bool> *active_mixins,
                                std::vector<bool> *active_classes) {
    if (class_index >= module_.classes.size()) {
      set_fault(frame, "VMError", "class dispatch ref is out of range");
      return nullptr;
    }
    if ((*active_classes)[class_index]) {
      set_fault(frame, "VMError", "cycle detected in superclass chain");
      return nullptr;
    }
    (*active_classes)[class_index] = true;

    const bytecode::BcMethod *found = find_method_in_owner_table(
        frame, class_index, selector,
        class_side ? kMethodFlagClass : kMethodFlagInstance);
    if (found != nullptr || fault_.has_value()) {
      (*active_classes)[class_index] = false;
      return found;
    }

    const bytecode::BcClass &klass = module_.classes[class_index];
    if (class_index >= state_->classes.size()) {
      set_fault(frame, "VMError", "runtime class state is out of range");
      (*active_classes)[class_index] = false;
      return nullptr;
    }
    const std::vector<std::uint32_t> &dynamic_mixins =
        class_side ? state_->classes[class_index].direct_extend_indices
                   : state_->classes[class_index].direct_include_indices;
    found = find_method_in_mixin_indices(frame, dynamic_mixins, selector,
                                         seen_mixins, active_mixins);
    if (found != nullptr || fault_.has_value()) {
      (*active_classes)[class_index] = false;
      return found;
    }

    found = find_method_in_mixin_refs(frame,
                                      class_side ? klass.direct_extend_refs
                                                 : klass.direct_include_refs,
                                      selector, seen_mixins, active_mixins);
    if (found != nullptr || fault_.has_value()) {
      (*active_classes)[class_index] = false;
      return found;
    }

    if (klass.has_superclass_ref) {
      std::uint32_t superclass_index = 0;
      if (!resolve_class_ref(frame, klass.superclass_ref, &superclass_index)) {
        (*active_classes)[class_index] = false;
        return nullptr;
      }
      found = find_method_for_dispatch_impl(frame, superclass_index, selector,
                                            class_side, seen_mixins,
                                            active_mixins, active_classes);
      (*active_classes)[class_index] = false;
      return found;
    }

    (*active_classes)[class_index] = false;
    return nullptr;
  }

  const bytecode::BcMethod *
  find_method_for_dispatch(const Frame &frame, std::uint32_t class_index,
                           const std::string &selector,
                           std::uint32_t expected_flags) {
    std::vector<bool> seen_mixins(module_.classes.size(), false);
    std::vector<bool> active_mixins(module_.classes.size(), false);
    std::vector<bool> active_classes(module_.classes.size(), false);
    return find_method_for_dispatch_impl(
        frame, class_index, selector, expected_flags == kMethodFlagClass,
        &seen_mixins, &active_mixins, &active_classes);
  }

  bool try_invoke_method_missing(
      Frame &frame, std::uint32_t class_index, std::uint32_t dispatch_flags,
      const std::string &selector_text,
      const std::optional<Value> &selector_value,
      const std::vector<Value> &pos_args,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      const Value &receiver, const Value &block, std::uint32_t dst) {
    if (selector_text == "method_missing") {
      return false;
    }
    const bytecode::BcMethod *method = find_method_for_dispatch(
        frame, class_index, "method_missing", dispatch_flags);
    if (fault_.has_value() || method == nullptr) {
      return false;
    }
    std::vector<Value> fallback_args;
    fallback_args.reserve(pos_args.size() + 1);
    fallback_args.push_back(
        selector_value_for_method_missing(selector_text, selector_value));
    for (const Value &arg : pos_args) {
      fallback_args.push_back(arg);
    }
    return invoke_method(frame, *method, fallback_args, kw_args, receiver,
                         block, dst);
  }

  const bytecode::HandlerEntry *find_handler_for_pc(const Frame &frame,
                                                    std::uint32_t pc) const {
    if (frame.code == nullptr) {
      return nullptr;
    }
    const bytecode::HandlerEntry *best = nullptr;
    std::uint32_t best_width = std::numeric_limits<std::uint32_t>::max();
    for (const bytecode::HandlerEntry &entry : frame.code->handler_table) {
      if (entry.protected_from <= pc && pc < entry.protected_to) {
        const std::uint32_t width = entry.protected_to - entry.protected_from;
        if (best == nullptr || width < best_width) {
          best = &entry;
          best_width = width;
        }
      }
    }
    return best;
  }

  bool find_unwind_target(std::size_t *frame_index,
                          const bytecode::HandlerEntry **handler) const {
    for (std::size_t index = frames_.size(); index > 0; --index) {
      const std::size_t candidate = index - 1U;
      const Frame &frame = frames_[candidate];
      const std::uint32_t pc =
          frame.active_call_pc.value_or(static_cast<std::uint32_t>(frame.pc));
      const bytecode::HandlerEntry *entry = find_handler_for_pc(frame, pc);
      if (entry != nullptr) {
        *frame_index = candidate;
        *handler = entry;
        return true;
      }
    }
    return false;
  }

  bool raise_value(const Frame &raising_frame, const Value &exception) {
    std::size_t target_index = 0;
    const bytecode::HandlerEntry *handler = nullptr;
    if (!find_unwind_target(&target_index, &handler)) {
      const std::string error_name = exception_error_name(exception);
      const std::string message =
          "unhandled exception " + value_to_debug_string(exception, &module_);
      fault_ = make_fault(raising_frame, error_name, message);
      return false;
    }

    const BcCode *handler_code = find_code(module_, handler->handler_code_id);
    if (handler_code == nullptr) {
      set_fault(raising_frame, "VMError", "handler code id is unknown");
      return false;
    }
    if (target_index >= frames_.size()) {
      set_fault(raising_frame, "VMError", "handler frame is out of range");
      return false;
    }

    while (frames_.size() > target_index + 1U) {
      frames_.pop_back();
    }

    Frame &target = frames_[target_index];
    if (target.code == nullptr ||
        handler->handler_pc >= target.code->instructions.size()) {
      set_fault(target, "VMError", "handler pc is out of range");
      return false;
    }

    clear_pattern_state(target);
    target.pending_pattern_bindings.clear();
    target.active_call_pc.reset();
    target.pc = handler->handler_pc;
    push_frame(*handler_code, {exception}, target.captures, target.self,
               target.block, 0U);
    return true;
  }

  bool is_pattern_opcode(Opcode opcode) const {
    switch (opcode) {
    case Opcode::PPrepSeq:
    case Opcode::PPrepMap:
    case Opcode::PCheckEq:
    case Opcode::PCheckPin:
    case Opcode::PCheckLenEq:
    case Opcode::PCheckLenGte:
    case Opcode::PGetIndex:
    case Opcode::PHasKey:
    case Opcode::PGetKey:
    case Opcode::PTripleEq:
    case Opcode::PBind:
    case Opcode::PCommit:
    case Opcode::PFail:
      return true;
    default:
      return false;
    }
  }

  SendStatus try_apply_scalar_send(const Frame &frame, const Value &receiver,
                                   const std::string &selector,
                                   const std::vector<Value> &args,
                                   const Value &block, bool has_keywords,
                                   Value *out) {
    if (!ensure_lifecycle_access(frame, receiver)) {
      return SendStatus::Faulted;
    }

    auto require_arity = [&](std::size_t expected) -> bool {
      if (args.size() != expected) {
        set_fault(frame, "TypeError", "wrong builtin SEND arity");
        return false;
      }
      return true;
    };

    auto require_integer_arg = [&](std::size_t index,
                                   std::int64_t *value) -> bool {
      if (index >= args.size() || !args[index].is_integer()) {
        set_fault(frame, "TypeError", "builtin SEND expects integer argument");
        return false;
      }
      *value = args[index].as_integer();
      return true;
    };

    auto require_no_block = [&]() -> bool {
      if (!block.is_null()) {
        set_fault(frame, "TypeError",
                  "builtin SEND selector does not accept block arguments");
        return false;
      }
      return true;
    };

    auto selector_in = [&](std::initializer_list<const char *> names) -> bool {
      for (const char *name : names) {
        if (selector == name) {
          return true;
        }
      }
      return false;
    };

    auto require_receiver_live_after_block = [&]() -> bool {
      return ensure_lifecycle_access(frame, receiver);
    };

    const bool builtin_selector =
        selector_in({"==", "==="}) ||
        ((receiver.is_list() || receiver.is_tuple()) &&
         selector_in({"empty?", "[]", "deconstruct", "first", "count", "to_a",
                      "lazy", "each", "map", "flat_map", "select", "reject",
                      "find", "group_by", "any?", "all?", "none?",
                      "reduce"})) ||
        (receiver.is_map() &&
         selector_in({"empty?", "[]", "deconstruct_keys", "keys", "values",
                      "entries", "to_a", "each", "map", "select", "reject",
                      "transform_values"})) ||
        (receiver.is_integer() &&
         selector_in({"+", "-", "*", "/", ">", "<", ">=", "<="}));
    if (has_keywords && builtin_selector) {
      set_fault(frame, "TypeError",
                "builtin SEND does not accept keyword arguments");
      return SendStatus::Faulted;
    }

    if ((selector == "==" || selector == "===") &&
        !receiver.is_instance_object() && !receiver.is_class_object()) {
      if (!require_arity(1) || !require_no_block()) {
        return SendStatus::Faulted;
      }
      if (!ensure_lifecycle_access(frame, args[0])) {
        return SendStatus::Faulted;
      }
      *out = Value::boolean(value_equals(receiver, args[0]));
      return SendStatus::Matched;
    }

    if (receiver.is_list() || receiver.is_tuple()) {
      std::vector<Value> items;
      bool source_was_tuple = false;
      const std::optional<std::vector<Value>> extracted =
          extract_sequence_items(frame, receiver, &source_was_tuple);
      if (fault_.has_value()) {
        return SendStatus::Faulted;
      }
      if (extracted.has_value()) {
        items = *extracted;
        if (selector == "empty?") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          *out = Value::boolean(items.empty());
          return SendStatus::Matched;
        }
        if (selector == "[]") {
          std::int64_t index = 0;
          if (!require_arity(1) || !require_no_block() ||
              !require_integer_arg(0, &index)) {
            return SendStatus::Faulted;
          }
          if (index < 0 || static_cast<std::size_t>(index) >= items.size()) {
            *out = Value::null();
          } else {
            *out = items[static_cast<std::size_t>(index)];
          }
          return SendStatus::Matched;
        }
        if (selector == "deconstruct") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          *out = receiver;
          return SendStatus::Matched;
        }
        if (selector == "first") {
          if (!require_no_block()) {
            return SendStatus::Faulted;
          }
          if (args.empty()) {
            *out = items.empty() ? Value::null() : items.front();
            return SendStatus::Matched;
          }
          std::int64_t count = 0;
          if (!require_arity(1) || !require_integer_arg(0, &count)) {
            return SendStatus::Faulted;
          }
          const std::size_t take =
              count <= 0 ? 0U
                         : std::min<std::size_t>(
                               static_cast<std::size_t>(count), items.size());
          *out = make_list_value(
              std::vector<Value>(items.begin(), items.begin() + take));
          return SendStatus::Matched;
        }
        if (selector == "count") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          if (block.is_null()) {
            *out = Value::integer(static_cast<std::int64_t>(items.size()));
            return SendStatus::Matched;
          }
          std::int64_t count = 0;
          for (const Value &item : items) {
            const std::optional<Value> predicate =
                call_block_to_value(frame, block, {item});
            if (!predicate.has_value()) {
              return SendStatus::Faulted;
            }
            if (!require_receiver_live_after_block()) {
              return SendStatus::Faulted;
            }
            if (is_truthy(*predicate)) {
              ++count;
            }
          }
          *out = Value::integer(count);
          return SendStatus::Matched;
        }
        if (selector == "to_a") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          *out = make_list_value(items);
          return SendStatus::Matched;
        }
        if (selector == "lazy") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          *out = receiver;
          return SendStatus::Matched;
        }
        if (selector == "each") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          for (const Value &item : items) {
            if (!call_block_to_value(frame, block, {item}).has_value()) {
              return SendStatus::Faulted;
            }
            if (!require_receiver_live_after_block()) {
              return SendStatus::Faulted;
            }
          }
          *out = receiver;
          return SendStatus::Matched;
        }
        if (selector == "map" || selector == "select" || selector == "reject" ||
            selector == "flat_map" || selector == "find" ||
            selector == "group_by") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          if (selector == "map") {
            std::vector<Value> mapped;
            mapped.reserve(items.size());
            for (const Value &item : items) {
              const std::optional<Value> value =
                  call_block_to_value(frame, block, {item});
              if (!value.has_value()) {
                return SendStatus::Faulted;
              }
              if (!require_receiver_live_after_block()) {
                return SendStatus::Faulted;
              }
              mapped.push_back(*value);
            }
            *out = make_list_value(std::move(mapped));
            return SendStatus::Matched;
          }
          if (selector == "flat_map") {
            std::vector<Value> mapped;
            for (const Value &item : items) {
              const std::optional<Value> value =
                  call_block_to_value(frame, block, {item});
              if (!value.has_value()) {
                return SendStatus::Faulted;
              }
              if (!require_receiver_live_after_block()) {
                return SendStatus::Faulted;
              }
              bool nested_was_tuple = false;
              const std::optional<std::vector<Value>> nested =
                  extract_sequence_items(frame, *value, &nested_was_tuple);
              if (fault_.has_value()) {
                return SendStatus::Faulted;
              }
              if (!nested.has_value()) {
                set_fault(frame, "TypeError",
                          "flat_map block must return sequence");
                return SendStatus::Faulted;
              }
              mapped.insert(mapped.end(), nested->begin(), nested->end());
            }
            *out = make_list_value(std::move(mapped));
            return SendStatus::Matched;
          }
          if (selector == "find") {
            for (const Value &item : items) {
              const std::optional<Value> predicate =
                  call_block_to_value(frame, block, {item});
              if (!predicate.has_value()) {
                return SendStatus::Faulted;
              }
              if (!require_receiver_live_after_block()) {
                return SendStatus::Faulted;
              }
              if (is_truthy(*predicate)) {
                *out = item;
                return SendStatus::Matched;
              }
            }
            *out = Value::null();
            return SendStatus::Matched;
          }
          if (selector == "group_by") {
            std::vector<std::pair<std::uint32_t, std::vector<Value>>> groups;
            for (const Value &item : items) {
              const std::optional<Value> key =
                  call_block_to_value(frame, block, {item});
              if (!key.has_value()) {
                return SendStatus::Faulted;
              }
              if (!require_receiver_live_after_block()) {
                return SendStatus::Faulted;
              }
              std::optional<std::uint32_t> key_symbol_id;
              if (key->is_symbol()) {
                key_symbol_id = key->as_symbol().symbol_id;
              } else if (key->is_string()) {
                const std::optional<std::string> text =
                    string_text_from_id(key->as_string().string_id);
                if (!text.has_value()) {
                  set_fault(frame, "VMError",
                            "group_by string key ref is invalid");
                  return SendStatus::Faulted;
                }
                key_symbol_id = symbol_id_for_text(*text);
              }
              if (!key_symbol_id.has_value()) {
                set_fault(frame, "TypeError",
                          "group_by block must return Symbol key");
                return SendStatus::Faulted;
              }
              auto group = std::find_if(groups.begin(), groups.end(),
                                        [&](const auto &entry) {
                                          return entry.first == *key_symbol_id;
                                        });
              if (group == groups.end()) {
                groups.push_back({*key_symbol_id, {}});
                group = groups.end() - 1;
              }
              group->second.push_back(item);
            }
            std::vector<MapEntry> entries;
            entries.reserve(groups.size());
            for (auto &group : groups) {
              entries.push_back(
                  {group.first, make_list_value(std::move(group.second))});
            }
            *out = make_symbol_map_value(std::move(entries));
            return SendStatus::Matched;
          }
          std::vector<Value> filtered;
          filtered.reserve(items.size());
          for (const Value &item : items) {
            const std::optional<Value> predicate =
                call_block_to_value(frame, block, {item});
            if (!predicate.has_value()) {
              return SendStatus::Faulted;
            }
            if (!require_receiver_live_after_block()) {
              return SendStatus::Faulted;
            }
            const bool keep = is_truthy(*predicate);
            if ((selector == "select" && keep) ||
                (selector == "reject" && !keep)) {
              filtered.push_back(item);
            }
          }
          *out = make_list_value(std::move(filtered));
          return SendStatus::Matched;
        }
        if (selector == "any?" || selector == "all?" || selector == "none?") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          bool saw_any = false;
          bool all_match = true;
          bool any_match = false;
          for (const Value &item : items) {
            saw_any = true;
            Value predicate = item;
            if (!block.is_null()) {
              const std::optional<Value> value =
                  call_block_to_value(frame, block, {item});
              if (!value.has_value()) {
                return SendStatus::Faulted;
              }
              if (!require_receiver_live_after_block()) {
                return SendStatus::Faulted;
              }
              predicate = *value;
            }
            const bool truthy = is_truthy(predicate);
            any_match = any_match || truthy;
            all_match = all_match && truthy;
          }
          if (selector == "any?") {
            *out = Value::boolean(any_match);
          } else if (selector == "all?") {
            *out = Value::boolean(!saw_any || all_match);
          } else {
            *out = Value::boolean(!any_match);
          }
          return SendStatus::Matched;
        }
        if (selector == "reduce") {
          if (args.size() > 1U) {
            set_fault(frame, "TypeError", "wrong builtin SEND arity");
            return SendStatus::Faulted;
          }
          if (items.empty() && args.empty()) {
            set_fault(frame, "EmptyCollectionError",
                      "reduce without initial value on empty sequence");
            return SendStatus::Faulted;
          }
          if (block.is_null()) {
            set_fault(frame, "TypeError", "reduce requires block");
            return SendStatus::Faulted;
          }
          Value accumulator = Value::null();
          std::size_t index = 0;
          if (args.empty()) {
            accumulator = items.front();
            index = 1;
          } else {
            accumulator = args[0];
          }
          for (; index < items.size(); ++index) {
            const std::optional<Value> value =
                call_block_to_value(frame, block, {accumulator, items[index]});
            if (!value.has_value()) {
              return SendStatus::Faulted;
            }
            if (!require_receiver_live_after_block()) {
              return SendStatus::Faulted;
            }
            accumulator = *value;
          }
          *out = accumulator;
          return SendStatus::Matched;
        }
      }
    }

    if (receiver.is_map()) {
      const std::optional<std::vector<MapEntry>> extracted =
          extract_map_entries(frame, receiver);
      if (fault_.has_value()) {
        return SendStatus::Faulted;
      }
      if (extracted.has_value()) {
        if (selector == "empty?") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          *out = Value::boolean(extracted->empty());
          return SendStatus::Matched;
        }
        if (selector == "[]") {
          if (!require_arity(1) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          std::optional<std::uint32_t> key_symbol_id;
          if (args[0].is_symbol()) {
            key_symbol_id = args[0].as_symbol().symbol_id;
          } else if (args[0].is_string()) {
            const std::optional<std::string> text =
                string_text_from_id(args[0].as_string().string_id);
            if (!text.has_value()) {
              set_fault(frame, "VMError", "map index string ref is invalid");
              return SendStatus::Faulted;
            }
            key_symbol_id = symbol_id_for_text(*text);
          } else {
            set_fault(frame, "TypeError",
                      "map index expects Symbol or String key");
            return SendStatus::Faulted;
          }
          if (!key_symbol_id.has_value()) {
            *out = Value::null();
            return SendStatus::Matched;
          }
          Value found = Value::null();
          for (const MapEntry &entry : *extracted) {
            if (entry.symbol_id == *key_symbol_id) {
              found = entry.value;
              break;
            }
          }
          *out = found;
          return SendStatus::Matched;
        }
        if (selector == "deconstruct_keys") {
          if (!require_arity(1) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          bool keyset_is_valid = false;
          if (args[0].is_tuple() || args[0].is_list()) {
            bool keyset_source_was_tuple = false;
            const std::optional<std::vector<Value>> keyset_items =
                extract_sequence_items(frame, args[0],
                                       &keyset_source_was_tuple);
            if (fault_.has_value()) {
              return SendStatus::Faulted;
            }
            keyset_is_valid = keyset_items.has_value();
            if (keyset_items.has_value()) {
              for (const Value &item : *keyset_items) {
                if (!item.is_symbol()) {
                  keyset_is_valid = false;
                  break;
                }
              }
            }
          }
          if (!keyset_is_valid) {
            set_fault(frame, "TypeError",
                      "deconstruct_keys expects Symbol tuple/list keyset");
            return SendStatus::Faulted;
          }
          *out = receiver;
          return SendStatus::Matched;
        }
        if (selector == "keys") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          std::vector<Value> keys;
          keys.reserve(extracted->size());
          for (const MapEntry &entry : *extracted) {
            keys.push_back(Value::symbol(entry.symbol_id));
          }
          *out = make_list_value(std::move(keys));
          return SendStatus::Matched;
        }
        if (selector == "values") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          std::vector<Value> values;
          values.reserve(extracted->size());
          for (const MapEntry &entry : *extracted) {
            values.push_back(entry.value);
          }
          *out = make_list_value(std::move(values));
          return SendStatus::Matched;
        }
        if (selector == "entries" || selector == "to_a") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          std::vector<Value> entries;
          entries.reserve(extracted->size());
          for (const MapEntry &entry : *extracted) {
            entries.push_back(make_tuple_value(
                {Value::symbol(entry.symbol_id), entry.value}));
          }
          *out = make_list_value(std::move(entries));
          return SendStatus::Matched;
        }
        if (selector == "each") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          for (const MapEntry &entry : *extracted) {
            if (!call_block_to_value(
                     frame, block,
                     {Value::symbol(entry.symbol_id), entry.value})
                     .has_value()) {
              return SendStatus::Faulted;
            }
            if (!require_receiver_live_after_block()) {
              return SendStatus::Faulted;
            }
          }
          *out = receiver;
          return SendStatus::Matched;
        }
        if (selector == "map") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          std::vector<Value> mapped;
          mapped.reserve(extracted->size());
          for (const MapEntry &entry : *extracted) {
            const std::optional<Value> value = call_block_to_value(
                frame, block, {Value::symbol(entry.symbol_id), entry.value});
            if (!value.has_value()) {
              return SendStatus::Faulted;
            }
            if (!require_receiver_live_after_block()) {
              return SendStatus::Faulted;
            }
            mapped.push_back(*value);
          }
          *out = make_list_value(std::move(mapped));
          return SendStatus::Matched;
        }
        if (selector == "select" || selector == "reject") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          std::vector<MapEntry> filtered;
          filtered.reserve(extracted->size());
          for (const MapEntry &entry : *extracted) {
            const std::optional<Value> predicate = call_block_to_value(
                frame, block, {Value::symbol(entry.symbol_id), entry.value});
            if (!predicate.has_value()) {
              return SendStatus::Faulted;
            }
            if (!require_receiver_live_after_block()) {
              return SendStatus::Faulted;
            }
            const bool keep = is_truthy(*predicate);
            if ((selector == "select" && keep) ||
                (selector == "reject" && !keep)) {
              filtered.push_back(entry);
            }
          }
          *out = make_symbol_map_value(std::move(filtered));
          return SendStatus::Matched;
        }
        if (selector == "transform_values") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          std::vector<MapEntry> transformed;
          transformed.reserve(extracted->size());
          for (const MapEntry &entry : *extracted) {
            const std::optional<Value> value =
                call_block_to_value(frame, block, {entry.value});
            if (!value.has_value()) {
              return SendStatus::Faulted;
            }
            if (!require_receiver_live_after_block()) {
              return SendStatus::Faulted;
            }
            transformed.push_back({entry.symbol_id, *value});
          }
          *out = make_symbol_map_value(std::move(transformed));
          return SendStatus::Matched;
        }
      }
    }

    if (receiver.is_integer()) {
      const std::int64_t lhs = receiver.as_integer();
      std::int64_t rhs = 0;
      if (selector == "+") {
        if (!require_arity(1) || !require_no_block() ||
            !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs + rhs);
        return SendStatus::Matched;
      }
      if (selector == "-") {
        if (!require_arity(1) || !require_no_block() ||
            !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs - rhs);
        return SendStatus::Matched;
      }
      if (selector == "*") {
        if (!require_arity(1) || !require_no_block() ||
            !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs * rhs);
        return SendStatus::Matched;
      }
      if (selector == "/") {
        if (!require_arity(1) || !require_no_block() ||
            !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        if (rhs == 0) {
          set_fault(frame, "TypeError", "division by zero");
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs / rhs);
        return SendStatus::Matched;
      }
      if (selector == ">") {
        if (!require_arity(1) || !require_no_block() ||
            !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs > rhs);
        return SendStatus::Matched;
      }
      if (selector == "<") {
        if (!require_arity(1) || !require_no_block() ||
            !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs < rhs);
        return SendStatus::Matched;
      }
      if (selector == ">=") {
        if (!require_arity(1) || !require_no_block() ||
            !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs >= rhs);
        return SendStatus::Matched;
      }
      if (selector == "<=") {
        if (!require_arity(1) || !require_no_block() ||
            !require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs <= rhs);
        return SendStatus::Matched;
      }
    }

    return SendStatus::NotHandled;
  }

  bool step_send(Frame &frame, const Instruction &insn, bool dynamic_selector) {
    std::uint32_t dst = 0;
    std::uint32_t recv_reg = 0;
    if (!operand_u32(frame, insn, 0, &dst) ||
        !operand_u32(frame, insn, 1, &recv_reg)) {
      return false;
    }

    std::size_t operand_index = 2;
    std::optional<std::string> selector;
    std::optional<Value> selector_value;
    std::optional<std::uint32_t> selector_symbol_id_for_cache;
    if (dynamic_selector) {
      std::uint32_t selector_reg = 0;
      if (!operand_u32(frame, insn, operand_index++, &selector_reg)) {
        return false;
      }
      selector_value = read_reg(frame, selector_reg);
      selector = selector_text_from_value(*selector_value);
      if (fault_.has_value()) {
        return false;
      }
      if (!selector.has_value()) {
        set_fault(frame, "TypeError",
                  "SEND_DYN expects Symbol or String selector");
        return false;
      }
      if (selector_value->is_symbol()) {
        selector_symbol_id_for_cache = selector_value->as_symbol().symbol_id;
      } else {
        selector_symbol_id_for_cache = symbol_id_for_text(*selector);
      }
    } else {
      std::uint32_t selector_id = 0;
      if (!operand_u32(frame, insn, operand_index++, &selector_id)) {
        return false;
      }
      selector_value = Value::symbol(selector_id);
      selector_symbol_id_for_cache = selector_id;
      selector = selector_text_from_symbol(selector_id);
      if (!selector.has_value()) {
        set_fault(frame, "VMError", "selector symbol ref is out of range");
        return false;
      }
    }

    std::uint32_t pos_count = 0;
    if (!operand_u32(frame, insn, operand_index++, &pos_count)) {
      return false;
    }
    std::vector<Value> args;
    args.reserve(pos_count);
    for (std::uint32_t i = 0; i < pos_count; ++i) {
      std::uint32_t reg = 0;
      if (!operand_u32(frame, insn, operand_index++, &reg)) {
        return false;
      }
      args.push_back(read_reg(frame, reg));
      if (fault_.has_value()) {
        return false;
      }
    }

    std::uint32_t kw_count = 0;
    if (!operand_u32(frame, insn, operand_index++, &kw_count)) {
      return false;
    }
    std::vector<std::pair<std::uint32_t, Value>> kw_args;
    kw_args.reserve(kw_count);
    for (std::uint32_t i = 0; i < kw_count; ++i) {
      std::uint32_t name_symbol_id = 0;
      std::uint32_t reg = 0;
      if (!operand_u32(frame, insn, operand_index++, &name_symbol_id) ||
          !operand_u32(frame, insn, operand_index++, &reg)) {
        return false;
      }
      kw_args.push_back({name_symbol_id, read_reg(frame, reg)});
      if (fault_.has_value()) {
        return false;
      }
    }

    std::int64_t block_reg = -1;
    if (!operand_i64(frame, insn, operand_index++, &block_reg)) {
      return false;
    }
    const std::optional<std::uint32_t> site_id =
        optional_operand_u32(frame, insn, operand_index++);
    if (fault_.has_value()) {
      return false;
    }
    const Value block =
        has_optional_reg(block_reg)
            ? read_reg(frame, static_cast<std::uint32_t>(block_reg))
            : Value::null();
    if (fault_.has_value()) {
      return false;
    }

    const Value receiver = read_reg(frame, recv_reg);
    if (fault_.has_value()) {
      return false;
    }
    if (*selector == "destroy!") {
      if (!args.empty() || !kw_args.empty() || !block.is_null()) {
        set_fault(frame, "TypeError", "destroy! accepts no arguments");
        return false;
      }
      bool changed = false;
      if (!lifecycle_destroy(frame, receiver, &changed) || fault_.has_value()) {
        return false;
      }
      if (!write_reg(frame, dst, Value::boolean(changed))) {
        return false;
      }
      ++frame.pc;
      return true;
    }
    Value result = Value::null();
    const SendStatus scalar_status = try_apply_scalar_send(
        frame, receiver, *selector, args, block, !kw_args.empty(), &result);
    if (scalar_status == SendStatus::Faulted) {
      return false;
    }
    if (scalar_status == SendStatus::Matched) {
      if (!kw_args.empty()) {
        set_fault(frame, "TypeError",
                  "builtin SEND does not accept keyword arguments");
        return false;
      }
      if (!write_reg(frame, dst, std::move(result))) {
        return false;
      }
      ++frame.pc;
      return true;
    }

    std::uint32_t class_index = 0;
    std::uint32_t dispatch_flags = 0;
    if (receiver.is_instance_object()) {
      const std::shared_ptr<InstanceValue> instance =
          receiver.as_instance_object();
      if (instance == nullptr) {
        set_fault(frame, "TypeError", "instance receiver is null");
        return false;
      }
      class_index = instance->class_index;
      dispatch_flags = kMethodFlagInstance;
    } else if (receiver.is_class_object()) {
      class_index = receiver.as_class_object().class_index;
      dispatch_flags = kMethodFlagClass;
    } else {
      set_fault(frame, "NoMethodError",
                "selector is not implemented in current runtime baseline");
      return false;
    }

    if (site_id.has_value() && selector_symbol_id_for_cache.has_value()) {
      const bytecode::BcMethod *cached = probe_call_cache(
          frame, *site_id, class_index, dispatch_flags,
          *selector_symbol_id_for_cache,
          static_cast<std::uint32_t>(args.size()), kw_args, block);
      if (cached != nullptr) {
        return invoke_method(frame, *cached, args, kw_args, receiver, block,
                             dst);
      }
    }

    const bytecode::BcMethod *method =
        find_method_for_dispatch(frame, class_index, *selector, dispatch_flags);
    if (fault_.has_value()) {
      return false;
    }
    if (method == nullptr) {
      if (try_invoke_method_missing(frame, class_index, dispatch_flags,
                                    *selector, selector_value, args, kw_args,
                                    receiver, block, dst)) {
        return true;
      }
      if (fault_.has_value()) {
        return false;
      }
      set_fault(frame, "NoMethodError",
                "selector is not implemented in current runtime baseline");
      return false;
    }
    if (site_id.has_value() && selector_symbol_id_for_cache.has_value()) {
      update_call_cache(frame, *site_id, class_index, dispatch_flags,
                        *selector_symbol_id_for_cache,
                        static_cast<std::uint32_t>(args.size()), kw_args, block,
                        *method);
    }
    return invoke_method(frame, *method, args, kw_args, receiver, block, dst);
  }

  void step() {
    Frame &frame = frames_.back();
    if (frame.code == nullptr || frame.pc >= frame.code->instructions.size()) {
      set_fault(frame, "VMError", "program counter out of range");
      return;
    }

    const Instruction &insn = frame.code->instructions[frame.pc];
    if (!is_pattern_opcode(insn.opcode) && has_active_pattern_state(frame)) {
      if (!finalize_pattern_success(frame, false)) {
        return;
      }
      if (frame.pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "program counter out of range");
        return;
      }
    }
    switch (insn.opcode) {
    case Opcode::LoadK: {
      std::uint32_t dst = 0;
      std::uint32_t const_id = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &const_id)) {
        return;
      }
      if (!write_reg(frame, dst, load_constant(frame, const_id))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadNull: {
      std::uint32_t dst = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !write_reg(frame, dst, Value::null())) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadBool: {
      std::uint32_t dst = 0;
      std::int64_t raw = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_i64(frame, insn, 1, &raw) ||
          !write_reg(frame, dst, Value::boolean(raw != 0))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::Move: {
      std::uint32_t dst = 0;
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &src)) {
        return;
      }
      if (!write_reg(frame, dst, read_reg(frame, src))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::MakeList:
    case Opcode::MakeTuple: {
      std::uint32_t dst = 0;
      std::uint32_t first_reg = 0;
      std::uint32_t count = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &first_reg) ||
          !operand_u32(frame, insn, 2, &count)) {
        return;
      }
      std::vector<Value> items;
      items.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        items.push_back(read_reg(frame, first_reg + i));
        if (fault_.has_value()) {
          return;
        }
      }
      const Value value = insn.opcode == Opcode::MakeTuple
                              ? make_tuple_value(std::move(items))
                              : make_list_value(std::move(items));
      if (!write_reg(frame, dst, value)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::MakeMap: {
      std::uint32_t dst = 0;
      std::uint32_t count = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &count)) {
        return;
      }
      std::size_t operand_index = 2;
      std::vector<MapEntry> entries;
      entries.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t symbol_id = 0;
        std::uint32_t reg = 0;
        if (!operand_u32(frame, insn, operand_index++, &symbol_id) ||
            !operand_u32(frame, insn, operand_index++, &reg)) {
          return;
        }
        entries.push_back({symbol_id, read_reg(frame, reg)});
        if (fault_.has_value()) {
          return;
        }
      }
      if (!write_reg(frame, dst, make_symbol_map_value(std::move(entries)))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::Freeze: {
      std::uint32_t dst = 0;
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &src)) {
        return;
      }
      const Value value = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      if (value.is_list()) {
        const std::shared_ptr<ListValue> list = value.as_list();
        if (list == nullptr) {
          set_fault(frame, "TypeError", "list value is null");
          return;
        }
        if (!ensure_lifecycle_access(frame, value)) {
          return;
        }
        if (!write_reg(frame, dst, make_list_value(list->items, true))) {
          return;
        }
      } else if (value.is_map()) {
        const std::shared_ptr<MapValue> map = value.as_map();
        if (map == nullptr) {
          set_fault(frame, "TypeError", "map value is null");
          return;
        }
        if (!ensure_lifecycle_access(frame, value)) {
          return;
        }
        if (!write_reg(frame, dst, make_symbol_map_value(map->entries, true))) {
          return;
        }
      } else {
        if (!write_reg(frame, dst, value)) {
          return;
        }
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadSelf: {
      std::uint32_t dst = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !write_reg(frame, dst, frame.self)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadIvar: {
      std::uint32_t dst = 0;
      std::uint32_t recv_reg = 0;
      std::uint32_t symbol_id = 0;
      std::uint32_t site_id = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &recv_reg) ||
          !operand_u32(frame, insn, 2, &symbol_id) ||
          !operand_u32(frame, insn, 3, &site_id)) {
        return;
      }
      const Value receiver = read_reg(frame, recv_reg);
      if (fault_.has_value()) {
        return;
      }
      std::shared_ptr<InstanceValue> instance;
      if (!expect_instance_receiver(frame, receiver, &instance)) {
        return;
      }
      std::optional<std::uint32_t> slot =
          probe_ivar_cache(frame, site_id, *instance, symbol_id);
      if (!slot.has_value()) {
        const std::optional<std::string> ivar_name =
            selector_text_from_symbol(symbol_id);
        if (!ivar_name.has_value()) {
          set_fault(frame, "VMError", "ivar symbol ref is out of range");
          return;
        }
        slot = ensure_instance_ivar_slot(frame, instance, *ivar_name);
        if (fault_.has_value()) {
          return;
        }
        if (slot.has_value()) {
          update_ivar_cache(frame, site_id, *instance, symbol_id, *slot);
        }
      }
      const Value value =
          slot.has_value() && *slot < instance->ivar_storage.size()
              ? instance->ivar_storage[*slot]
              : Value::null();
      if (!write_reg(frame, dst, value)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadCvar: {
      std::uint32_t dst = 0;
      std::uint32_t owner_reg = 0;
      std::uint32_t symbol_id = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &owner_reg) ||
          !operand_u32(frame, insn, 2, &symbol_id)) {
        return;
      }
      const Value owner = read_reg(frame, owner_reg);
      if (fault_.has_value()) {
        return;
      }
      std::uint32_t class_index = 0;
      if (!expect_class_owner(frame, owner, &class_index)) {
        return;
      }
      const std::optional<std::string> cvar_name =
          selector_text_from_symbol(symbol_id);
      if (!cvar_name.has_value()) {
        set_fault(frame, "VMError", "cvar symbol ref is out of range");
        return;
      }
      const auto cvar = state_->classes[class_index].cvars.find(*cvar_name);
      const Value value = cvar == state_->classes[class_index].cvars.end()
                              ? Value::null()
                              : cvar->second;
      if (!write_reg(frame, dst, value)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::StoreIvar: {
      std::uint32_t recv_reg = 0;
      std::uint32_t symbol_id = 0;
      std::uint32_t src = 0;
      std::uint32_t site_id = 0;
      if (!operand_u32(frame, insn, 0, &recv_reg) ||
          !operand_u32(frame, insn, 1, &symbol_id) ||
          !operand_u32(frame, insn, 2, &src) ||
          !operand_u32(frame, insn, 3, &site_id)) {
        return;
      }
      const Value receiver = read_reg(frame, recv_reg);
      if (fault_.has_value()) {
        return;
      }
      std::shared_ptr<InstanceValue> instance;
      if (!expect_instance_receiver(frame, receiver, &instance)) {
        return;
      }
      std::optional<std::uint32_t> cached_slot =
          probe_ivar_cache(frame, site_id, *instance, symbol_id);
      (void)cached_slot;
      const std::optional<std::string> ivar_name =
          selector_text_from_symbol(symbol_id);
      if (!ivar_name.has_value()) {
        set_fault(frame, "VMError", "ivar symbol ref is out of range");
        return;
      }
      const Value value = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      if (!store_instance_ivar(frame, instance, *ivar_name, value, symbol_id,
                               site_id)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::StoreCvar: {
      std::uint32_t owner_reg = 0;
      std::uint32_t symbol_id = 0;
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &owner_reg) ||
          !operand_u32(frame, insn, 1, &symbol_id) ||
          !operand_u32(frame, insn, 2, &src)) {
        return;
      }
      const Value owner = read_reg(frame, owner_reg);
      if (fault_.has_value()) {
        return;
      }
      std::uint32_t class_index = 0;
      if (!expect_class_owner(frame, owner, &class_index)) {
        return;
      }
      const std::optional<std::string> cvar_name =
          selector_text_from_symbol(symbol_id);
      if (!cvar_name.has_value()) {
        set_fault(frame, "VMError", "cvar symbol ref is out of range");
        return;
      }
      state_->classes[class_index].cvars[*cvar_name] = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LookupConst: {
      std::uint32_t dst = 0;
      std::uint32_t const_id = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &const_id)) {
        return;
      }
      if (!write_reg(frame, dst, lookup_constant(frame, const_id))) {
        return;
      }
      if (fault_.has_value()) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::GetLast: {
      std::uint32_t dst = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !write_reg(frame, dst, frame.last_result)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::SetLast: {
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &src)) {
        return;
      }
      frame.last_result = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::LoadUpval: {
      std::uint32_t dst = 0;
      std::uint32_t slot = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &slot)) {
        return;
      }
      if (slot >= frame.captures.size()) {
        set_fault(frame, "VMError", "capture slot out of range");
        return;
      }
      if (!write_reg(frame, dst, frame.captures[slot])) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::StoreUpval: {
      std::uint32_t slot = 0;
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &slot) ||
          !operand_u32(frame, insn, 1, &src)) {
        return;
      }
      if (slot >= frame.captures.size()) {
        set_fault(frame, "VMError", "capture slot out of range");
        return;
      }
      frame.captures[slot] = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::MakeClosure: {
      std::uint32_t dst = 0;
      std::uint32_t code_id = 0;
      std::uint32_t capture_count = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &code_id) ||
          !operand_u32(frame, insn, 2, &capture_count)) {
        return;
      }
      std::size_t operand_index = 3;
      auto closure = make_closure_value();
      closure->code_id = code_id;
      closure->self = frame.self;
      closure->captures.reserve(capture_count);
      const Value closure_value = Value::closure(closure);
      for (std::uint32_t i = 0; i < capture_count; ++i) {
        std::uint32_t kind = 0;
        std::uint32_t slot = 0;
        if (!operand_u32(frame, insn, operand_index++, &kind) ||
            !operand_u32(frame, insn, operand_index++, &slot)) {
          return;
        }
        if (kind == 0U) {
          closure->captures.push_back(slot == dst ? closure_value
                                                  : read_reg(frame, slot));
        } else {
          if (slot >= frame.captures.size()) {
            set_fault(frame, "VMError", "capture slot out of range");
            return;
          }
          closure->captures.push_back(frame.captures[slot]);
        }
        if (fault_.has_value()) {
          return;
        }
      }
      if (!write_reg(frame, dst, closure_value)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::ObjDestroy:
    case Opcode::ObjDealloc: {
      std::uint32_t dst = 0;
      std::uint32_t obj_reg = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &obj_reg)) {
        return;
      }
      const Value object = read_reg(frame, obj_reg);
      if (fault_.has_value()) {
        return;
      }
      bool changed = false;
      const bool ok = insn.opcode == Opcode::ObjDestroy
                          ? lifecycle_destroy(frame, object, &changed)
                          : lifecycle_dealloc(frame, object, &changed);
      if (!ok || fault_.has_value()) {
        return;
      }
      if (!write_reg(frame, dst, Value::boolean(changed))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::Call: {
      CallPacket packet;
      if (!read_call_packet(frame, insn, &packet)) {
        return;
      }

      if (packet.callee.is_closure()) {
        if (!packet.kw_args.empty()) {
          set_fault(frame, "TypeError",
                    "closure CALL does not accept keyword arguments");
          return;
        }
        if (!ensure_lifecycle_access(frame, packet.callee)) {
          return;
        }
        const std::shared_ptr<ClosureValue> closure =
            packet.callee.as_closure();
        if (closure == nullptr) {
          set_fault(frame, "TypeError", "closure value is null");
          return;
        }
        const BcCode *code = find_code(module_, closure->code_id);
        if (code == nullptr) {
          set_fault(frame, "VMError", "closure code id is unknown");
          return;
        }
        const std::uint32_t call_pc = static_cast<std::uint32_t>(frame.pc);
        ++frame.pc;
        frame.active_call_pc = call_pc;
        push_frame(*code, packet.pos_args, closure->captures, closure->self,
                   packet.block, packet.dst);
        return;
      }

      if (packet.callee.is_class_object()) {
        const std::uint32_t class_index =
            packet.callee.as_class_object().class_index;
        auto instance = make_instance_value(class_index);
        if (!ensure_instance_layout(frame, instance)) {
          return;
        }
        const Value instance_value = Value::instance(instance);
        const bytecode::BcMethod *init = find_method_for_dispatch(
            frame, class_index, "init", kMethodFlagInstance);
        if (fault_.has_value()) {
          return;
        }
        if (init == nullptr) {
          if (!packet.pos_args.empty()) {
            set_fault(
                frame, "TypeError",
                "class call without init accepts no positional arguments");
            return;
          }
          if (!packet.kw_args.empty()) {
            set_fault(
                frame, "TypeError",
                "class call without init does not accept keyword arguments");
            return;
          }
          if (!packet.block.is_null()) {
            set_fault(frame, "TypeError",
                      "class call without init does not accept block");
            return;
          }
          if (!write_reg(frame, packet.dst, instance_value)) {
            return;
          }
          ++frame.pc;
          return;
        }
        if (!invoke_method(frame, *init, packet.pos_args, packet.kw_args,
                           instance_value, packet.block, packet.dst,
                           instance_value)) {
          return;
        }
        return;
      }

      if (packet.callee.is_instance_object()) {
        const std::shared_ptr<InstanceValue> instance =
            packet.callee.as_instance_object();
        if (instance == nullptr) {
          set_fault(frame, "TypeError", "instance callee is null");
          return;
        }
        const std::optional<std::uint32_t> call_symbol =
            symbol_id_for_text("call");
        const bytecode::BcMethod *method = find_method_for_dispatch(
            frame, instance->class_index, "call", kMethodFlagInstance);
        if (fault_.has_value()) {
          return;
        }
        if (method == nullptr) {
          set_fault(frame, "TypeError",
                    "CALL expects closure, class, or object with call method");
          return;
        }
        if (packet.site_id.has_value() && call_symbol.has_value()) {
          const bytecode::BcMethod *cached = probe_call_cache(
              frame, *packet.site_id, instance->class_index,
              kMethodFlagInstance, *call_symbol,
              static_cast<std::uint32_t>(packet.pos_args.size()),
              packet.kw_args, packet.block);
          if (cached != nullptr) {
            if (!invoke_method(frame, *cached, packet.pos_args, packet.kw_args,
                               packet.callee, packet.block, packet.dst)) {
              return;
            }
            return;
          }
          update_call_cache(frame, *packet.site_id, instance->class_index,
                            kMethodFlagInstance, *call_symbol,
                            static_cast<std::uint32_t>(packet.pos_args.size()),
                            packet.kw_args, packet.block, *method);
        }
        if (!invoke_method(frame, *method, packet.pos_args, packet.kw_args,
                           packet.callee, packet.block, packet.dst)) {
          return;
        }
        return;
      }

      set_fault(frame, "TypeError",
                "CALL expects closure, class, or object with call method");
      return;
    }
    case Opcode::Send:
      step_send(frame, insn, false);
      return;
    case Opcode::SendDyn:
      step_send(frame, insn, true);
      return;
    case Opcode::Jump: {
      std::uint32_t target = 0;
      if (!operand_u32(frame, insn, 0, &target)) {
        return;
      }
      if (target >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "jump target out of range");
        return;
      }
      frame.pc = target;
      return;
    }
    case Opcode::JumpIfTrue:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull: {
      std::uint32_t cond_reg = 0;
      std::uint32_t target = 0;
      if (!operand_u32(frame, insn, 0, &cond_reg) ||
          !operand_u32(frame, insn, 1, &target)) {
        return;
      }
      if (target >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "jump target out of range");
        return;
      }
      const Value cond = read_reg(frame, cond_reg);
      if (fault_.has_value()) {
        return;
      }
      const bool take =
          insn.opcode == Opcode::JumpIfTrue
              ? is_truthy(cond)
              : (insn.opcode == Opcode::JumpIfFalse ? !is_truthy(cond)
                                                    : cond.is_null());
      frame.pc = take ? target : frame.pc + 1U;
      return;
    }
    case Opcode::PPrepSeq: {
      std::uint32_t dst = 0;
      std::uint32_t src_reg = 0;
      std::uint32_t ignored_mode = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &src_reg) ||
          !operand_u32(frame, insn, 2, &ignored_mode) ||
          !operand_u32(frame, insn, 3, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const Value source = read_reg(frame, src_reg);
      if (fault_.has_value()) {
        return;
      }
      const std::optional<CoercedSeqState> sequence =
          coerce_sequence_for_pattern(frame, source);
      if (fault_.has_value()) {
        return;
      }
      if (!sequence.has_value()) {
        clear_pattern_state(frame);
        frame.pc = fail_pc;
        return;
      }
      if (!write_reg(frame, dst, sequence->value)) {
        return;
      }
      frame.prepared_seq_regs[dst] =
          PreparedSeqState{sequence->items, 0U, sequence->source_was_tuple};
      ++frame.pc;
      (void)ignored_mode;
      return;
    }
    case Opcode::PPrepMap: {
      std::uint32_t dst = 0;
      std::uint32_t src_reg = 0;
      std::uint32_t keyset_id = 0;
      std::uint32_t needs_full = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &src_reg) ||
          !operand_u32(frame, insn, 2, &keyset_id) ||
          !operand_u32(frame, insn, 3, &needs_full) ||
          !operand_u32(frame, insn, 4, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      if (keyset_id >= module_.const_pool.size()) {
        set_fault(frame, "VMError", "keyset ref is out of range");
        return;
      }
      const Constant &keyset = module_.const_pool[keyset_id];
      if (keyset.kind != ConstantKind::KeySet) {
        set_fault(frame, "VMError", "P_PREP_MAP expects keyset constant");
        return;
      }
      const Value source = read_reg(frame, src_reg);
      if (fault_.has_value()) {
        return;
      }
      const std::optional<CoercedMapState> map =
          coerce_map_for_pattern(frame, source, keyset, needs_full != 0U);
      if (fault_.has_value()) {
        return;
      }
      if (!map.has_value()) {
        clear_pattern_state(frame);
        frame.pc = fail_pc;
        return;
      }
      PreparedMapState state;
      state.entries = map->entries;
      state.requested_keys = keyset.items;
      state.needs_full = needs_full != 0U;
      state.fail_pc = fail_pc;
      for (std::size_t i = 0; i < state.entries.size(); ++i) {
        state.index_by_key[state.entries[i].symbol_id] = i;
      }
      if (!write_reg(frame, dst, map->value)) {
        return;
      }
      frame.prepared_map_regs[dst] = std::move(state);
      ++frame.pc;
      return;
    }
    case Opcode::PCheckEq: {
      std::uint32_t value_reg = 0;
      std::uint32_t const_id = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &value_reg) ||
          !operand_u32(frame, insn, 1, &const_id) ||
          !operand_u32(frame, insn, 2, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const Value actual = read_reg(frame, value_reg);
      const Value expected = load_constant(frame, const_id);
      if (fault_.has_value()) {
        return;
      }
      if (!ensure_lifecycle_access(frame, actual) ||
          !ensure_lifecycle_access(frame, expected)) {
        return;
      }
      frame.pc = value_equals(actual, expected) ? frame.pc + 1U : fail_pc;
      if (frame.pc == fail_pc) {
        clear_pattern_state(frame);
      }
      return;
    }
    case Opcode::PCheckPin: {
      std::uint32_t value_reg = 0;
      std::uint32_t slot = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &value_reg) ||
          !operand_u32(frame, insn, 1, &slot) ||
          !operand_u32(frame, insn, 2, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const Value actual = read_reg(frame, value_reg);
      const Value expected = read_reg(frame, slot);
      if (fault_.has_value()) {
        return;
      }
      if (!ensure_lifecycle_access(frame, actual) ||
          !ensure_lifecycle_access(frame, expected)) {
        return;
      }
      frame.pc = value_equals(actual, expected) ? frame.pc + 1U : fail_pc;
      if (frame.pc == fail_pc) {
        clear_pattern_state(frame);
      }
      return;
    }
    case Opcode::PCheckLenEq:
    case Opcode::PCheckLenGte: {
      std::uint32_t seq_reg = 0;
      std::uint32_t expected = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &seq_reg) ||
          !operand_u32(frame, insn, 1, &expected) ||
          !operand_u32(frame, insn, 2, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const auto state = frame.prepared_seq_regs.find(seq_reg);
      if (state == frame.prepared_seq_regs.end()) {
        set_fault(frame, "VMError", "P_CHECK_LEN expects prepared sequence");
        return;
      }
      const std::size_t size = state->second.items.size();
      const bool ok = insn.opcode == Opcode::PCheckLenEq ? size == expected
                                                         : size >= expected;
      if (!ok) {
        clear_pattern_state(frame);
        frame.pc = fail_pc;
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::PGetIndex: {
      std::uint32_t dst = 0;
      std::uint32_t seq_reg = 0;
      std::uint32_t index = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &seq_reg) ||
          !operand_u32(frame, insn, 2, &index)) {
        return;
      }
      const auto state = frame.prepared_seq_regs.find(seq_reg);
      if (state == frame.prepared_seq_regs.end()) {
        set_fault(frame, "VMError", "P_GET_INDEX expects prepared sequence");
        return;
      }
      if (index >= state->second.items.size()) {
        set_fault(frame, "VMError", "P_GET_INDEX is out of range");
        return;
      }
      state->second.rest_start = std::max(state->second.rest_start,
                                          static_cast<std::size_t>(index) + 1U);
      if (!write_reg(frame, dst, state->second.items[index])) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::PHasKey: {
      std::uint32_t map_reg = 0;
      std::uint32_t key_id = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &map_reg) ||
          !operand_u32(frame, insn, 1, &key_id) ||
          !operand_u32(frame, insn, 2, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const auto state = frame.prepared_map_regs.find(map_reg);
      if (state == frame.prepared_map_regs.end()) {
        set_fault(frame, "VMError", "P_HAS_KEY expects prepared map");
        return;
      }
      if (state->second.index_by_key.find(key_id) ==
          state->second.index_by_key.end()) {
        clear_pattern_state(frame);
        frame.pc = fail_pc;
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::PGetKey: {
      std::uint32_t dst = 0;
      std::uint32_t map_reg = 0;
      std::uint32_t key_id = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &map_reg) ||
          !operand_u32(frame, insn, 2, &key_id)) {
        return;
      }
      const auto state = frame.prepared_map_regs.find(map_reg);
      if (state == frame.prepared_map_regs.end()) {
        set_fault(frame, "VMError", "P_GET_KEY expects prepared map");
        return;
      }
      const auto index = state->second.index_by_key.find(key_id);
      if (index == state->second.index_by_key.end()) {
        set_fault(frame, "VMError", "P_GET_KEY missing requested key");
        return;
      }
      if (!write_reg(frame, dst, state->second.entries[index->second].value)) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::PTripleEq: {
      std::uint32_t matcher_reg = 0;
      std::uint32_t value_reg = 0;
      std::uint32_t fail_pc = 0;
      if (!operand_u32(frame, insn, 0, &matcher_reg) ||
          !operand_u32(frame, insn, 1, &value_reg) ||
          !operand_u32(frame, insn, 2, &fail_pc)) {
        return;
      }
      if (fail_pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "pattern fail target out of range");
        return;
      }
      const Value matcher = read_reg(frame, matcher_reg);
      const Value value = read_reg(frame, value_reg);
      if (fault_.has_value()) {
        return;
      }
      bool matched = false;
      if (!pattern_triple_eq(frame, matcher, value, &matched)) {
        return;
      }
      if (fault_.has_value()) {
        return;
      }
      frame.pc = matched ? frame.pc + 1U : fail_pc;
      if (frame.pc == fail_pc) {
        clear_pattern_state(frame);
      }
      return;
    }
    case Opcode::PBind: {
      std::uint32_t slot = 0;
      std::uint32_t value_reg = 0;
      if (!operand_u32(frame, insn, 0, &slot) ||
          !operand_u32(frame, insn, 1, &value_reg)) {
        return;
      }
      Value bound = Value::null();
      const auto seq_state = frame.prepared_seq_regs.find(value_reg);
      if (seq_state != frame.prepared_seq_regs.end()) {
        bound = materialize_sequence_rest(seq_state->second);
      } else {
        const auto map_state = frame.prepared_map_regs.find(value_reg);
        if (map_state != frame.prepared_map_regs.end()) {
          map_state->second.rest_bound = true;
          bound = materialize_map_rest(map_state->second);
        } else {
          bound = read_reg(frame, value_reg);
          if (fault_.has_value()) {
            return;
          }
        }
      }
      frame.pending_pattern_bindings[slot] = std::move(bound);
      ++frame.pc;
      return;
    }
    case Opcode::PCommit: {
      std::uint32_t base_slot = 0;
      std::uint32_t count = 0;
      if (!operand_u32(frame, insn, 0, &base_slot) ||
          !operand_u32(frame, insn, 1, &count)) {
        return;
      }
      if (!finalize_pattern_success(frame, true)) {
        return;
      }
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t slot = base_slot + i;
        const auto pending = frame.pending_pattern_bindings.find(slot);
        if (pending == frame.pending_pattern_bindings.end()) {
          set_fault(frame, "VMError", "P_COMMIT slot is missing pending bind");
          clear_pattern_state(frame);
          return;
        }
        if (!write_reg(frame, slot, pending->second)) {
          clear_pattern_state(frame);
          return;
        }
      }
      frame.pending_pattern_bindings.clear();
      clear_pattern_state(frame);
      ++frame.pc;
      return;
    }
    case Opcode::PFail: {
      std::int64_t mode = 0;
      if (!operand_i64(frame, insn, 0, &mode)) {
        return;
      }
      clear_pattern_state(frame);
      if (mode == kPatternFailModeSoft) {
        ++frame.pc;
        return;
      }
      if (mode == kPatternFailModeMatchError) {
        set_fault(frame, "MatchError", "pattern match failed");
        return;
      }
      set_fault(frame, "VMError", "unknown pattern fail mode");
      return;
    }
    case Opcode::Return: {
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &src)) {
        return;
      }
      Value value = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      if (frame.return_override.has_value()) {
        value = *frame.return_override;
      }
      std::vector<Value> completed_regs = frame.regs;
      std::vector<std::uint8_t> completed_initialized = frame.initialized;
      const std::optional<std::uint32_t> caller_reg = frame.caller_result_reg;
      frames_.pop_back();
      last_completed_regs_ = std::move(completed_regs);
      last_completed_initialized_ = std::move(completed_initialized);
      if (frames_.empty()) {
        final_value_ = value;
        return;
      }
      Frame &caller = frames_.back();
      caller.active_call_pc.reset();
      if (!caller_reg.has_value() || !write_reg(caller, *caller_reg, value)) {
        return;
      }
      return;
    }
    case Opcode::Raise: {
      std::uint32_t src = 0;
      if (!operand_u32(frame, insn, 0, &src)) {
        return;
      }
      const Value exception = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      raise_value(frame, exception);
      return;
    }
    case Opcode::CloseUpvalues:
      ++frame.pc;
      return;
    case Opcode::Safepoint:
      run_safepoint();
      ++frame.pc;
      return;
    default:
      set_fault(frame, "VMError", "opcode is not implemented in W5.1 runtime");
      return;
    }
  }

  const BcModule &module_;
  std::shared_ptr<RuntimeState> state_;
  std::string module_id_;
  std::vector<Frame> frames_;
  std::optional<Fault> fault_;
  std::vector<Value> last_completed_regs_;
  std::vector<std::uint8_t> last_completed_initialized_;
  Value final_value_ = Value::null();
};

} // namespace

namespace {

struct RuntimePackageImage {
  pkg::PackageManifest manifest;
  std::map<std::string, bytecode::BcModule> modules;
};

struct RuntimePackageImageDecode {
  bool ok = false;
  RuntimePackageImage image;
  std::vector<RuntimePackageReloadDiagnostic> diagnostics;
};

RuntimePackageReloadDiagnostic
runtime_reload_diagnostic(std::string error_name, std::string message,
                          std::string module_name = {}) {
  RuntimePackageReloadDiagnostic diagnostic;
  diagnostic.error_name = std::move(error_name);
  diagnostic.message = std::move(message);
  diagnostic.module_name = std::move(module_name);
  return diagnostic;
}

RuntimePackageImageDecode
decode_runtime_package_image(const pkg::PackageArtifact &artifact) {
  RuntimePackageImageDecode decoded;
  decoded.image.manifest = artifact.manifest;

  for (const pkg::PackageModuleBlob &blob : artifact.modules) {
    if (blob.name.empty()) {
      decoded.diagnostics.push_back(runtime_reload_diagnostic(
          "PackageReloadError", "package module name is empty"));
      continue;
    }
    bytecode::DecodeResult module = bytecode::deserialize_module(blob.bytes);
    if (!module.ok()) {
      decoded.diagnostics.push_back(runtime_reload_diagnostic(
          "BytecodeVerificationError",
          bytecode::verify_errors_to_json(module.errors), blob.name));
      continue;
    }
    if (!decoded.image.modules.emplace(blob.name, std::move(module.module))
             .second) {
      decoded.diagnostics.push_back(runtime_reload_diagnostic(
          "PackageReloadError", "duplicate package module: " + blob.name,
          blob.name));
    }
  }

  if (!artifact.manifest.root_module.empty() &&
      decoded.image.modules.find(artifact.manifest.root_module) ==
          decoded.image.modules.end()) {
    decoded.diagnostics.push_back(runtime_reload_diagnostic(
        "PackageReloadError",
        "package root module is missing: " + artifact.manifest.root_module,
        artifact.manifest.root_module));
  }

  decoded.ok = decoded.diagnostics.empty();
  return decoded;
}

bytecode::BcModule
root_module_or_empty_for_package(const pkg::PackageArtifact &artifact,
                                 std::optional<RuntimePackageImage> *image) {
  RuntimePackageImageDecode decoded = decode_runtime_package_image(artifact);
  if (!decoded.ok) {
    if (image != nullptr) {
      image->reset();
    }
    return {};
  }
  const auto found = decoded.image.modules.find(artifact.manifest.root_module);
  if (found == decoded.image.modules.end()) {
    if (image != nullptr) {
      image->reset();
    }
    return {};
  }
  bytecode::BcModule root = found->second;
  if (image != nullptr) {
    *image = std::move(decoded.image);
  }
  return root;
}

} // namespace

ExecutionResult execute_code(const bytecode::BcModule &module,
                             std::uint32_t code_id,
                             const std::vector<Value> &args, Value self,
                             Value block) {
  Vm vm(module);
  return vm.execute(code_id, args, std::move(self), std::move(block));
}

struct RuntimeWorld::Impl {
  explicit Impl(const bytecode::BcModule &module_ref)
      : Impl(bytecode::BcModule(module_ref), std::nullopt,
             RuntimeWorldOptions{}) {}

  Impl(bytecode::BcModule module_value,
       std::optional<RuntimePackageImage> package_image,
       RuntimeWorldOptions world_options = {})
      : owned_module(
            std::make_shared<bytecode::BcModule>(std::move(module_value))),
        module(owned_module.get()), state(std::make_shared<RuntimeState>()),
        package(std::move(package_image)), options(std::move(world_options)) {
    state->initialize_for_module(*module);
    capabilities = capability::resolve_capabilities(module->capabilities,
                                                    options.capability_grants);
    effects = effect::validate_effect_summaries(
        module->effects, options.allowed_effects, options.enforce_effects);
    schemas =
        data::validate_schemas(module->schemas, module->schema_migrations);
    table_plans = data::validate_table_plans(module->table_plans);
    wasm_components =
        wasm_accel::validate_wasm_components(module->wasm_components);
    accelerator_kernels =
        wasm_accel::validate_accelerator_kernels(module->accelerator_kernels);
    agent_metadata = modern::validate_agent_metadata(
        module->agent_symbols, module->agent_patches,
        module->provenance_records);
    contract_metadata = modern::validate_contract_metadata(module->contracts,
                                                           module->properties);
    privacy_metadata = modern::validate_privacy_metadata(
        module->privacy_labels, module->privacy_policies,
        module->lineage_nodes);
    workflow_metadata = modern::validate_workflow_metadata(
        module->workflow_steps, module->workflow_history);
    trace.schema = "amber.replay.v1";
    trace.capability_grants = options.capability_grants;
    trace.schema_versions.push_back("amber.replay.v1");
    for (const data::SchemaDefinition &schema : schemas.schemas) {
      trace.schema_versions.push_back(data::schema_version_id(schema));
    }
    if (options.record_replay_trace || options.enforce_replay) {
      replay_validation.ok = true;
      record_event(replay::make_event(
          "loader.module.load",
          {{"module",
            package.has_value() ? package->manifest.root_module : "module"}}));
    }
  }

  replay::TraceEvent record_event(replay::TraceEvent event) {
    if (!options.record_replay_trace && !options.enforce_replay) {
      return event;
    }
    if (event.event_id == 0) {
      event.event_id = static_cast<std::uint64_t>(trace.events.size()) + 1U;
    }
    if (event.timestamp_or_virtual_time == 0) {
      event.timestamp_or_virtual_time =
          options.virtual_time_start +
          (event.event_id - 1U) * options.virtual_time_step;
    }
    if (event.trace_id.empty()) {
      event.trace_id = options.trace_id.empty() ? "runtime" : options.trace_id;
    }
    if (event.module_id.empty() && package.has_value()) {
      event.module_id = package->manifest.root_module;
    }
    if (event.world_epoch == 0 && state != nullptr) {
      event.world_epoch = state->world_epoch;
    }
    event = replay::normalize_event(std::move(event));
    trace.events.push_back(event);

    if (options.enforce_replay) {
      const replay::ReplayTrace expected =
          replay::normalize_trace(options.expected_replay);
      if (replay_cursor >= expected.events.size()) {
        replay_validation.diagnostics.push_back(replay::ReplayDiagnostic{
            "ReplayDivergenceError", "replay produced an extra event", 0,
            event.event_id, event.name});
      } else {
        const replay::TraceEvent expected_event =
            replay::normalize_event(expected.events[replay_cursor]);
        if (replay::event_signature(expected_event) !=
            replay::event_signature(event)) {
          replay_validation.diagnostics.push_back(replay::ReplayDiagnostic{
              "ReplayDivergenceError",
              "replay event diverged at index " + std::to_string(replay_cursor),
              expected_event.event_id, event.event_id, event.name});
        }
      }
      ++replay_cursor;
      replay_validation.consumed_events = replay_cursor;
      replay_validation.ok = replay_validation.diagnostics.empty();
    }
    return event;
  }

  RuntimeReplayValidation current_replay_validation() const {
    RuntimeReplayValidation result = replay_validation;
    if (options.enforce_replay && result.diagnostics.empty() &&
        replay_cursor < options.expected_replay.events.size()) {
      result.diagnostics.push_back(replay::ReplayDiagnostic{
          "ReplayDivergenceError",
          "replay ended before consuming expected events",
          options.expected_replay.events[replay_cursor].event_id, 0,
          options.expected_replay.events[replay_cursor].name});
    }
    result.consumed_events = replay_cursor;
    result.ok = result.diagnostics.empty();
    return result;
  }

  std::shared_ptr<bytecode::BcModule> owned_module;
  const bytecode::BcModule *module = nullptr;
  std::shared_ptr<RuntimeState> state;
  std::optional<RuntimePackageImage> package;
  RuntimeWorldOptions options;
  RuntimeCapabilityResolution capabilities;
  RuntimeEffectValidation effects;
  RuntimeSchemaValidation schemas;
  RuntimeTablePlanValidation table_plans;
  RuntimeWasmValidation wasm_components;
  RuntimeAcceleratorValidation accelerator_kernels;
  RuntimeAgentValidation agent_metadata;
  RuntimeContractValidation contract_metadata;
  RuntimePrivacyValidation privacy_metadata;
  RuntimeWorkflowValidation workflow_metadata;
  RuntimeReplayTrace trace;
  RuntimeReplayValidation replay_validation;
  std::size_t replay_cursor = 0;
};

RuntimeWorld::RuntimeWorld(const bytecode::BcModule &module)
    : impl_(std::make_shared<Impl>(module)) {}

RuntimeWorld::RuntimeWorld(const bytecode::BcModule &module,
                           RuntimeWorldOptions options)
    : impl_(std::make_shared<Impl>(bytecode::BcModule(module), std::nullopt,
                                   std::move(options))) {}

RuntimeWorld::RuntimeWorld(const pkg::PackageArtifact &artifact) {
  std::optional<RuntimePackageImage> image;
  bytecode::BcModule root = root_module_or_empty_for_package(artifact, &image);
  impl_ = std::make_shared<Impl>(std::move(root), std::move(image),
                                 RuntimeWorldOptions{});
}

RuntimeWorld::RuntimeWorld(const pkg::PackageArtifact &artifact,
                           RuntimeWorldOptions options) {
  std::optional<RuntimePackageImage> image;
  bytecode::BcModule root = root_module_or_empty_for_package(artifact, &image);
  impl_ = std::make_shared<Impl>(std::move(root), std::move(image),
                                 std::move(options));
}

RuntimeWorld::~RuntimeWorld() = default;

ExecutionResult RuntimeWorld::execute(std::uint32_t code_id,
                                      const std::vector<Value> &args,
                                      Value self, Value block) {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {Value::null(),
            Fault{"VMError", "runtime world is not bound", 0, 0}};
  }
  impl_->state->initialize_for_module(*impl_->module);
  impl_->record_event(replay::make_event(
      "task.started", {{"code_id", std::to_string(code_id)}}));
  const std::string module_id =
      impl_->package.has_value() ? impl_->package->manifest.root_module : "";
  Vm vm(*impl_->module, impl_->state, module_id);
  ExecutionResult result =
      vm.execute(code_id, args, std::move(self), std::move(block));
  if (result.ok()) {
    impl_->record_event(replay::make_event(
        "task.completed", {{"code_id", std::to_string(code_id)}}));
  } else {
    impl_->record_event(replay::make_event(
        "task.failed", {{"code_id", std::to_string(code_id)},
                        {"error_name", result.fault->error_name}}));
  }
  return result;
}

namespace {

ExecutionResult runtime_world_fault(const std::string &name,
                                    const std::string &message) {
  return {Value::null(), Fault{name, message, 0, 0}};
}

RuntimeOwnerKind owner_kind_for_index(const bytecode::BcModule &module,
                                      std::uint32_t owner_index) {
  if (owner_index < module.classes.size() &&
      (module.classes[owner_index].flags & bytecode::kClassFlagMixin) != 0U) {
    return RuntimeOwnerKind::Mixin;
  }
  return RuntimeOwnerKind::Class;
}

bool owner_is_mixin(const bytecode::BcModule &module,
                    std::uint32_t owner_index) {
  return owner_index < module.classes.size() &&
         (module.classes[owner_index].flags & bytecode::kClassFlagMixin) != 0U;
}

std::string join_class_path(const std::vector<std::string> &segments) {
  std::string out;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (i != 0U) {
      out += ".";
    }
    out += segments[i];
  }
  return out;
}

bool resolve_class_ref_for_world(const bytecode::BcModule &module,
                                 std::uint32_t path_ref,
                                 std::uint32_t *out_class_index,
                                 std::string *error_message) {
  if (path_ref >= module.const_pool.size()) {
    *error_message = "class path ref is out of range";
    return false;
  }
  const bytecode::Constant &constant = module.const_pool[path_ref];
  if (constant.kind != bytecode::ConstantKind::Path) {
    *error_message = "class ref must point to path constant";
    return false;
  }
  if (constant.items.empty()) {
    *error_message = "class path constant is empty";
    return false;
  }

  std::vector<std::string> segments;
  segments.reserve(constant.items.size());
  for (std::uint32_t symbol_id : constant.items) {
    if (symbol_id >= module.symbols.size()) {
      *error_message = "class path symbol ref is out of range";
      return false;
    }
    segments.push_back(module.symbols[symbol_id]);
  }

  const std::string full_path = join_class_path(segments);
  for (std::uint32_t index = 0; index < module.classes.size(); ++index) {
    const std::uint32_t symbol_id = module.classes[index].class_name_sym_id;
    if (symbol_id < module.symbols.size() &&
        module.symbols[symbol_id] == full_path) {
      *out_class_index = index;
      return true;
    }
  }

  const std::string &leaf = segments.back();
  std::optional<std::uint32_t> match;
  for (std::uint32_t index = 0; index < module.classes.size(); ++index) {
    const std::uint32_t symbol_id = module.classes[index].class_name_sym_id;
    if (symbol_id >= module.symbols.size() ||
        module.symbols[symbol_id] != leaf) {
      continue;
    }
    if (match.has_value()) {
      *error_message = "class path ref is ambiguous";
      return false;
    }
    match = index;
  }
  if (match.has_value()) {
    *out_class_index = *match;
    return true;
  }
  *error_message = "class path ref target is unknown";
  return false;
}

bool static_direct_refs_contain(const bytecode::BcModule &module,
                                const std::vector<std::uint32_t> &refs,
                                std::uint32_t mixin_index,
                                std::string *error_message) {
  for (std::uint32_t ref : refs) {
    std::uint32_t resolved = 0;
    if (!resolve_class_ref_for_world(module, ref, &resolved, error_message)) {
      return false;
    }
    if (resolved == mixin_index) {
      return true;
    }
  }
  return false;
}

bool effective_direct_mixins_contain(const bytecode::BcModule &module,
                                     const RuntimeState &state,
                                     std::uint32_t owner_index,
                                     std::uint32_t mixin_index, bool class_side,
                                     std::string *error_message) {
  if (owner_index < state.classes.size()) {
    const std::vector<std::uint32_t> &dynamic_mixins =
        class_side ? state.classes[owner_index].direct_extend_indices
                   : state.classes[owner_index].direct_include_indices;
    if (std::find(dynamic_mixins.begin(), dynamic_mixins.end(), mixin_index) !=
        dynamic_mixins.end()) {
      return true;
    }
  }
  if (owner_index >= module.classes.size()) {
    return false;
  }
  const std::vector<std::uint32_t> &static_refs =
      class_side ? module.classes[owner_index].direct_extend_refs
                 : module.classes[owner_index].direct_include_refs;
  return static_direct_refs_contain(module, static_refs, mixin_index,
                                    error_message);
}

bool append_static_include_edges(const bytecode::BcModule &module,
                                 std::uint32_t mixin_index,
                                 std::vector<std::uint32_t> *edges,
                                 Fault *fault) {
  for (std::uint32_t ref : module.classes[mixin_index].direct_include_refs) {
    std::uint32_t resolved = 0;
    std::string message;
    if (!resolve_class_ref_for_world(module, ref, &resolved, &message)) {
      *fault = Fault{"VMError", message, 0, 0};
      return false;
    }
    edges->push_back(resolved);
  }
  return true;
}

bool collect_include_edges_after_transaction(const bytecode::BcModule &module,
                                             const RuntimeState &state,
                                             const RuntimeWorldTransaction &tx,
                                             std::uint32_t mixin_index,
                                             std::vector<std::uint32_t> *edges,
                                             Fault *fault) {
  edges->clear();
  if (mixin_index < state.classes.size()) {
    const std::vector<std::uint32_t> &dynamic_mixins =
        state.classes[mixin_index].direct_include_indices;
    edges->insert(edges->end(), dynamic_mixins.begin(), dynamic_mixins.end());
  }
  if (tx.target_kind == RuntimeOwnerKind::Mixin &&
      tx.target_index == mixin_index) {
    edges->insert(edges->end(), tx.include_indices.begin(),
                  tx.include_indices.end());
  }
  return append_static_include_edges(module, mixin_index, edges, fault);
}

bool validate_mixin_include_graph_from(
    const bytecode::BcModule &module, const RuntimeState &state,
    const RuntimeWorldTransaction &tx, std::uint32_t mixin_index,
    std::vector<bool> *seen, std::vector<bool> *active, Fault *fault) {
  if (mixin_index >= module.classes.size()) {
    *fault = Fault{"VMError", "mixin target index is out of range", 0, 0};
    return false;
  }
  if (!owner_is_mixin(module, mixin_index)) {
    *fault = Fault{"TypeError", "include/extend target is not a mixin", 0, 0};
    return false;
  }
  if ((*active)[mixin_index]) {
    *fault = Fault{"IncludeCycleError", "cycle detected in mixin include graph",
                   0, 0};
    return false;
  }
  if ((*seen)[mixin_index]) {
    return true;
  }

  (*seen)[mixin_index] = true;
  (*active)[mixin_index] = true;

  std::vector<std::uint32_t> edges;
  if (!collect_include_edges_after_transaction(module, state, tx, mixin_index,
                                               &edges, fault)) {
    (*active)[mixin_index] = false;
    return false;
  }
  for (std::uint32_t edge : edges) {
    if (!validate_mixin_include_graph_from(module, state, tx, edge, seen,
                                           active, fault)) {
      (*active)[mixin_index] = false;
      return false;
    }
  }

  (*active)[mixin_index] = false;
  return true;
}

ExecutionResult validate_world_transaction(const bytecode::BcModule &module,
                                           const RuntimeState &state,
                                           const RuntimeWorldTransaction &tx) {
  if (state.world_frozen) {
    return runtime_world_fault("WorldFrozenError",
                               "world mutation after freeze barrier");
  }
  if (tx.target_index >= module.classes.size()) {
    return runtime_world_fault("VMError",
                               "world transaction target is out of range");
  }

  const RuntimeOwnerKind actual_kind =
      owner_kind_for_index(module, tx.target_index);
  if (actual_kind != tx.target_kind) {
    return runtime_world_fault("TypeError",
                               "world transaction target kind mismatch");
  }
  if (tx.target_kind == RuntimeOwnerKind::Mixin) {
    if (tx.has_superclass_ref) {
      return runtime_world_fault("TypeError",
                                 "mixin reopen cannot declare superclass");
    }
    if (!tx.class_methods.empty() || !tx.extend_indices.empty()) {
      return runtime_world_fault("TypeError",
                                 "mixin body cannot publish class-side state");
    }
  }
  if (tx.target_kind == RuntimeOwnerKind::Class && tx.has_superclass_ref) {
    const bytecode::BcClass &owner = module.classes[tx.target_index];
    if (!owner.has_superclass_ref) {
      return runtime_world_fault(
          "SuperclassMismatchError",
          "class reopen superclass does not match original declaration");
    }
    std::uint32_t existing_superclass = 0;
    std::uint32_t requested_superclass = 0;
    std::string message;
    if (!resolve_class_ref_for_world(module, owner.superclass_ref,
                                     &existing_superclass, &message) ||
        !resolve_class_ref_for_world(module, tx.superclass_ref,
                                     &requested_superclass, &message)) {
      return runtime_world_fault("VMError", message);
    }
    if (existing_superclass != requested_superclass) {
      return runtime_world_fault(
          "SuperclassMismatchError",
          "class reopen superclass does not match original declaration");
    }
  }

  for (const bytecode::BcMethod &method : tx.instance_methods) {
    if (method.selector_sym_id >= module.symbols.size()) {
      return runtime_world_fault("VMError",
                                 "define_method selector is out of range");
    }
    if (find_code(module, method.entry_code_id) == nullptr) {
      return runtime_world_fault("VMError",
                                 "define_method entry code id is unknown");
    }
  }
  for (const bytecode::BcMethod &method : tx.class_methods) {
    if (method.selector_sym_id >= module.symbols.size()) {
      return runtime_world_fault("VMError",
                                 "define_method selector is out of range");
    }
    if (find_code(module, method.entry_code_id) == nullptr) {
      return runtime_world_fault("VMError",
                                 "define_method entry code id is unknown");
    }
  }

  for (std::uint32_t mixin_index : tx.include_indices) {
    if (mixin_index >= module.classes.size()) {
      return runtime_world_fault("VMError",
                                 "mixin target index is out of range");
    }
    if (!owner_is_mixin(module, mixin_index)) {
      return runtime_world_fault("TypeError",
                                 "include/extend target is not a mixin");
    }
  }
  for (std::uint32_t mixin_index : tx.extend_indices) {
    if (mixin_index >= module.classes.size()) {
      return runtime_world_fault("VMError",
                                 "mixin target index is out of range");
    }
    if (!owner_is_mixin(module, mixin_index)) {
      return runtime_world_fault("TypeError",
                                 "include/extend target is not a mixin");
    }
  }

  if (!tx.include_indices.empty() || !tx.extend_indices.empty()) {
    std::vector<bool> seen(module.classes.size(), false);
    std::vector<bool> active(module.classes.size(), false);
    Fault fault;
    if (tx.target_kind == RuntimeOwnerKind::Mixin &&
        !validate_mixin_include_graph_from(module, state, tx, tx.target_index,
                                           &seen, &active, &fault)) {
      return {Value::null(), fault};
    }
    for (std::uint32_t mixin_index : tx.include_indices) {
      if (!validate_mixin_include_graph_from(module, state, tx, mixin_index,
                                             &seen, &active, &fault)) {
        return {Value::null(), fault};
      }
    }
    for (std::uint32_t mixin_index : tx.extend_indices) {
      if (!validate_mixin_include_graph_from(module, state, tx, mixin_index,
                                             &seen, &active, &fault)) {
        return {Value::null(), fault};
      }
    }
  }

  return {Value::null(), std::nullopt};
}

RuntimeOwnerKind owner_kind_for_module_ptr(const bytecode::BcModule *module,
                                           std::uint32_t owner_index) {
  if (module == nullptr || owner_index >= module->classes.size()) {
    return RuntimeOwnerKind::Class;
  }
  return owner_kind_for_index(*module, owner_index);
}

std::string symbol_name_for_mirror(const bytecode::BcModule &module,
                                   std::uint32_t symbol_id) {
  if (symbol_id >= module.symbols.size()) {
    return "";
  }
  return module.symbols[symbol_id];
}

std::string string_name_for_mirror(const bytecode::BcModule &module,
                                   std::uint32_t string_id) {
  if (string_id >= module.strings.size()) {
    return "";
  }
  return module.strings[string_id];
}

std::string owner_name_for_mirror(const bytecode::BcModule &module,
                                  std::uint32_t owner_index) {
  if (owner_index >= module.classes.size()) {
    return "";
  }
  return symbol_name_for_mirror(module,
                                module.classes[owner_index].class_name_sym_id);
}

RuntimeMirrorSourceLocation
source_location_for_code(const bytecode::BcModule &module,
                         std::uint32_t code_id) {
  RuntimeMirrorSourceLocation location;
  location.code_id = code_id;

  const bytecode::BcCode *code = find_code(module, code_id);
  if (code != nullptr && !code->source_spans.empty()) {
    const bytecode::SourceSpanEntry *best = &code->source_spans.front();
    for (const bytecode::SourceSpanEntry &span : code->source_spans) {
      if (span.pc_from < best->pc_from) {
        best = &span;
      }
    }
    location.present = true;
    location.pc = best->pc_from;
    location.file = best->span.file;
    location.line = static_cast<std::uint32_t>(best->span.start.line);
    location.column = static_cast<std::uint32_t>(best->span.start.col);
    return location;
  }

  const bytecode::LineEntry *best_line = nullptr;
  for (const bytecode::LineEntry &entry : module.line_table) {
    if (entry.code_id != code_id) {
      continue;
    }
    if (best_line == nullptr || entry.pc < best_line->pc) {
      best_line = &entry;
    }
  }
  if (best_line != nullptr) {
    location.present = true;
    location.pc = best_line->pc;
    location.line = best_line->line;
  }
  return location;
}

RuntimeMirrorSourceLocation
source_location_for_owner(const bytecode::BcModule &module,
                          const bytecode::BcClass &owner) {
  if (owner.has_class_init_code_id) {
    return source_location_for_code(module, owner.class_init_code_id);
  }
  if (owner.method_range_start + owner.method_range_count <=
      module.methods.size()) {
    for (std::uint32_t offset = 0; offset < owner.method_range_count;
         ++offset) {
      const bytecode::BcMethod &method =
          module.methods[owner.method_range_start + offset];
      RuntimeMirrorSourceLocation location =
          source_location_for_code(module, method.entry_code_id);
      if (location.present) {
        return location;
      }
    }
  }
  return {};
}

RuntimeMethodMirror method_mirror_for(const bytecode::BcModule &module,
                                      std::uint32_t owner_index,
                                      MethodTableSide side,
                                      const bytecode::BcMethod &method) {
  RuntimeMethodMirror mirror;
  mirror.selector_symbol_id = method.selector_sym_id;
  mirror.selector = symbol_name_for_mirror(module, method.selector_sym_id);
  mirror.owner_index = owner_index;
  mirror.owner_name = owner_name_for_mirror(module, owner_index);
  mirror.owner_kind = owner_kind_for_index(module, owner_index);
  mirror.side = side;
  mirror.signature_blob_id = method.signature_blob_id;
  mirror.entry_code_id = method.entry_code_id;
  mirror.flags = method.flags;
  mirror.parameter_count = method.params.size();
  mirror.default_count = method.default_thunk_ids.size();
  mirror.type_hook_count = method.type_hook_ids.size();
  mirror.clause_count = method.clause_table.size();
  mirror.source_location =
      source_location_for_code(module, method.entry_code_id);
  return mirror;
}

std::vector<RuntimeMethodMirror>
method_mirrors_for_table(const bytecode::BcModule &module,
                         std::uint32_t owner_index, MethodTableSide side,
                         const MethodTableDescriptor &table) {
  std::vector<RuntimeMethodMirror> mirrors;
  mirrors.reserve(table.entries.size());
  for (const auto &[selector_id, method] : table.entries) {
    (void)selector_id;
    mirrors.push_back(method_mirror_for(module, owner_index, side, method));
  }
  std::sort(
      mirrors.begin(), mirrors.end(),
      [](const RuntimeMethodMirror &left, const RuntimeMethodMirror &right) {
        if (left.selector != right.selector) {
          return left.selector < right.selector;
        }
        if (left.selector_symbol_id != right.selector_symbol_id) {
          return left.selector_symbol_id < right.selector_symbol_id;
        }
        return left.entry_code_id < right.entry_code_id;
      });
  return mirrors;
}

RuntimeDirectMixinMirror
direct_mixin_mirror_for_index(const bytecode::BcModule &module,
                              std::uint32_t mixin_index, bool dynamic) {
  RuntimeDirectMixinMirror mirror;
  mirror.index = mixin_index;
  mirror.name = owner_name_for_mirror(module, mixin_index);
  mirror.dynamic = dynamic;
  return mirror;
}

void append_static_direct_mixin_mirrors(
    const bytecode::BcModule &module, const std::vector<std::uint32_t> &refs,
    std::vector<RuntimeDirectMixinMirror> *mirrors) {
  for (std::uint32_t ref : refs) {
    std::uint32_t mixin_index = 0;
    std::string message;
    if (resolve_class_ref_for_world(module, ref, &mixin_index, &message)) {
      mirrors->push_back(
          direct_mixin_mirror_for_index(module, mixin_index, false));
    }
  }
}

void append_dynamic_direct_mixin_mirrors(
    const bytecode::BcModule &module, const std::vector<std::uint32_t> &indices,
    std::vector<RuntimeDirectMixinMirror> *mirrors) {
  for (std::uint32_t mixin_index : indices) {
    mirrors->push_back(
        direct_mixin_mirror_for_index(module, mixin_index, true));
  }
}

RuntimeOwnerMirror owner_mirror_for(const bytecode::BcModule &module,
                                    const RuntimeState &state,
                                    std::uint32_t owner_index) {
  RuntimeOwnerMirror mirror;
  mirror.index = owner_index;
  if (owner_index >= module.classes.size() ||
      owner_index >= state.classes.size()) {
    return mirror;
  }

  const bytecode::BcClass &owner = module.classes[owner_index];
  const ClassRuntimeState &runtime_owner = state.classes[owner_index];
  mirror.name = owner_name_for_mirror(module, owner_index);
  mirror.kind = owner_kind_for_index(module, owner_index);
  mirror.owner_flags = runtime_owner.owner_flags;
  mirror.ivar_schema_id = runtime_owner.ivar_schema_id;
  mirror.has_superclass = runtime_owner.has_superclass_ref;
  mirror.method_version = runtime_owner.method_version;
  mirror.world_epoch = state.world_epoch;
  mirror.source_location = source_location_for_owner(module, owner);

  if (runtime_owner.has_superclass_ref) {
    std::uint32_t superclass_index = 0;
    std::string message;
    if (resolve_class_ref_for_world(module, runtime_owner.superclass_ref,
                                    &superclass_index, &message)) {
      mirror.superclass_index = superclass_index;
      mirror.superclass_name = owner_name_for_mirror(module, superclass_index);
    }
  }

  append_static_direct_mixin_mirrors(module, owner.direct_include_refs,
                                     &mirror.direct_includes);
  append_dynamic_direct_mixin_mirrors(
      module, runtime_owner.direct_include_indices, &mirror.direct_includes);
  append_static_direct_mixin_mirrors(module, owner.direct_extend_refs,
                                     &mirror.direct_extends);
  append_dynamic_direct_mixin_mirrors(
      module, runtime_owner.direct_extend_indices, &mirror.direct_extends);

  mirror.instance_methods =
      method_mirrors_for_table(module, owner_index, MethodTableSide::Instance,
                               runtime_owner.instance_method_table);
  mirror.class_methods =
      method_mirrors_for_table(module, owner_index, MethodTableSide::Class,
                               runtime_owner.class_method_table);
  return mirror;
}

std::string package_name_for_mirror(const bytecode::BcModule &module) {
  for (const bytecode::AttrEntry &attr : module.attrs) {
    const std::string key = string_name_for_mirror(module, attr.key_str_id);
    if (key != "amber.package" && key != "package" && key != "module" &&
        key != "module.name") {
      continue;
    }
    const std::string value = string_name_for_mirror(module, attr.value_str_id);
    if (!value.empty()) {
      return value;
    }
  }
  return "";
}

RuntimePackageMirror package_mirror_for(const bytecode::BcModule &module) {
  RuntimePackageMirror mirror;
  mirror.name = package_name_for_mirror(module);
  mirror.format_version = module.format_version;
  mirror.language_version = module.language_version;
  mirror.profile_flags = module.profile_flags;
  mirror.file_flags = module.file_flags;
  mirror.has_init = module.init.has_entry_code_id;
  mirror.init_code_id = module.init.entry_code_id;

  for (const bytecode::DepEntry &dependency : module.dependencies) {
    RuntimePackageDependencyMirror dep;
    dep.module_name =
        string_name_for_mirror(module, dependency.module_name_str_id);
    dep.required_format = dependency.required_format;
    dep.min_language_version = dependency.min_language_version;
    dep.has_max_language_version = dependency.has_max_language_version;
    dep.max_language_version = dependency.max_language_version;
    dep.has_abi_requirement = dependency.has_abi_requirement;
    mirror.dependencies.push_back(dep);
  }
  std::sort(mirror.dependencies.begin(), mirror.dependencies.end(),
            [](const RuntimePackageDependencyMirror &left,
               const RuntimePackageDependencyMirror &right) {
              return left.module_name < right.module_name;
            });

  for (const bytecode::ExportEntry &entry : module.exports) {
    RuntimePackageExportMirror export_mirror;
    export_mirror.public_name = symbol_name_for_mirror(module, entry.symbol_id);
    export_mirror.target_kind =
        string_name_for_mirror(module, entry.target_kind_str_id);
    export_mirror.target_index = entry.target_index;
    export_mirror.visibility_flags = entry.visibility_flags;
    export_mirror.has_reexport = entry.has_reexport_module_name;
    export_mirror.reexport_module_name =
        string_name_for_mirror(module, entry.reexport_module_name_str_id);
    mirror.exports.push_back(export_mirror);
  }
  std::sort(mirror.exports.begin(), mirror.exports.end(),
            [](const RuntimePackageExportMirror &left,
               const RuntimePackageExportMirror &right) {
              if (left.public_name != right.public_name) {
                return left.public_name < right.public_name;
              }
              if (left.target_kind != right.target_kind) {
                return left.target_kind < right.target_kind;
              }
              return left.target_index < right.target_index;
            });

  for (const bytecode::AttrEntry &attr : module.attrs) {
    mirror.attrs.push_back({string_name_for_mirror(module, attr.key_str_id),
                            string_name_for_mirror(module, attr.value_str_id)});
  }
  std::sort(mirror.attrs.begin(), mirror.attrs.end(),
            [](const RuntimePackageAttrMirror &left,
               const RuntimePackageAttrMirror &right) {
              if (left.key != right.key) {
                return left.key < right.key;
              }
              return left.value < right.value;
            });

  mirror.capabilities = module.capabilities;
  std::sort(mirror.capabilities.begin(), mirror.capabilities.end(),
            [](const RuntimeCapabilityGrant &left,
               const RuntimeCapabilityGrant &right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.target < right.target;
            });
  mirror.effects = module.effects;
  std::sort(
      mirror.effects.begin(), mirror.effects.end(),
      [](const RuntimeEffectSummary &left, const RuntimeEffectSummary &right) {
        if (left.owner != right.owner) {
          return left.owner < right.owner;
        }
        return left.kind < right.kind;
      });
  mirror.observability_sites = module.observability_sites;
  std::sort(mirror.observability_sites.begin(),
            mirror.observability_sites.end(),
            [](const RuntimeObservabilitySite &left,
               const RuntimeObservabilitySite &right) {
              return left.site_id < right.site_id;
            });
  mirror.replay_metadata = replay::normalize_metadata(module.replay_metadata);
  const RuntimeSchemaValidation schema_validation =
      data::validate_schemas(module.schemas, module.schema_migrations);
  mirror.schemas = schema_validation.schemas;
  mirror.schema_migrations = schema_validation.migrations;
  mirror.table_plans = data::validate_table_plans(module.table_plans).plans;
  mirror.wasm_components =
      wasm_accel::validate_wasm_components(module.wasm_components).components;
  mirror.accelerator_kernels =
      wasm_accel::validate_accelerator_kernels(module.accelerator_kernels)
          .kernels;
  mirror.agent_symbols =
      modern::validate_agent_metadata(
          module.agent_symbols, module.agent_patches, module.provenance_records)
          .symbols;
  mirror.contracts =
      modern::validate_contract_metadata(module.contracts, module.properties)
          .contracts;
  mirror.privacy_labels =
      modern::validate_privacy_metadata(
          module.privacy_labels, module.privacy_policies, module.lineage_nodes)
          .labels;
  mirror.workflow_steps = modern::validate_workflow_metadata(
                              module.workflow_steps, module.workflow_history)
                              .steps;

  return mirror;
}

std::string version_signature(const bytecode::Version &version) {
  return std::to_string(version.major) + "." + std::to_string(version.minor);
}

std::string bytes_hex_signature(const std::array<std::uint8_t, 32> &bytes) {
  std::ostringstream out;
  const char *hex = "0123456789abcdef";
  for (const std::uint8_t byte : bytes) {
    out << hex[(byte >> 4U) & 0x0FU] << hex[byte & 0x0FU];
  }
  return out.str();
}

std::string module_contract_signature(const bytecode::BcModule &module) {
  std::ostringstream out;
  out << "format=" << version_signature(module.format_version)
      << "\nlanguage=" << version_signature(module.language_version)
      << "\nprofile=" << module.profile_flags << "\nfile=" << module.file_flags
      << "\nabi=" << bytes_hex_signature(module.abi_hash) << "\n";
  std::vector<RuntimeCapabilityGrant> capabilities = module.capabilities;
  std::sort(capabilities.begin(), capabilities.end(),
            [](const RuntimeCapabilityGrant &left,
               const RuntimeCapabilityGrant &right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.target < right.target;
            });
  for (const RuntimeCapabilityGrant &capability : capabilities) {
    out << "capability=" << capability.name << "|" << capability.target << "|"
        << capability.flags << "\n";
  }
  std::vector<RuntimeEffectSummary> effects = module.effects;
  std::sort(
      effects.begin(), effects.end(),
      [](const RuntimeEffectSummary &left, const RuntimeEffectSummary &right) {
        if (left.owner != right.owner) {
          return left.owner < right.owner;
        }
        return left.kind < right.kind;
      });
  for (const RuntimeEffectSummary &summary : effects) {
    out << "effect=" << summary.owner << "|" << summary.kind << "|"
        << effect::effect_row_to_text(summary.declared_effects) << "|"
        << effect::effect_row_to_text(summary.observed_effects) << "|"
        << summary.flags << "\n";
  }
  std::vector<RuntimeObservabilitySite> sites = module.observability_sites;
  std::sort(sites.begin(), sites.end(),
            [](const RuntimeObservabilitySite &left,
               const RuntimeObservabilitySite &right) {
              return left.site_id < right.site_id;
            });
  for (const RuntimeObservabilitySite &site : sites) {
    out << "observability=" << site.site_id << "|" << site.event_name << "|"
        << site.kind << "|" << site.owner << "|" << site.source.file << "|"
        << site.source.line << "|" << site.source.column << "|" << site.flags
        << "\n";
  }
  const RuntimeReplayMetadata replay_metadata =
      replay::normalize_metadata(module.replay_metadata);
  out << "replay.flags=" << replay_metadata.flags << "\n";
  for (const std::string &event_name : replay_metadata.required_event_names) {
    out << "replay.required_event=" << event_name << "\n";
  }
  for (const std::string &source : replay_metadata.deterministic_sources) {
    out << "replay.deterministic_source=" << source << "\n";
  }
  const RuntimeSchemaValidation schema_validation =
      data::validate_schemas(module.schemas, module.schema_migrations);
  for (const RuntimeSchemaDefinition &schema : schema_validation.schemas) {
    out << "schema=" << data::schema_version_id(schema) << "|" << schema.flags
        << "\n";
    for (const data::SchemaField &field : schema.fields) {
      out << "schema.field=" << data::schema_version_id(schema) << "|"
          << field.name << "|" << field.type << "|" << (field.required ? 1 : 0)
          << "|" << (field.nullable ? 1 : 0) << "|" << field.default_value
          << "|" << field.flags << "\n";
    }
  }
  for (const RuntimeSchemaMigration &migration : schema_validation.migrations) {
    out << "schema.migration=" << migration.schema_name << "|"
        << migration.from_version << "|" << migration.to_version << "|"
        << migration.kind << "|" << migration.flags << "\n";
  }
  const RuntimeTablePlanValidation table_validation =
      data::validate_table_plans(module.table_plans);
  for (const RuntimeTablePlan &plan : table_validation.plans) {
    out << "table.plan=" << plan.plan_id << "|"
        << data::table_plan_fingerprint(plan) << "|" << plan.flags << "\n";
  }
  const RuntimeWasmValidation wasm_validation =
      wasm_accel::validate_wasm_components(module.wasm_components);
  for (const RuntimeWasmComponent &component : wasm_validation.components) {
    out << "wasm.component=" << component.name << "|" << component.world << "|"
        << component.flags << "\n";
    for (const wasm_accel::WasmInterfaceEntry &entry : component.imports) {
      out << "wasm.import=" << component.name << "|" << entry.name << "|"
          << entry.kind << "|" << entry.type_signature << "|"
          << entry.schema_name << "|"
          << capability::request_to_text(entry.capability) << "|"
          << effect::effect_row_to_text(entry.effect_row) << "|" << entry.flags
          << "\n";
    }
    for (const wasm_accel::WasmInterfaceEntry &entry : component.exports) {
      out << "wasm.export=" << component.name << "|" << entry.name << "|"
          << entry.kind << "|" << entry.type_signature << "|"
          << entry.schema_name << "|"
          << effect::effect_row_to_text(entry.effect_row) << "|" << entry.flags
          << "\n";
    }
  }
  const RuntimeAcceleratorValidation accelerator_validation =
      wasm_accel::validate_accelerator_kernels(module.accelerator_kernels);
  for (const RuntimeAcceleratorKernel &kernel :
       accelerator_validation.kernels) {
    out << "accelerator.kernel=" << kernel.kernel_id << "|" << kernel.entry
        << "|" << kernel.target << "|"
        << effect::effect_row_to_text(kernel.effect_row) << "|" << kernel.flags
        << "\n";
    for (const wasm_accel::AcceleratorValue &value : kernel.params) {
      out << "accelerator.param=" << kernel.kernel_id << "|" << value.name
          << "|" << value.type << "|" << value.address_space << "|"
          << value.flags << "\n";
    }
    for (const wasm_accel::AcceleratorValue &value : kernel.captures) {
      out << "accelerator.capture=" << kernel.kernel_id << "|" << value.name
          << "|" << value.type << "|" << value.address_space << "|"
          << value.flags << "\n";
    }
    for (const std::string &feature : kernel.forbidden_features) {
      out << "accelerator.forbidden=" << kernel.kernel_id << "|" << feature
          << "\n";
    }
  }
  return out.str();
}

std::string path_ref_signature(const bytecode::BcModule &module,
                               std::uint32_t ref) {
  if (ref >= module.const_pool.size()) {
    return "#invalid";
  }
  const bytecode::Constant &constant = module.const_pool[ref];
  if (constant.kind != bytecode::ConstantKind::Path) {
    return "#non-path";
  }
  std::vector<std::string> segments;
  for (const std::uint32_t symbol_id : constant.items) {
    segments.push_back(symbol_name_for_mirror(module, symbol_id));
  }
  return join_class_path(segments);
}

std::string method_boundary_signature(const bytecode::BcModule &module,
                                      const bytecode::BcMethod &method) {
  std::ostringstream out;
  out << symbol_name_for_mirror(module, method.selector_sym_id) << "|"
      << (method.flags == kMethodFlagClass ? "class" : "instance") << "|"
      << "defaults=" << method.default_thunk_ids.size() << "|"
      << "type_hooks=" << method.type_hook_ids.size() << "|params=";
  for (std::size_t i = 0; i < method.params.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const bytecode::MethodParamEntry &param = method.params[i];
    out << symbol_name_for_mirror(module, param.external_name_sym_id) << ":"
        << param.flags;
  }
  return out.str();
}

std::vector<std::string>
owner_method_boundary_signatures(const bytecode::BcModule &module,
                                 const bytecode::BcClass &owner) {
  std::vector<std::string> out;
  if (owner.method_range_start + owner.method_range_count >
      module.methods.size()) {
    out.push_back("#invalid-method-range");
    return out;
  }
  for (std::uint32_t offset = 0; offset < owner.method_range_count; ++offset) {
    out.push_back(method_boundary_signature(
        module, module.methods[owner.method_range_start + offset]));
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string owner_boundary_signature(const bytecode::BcModule &module,
                                     std::uint32_t owner_index) {
  if (owner_index >= module.classes.size()) {
    return "#missing-owner";
  }
  const bytecode::BcClass &owner = module.classes[owner_index];
  std::ostringstream out;
  out << "name=" << owner_name_for_mirror(module, owner_index) << "\nkind="
      << ((owner.flags & bytecode::kClassFlagMixin) != 0U ? "mixin" : "class")
      << "\nivar_schema=" << owner.ivar_schema_id
      << "\nhas_super=" << (owner.has_superclass_ref ? "true" : "false");
  if (owner.has_superclass_ref) {
    out << "\nsuper=" << path_ref_signature(module, owner.superclass_ref);
  }
  out << "\ninclude=";
  for (std::uint32_t ref : owner.direct_include_refs) {
    out << path_ref_signature(module, ref) << ";";
  }
  out << "\nextend=";
  for (std::uint32_t ref : owner.direct_extend_refs) {
    out << path_ref_signature(module, ref) << ";";
  }
  out << "\nmethods=";
  const std::vector<std::string> methods =
      owner_method_boundary_signatures(module, owner);
  for (const std::string &method : methods) {
    out << method << "\n";
  }
  return out.str();
}

std::string reexport_export_name_for(const bytecode::BcModule &module,
                                     const bytecode::ExportEntry &entry,
                                     const std::string &public_name) {
  if (entry.target_index < module.strings.size()) {
    return module.strings[entry.target_index];
  }
  return public_name;
}

std::vector<std::string>
export_surface_signature(const bytecode::BcModule &module) {
  std::vector<std::string> out;
  for (const bytecode::ExportEntry &entry : module.exports) {
    const std::string public_name =
        symbol_name_for_mirror(module, entry.symbol_id);
    const std::string target_kind =
        string_name_for_mirror(module, entry.target_kind_str_id);
    std::ostringstream line;
    line << public_name << "|" << target_kind << "|" << entry.target_index
         << "|" << entry.visibility_flags << "|"
         << (entry.has_reexport_module_name ? "reexport" : "local") << "|"
         << string_name_for_mirror(module, entry.reexport_module_name_str_id)
         << "|" << reexport_export_name_for(module, entry, public_name);
    out.push_back(line.str());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string>
exported_callable_boundary_signature(const bytecode::BcModule &module) {
  std::vector<std::string> out;
  for (const bytecode::ExportEntry &entry : module.exports) {
    const std::string public_name =
        symbol_name_for_mirror(module, entry.symbol_id);
    const std::string target_kind =
        string_name_for_mirror(module, entry.target_kind_str_id);
    if (target_kind == "class") {
      out.push_back("class|" + public_name + "|" +
                    owner_boundary_signature(module, entry.target_index));
    } else if (target_kind == "method") {
      if (entry.target_index >= module.methods.size()) {
        out.push_back("method|" + public_name + "|#missing-method");
      } else {
        out.push_back("method|" + public_name + "|" +
                      method_boundary_signature(
                          module, module.methods[entry.target_index]));
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string>
manifest_identity_signature(const pkg::PackageManifest &manifest) {
  std::vector<std::string> out;
  out.push_back("name=" + manifest.name);
  out.push_back("version=" + manifest.version);
  out.push_back("root=" + manifest.root_module);
  for (const pkg::PackageModule &module : manifest.modules) {
    out.push_back("module=" + module.name + "|" + module.path);
  }
  for (const pkg::PackageDependency &dependency : manifest.dependencies) {
    out.push_back("dependency=" + dependency.name + "|" + dependency.version +
                  "|" + dependency.source + "|" + dependency.checksum);
  }
  for (const RuntimeCapabilityGrant &capability : manifest.capabilities) {
    out.push_back("capability=" + capability.name + "|" + capability.target +
                  "|" + std::to_string(capability.flags));
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool append_reload_incompatibility(
    std::vector<RuntimePackageReloadDiagnostic> *diagnostics,
    const std::string &message, const std::string &module_name = {}) {
  diagnostics->push_back(runtime_reload_diagnostic("ReloadIncompatibleError",
                                                   message, module_name));
  return false;
}

bool validate_module_reload_compatible(
    const bytecode::BcModule &current, const bytecode::BcModule &next,
    const std::string &module_name,
    std::vector<RuntimePackageReloadDiagnostic> *diagnostics) {
  if (module_contract_signature(current) != module_contract_signature(next)) {
    return append_reload_incompatibility(
        diagnostics, "package reload changes ABI/profile contract",
        module_name);
  }
  if (export_surface_signature(current) != export_surface_signature(next)) {
    return append_reload_incompatibility(
        diagnostics, "package reload changes public export surface",
        module_name);
  }
  if (exported_callable_boundary_signature(current) !=
      exported_callable_boundary_signature(next)) {
    return append_reload_incompatibility(
        diagnostics, "package reload changes exported selector/arity boundary",
        module_name);
  }
  return true;
}

bool validate_package_reload_compatible(
    const RuntimePackageImage *current_package,
    const bytecode::BcModule &current_root, const RuntimePackageImage &next,
    std::vector<RuntimePackageReloadDiagnostic> *diagnostics) {
  if (current_package == nullptr) {
    const auto root = next.modules.find(next.manifest.root_module);
    if (root == next.modules.end()) {
      return append_reload_incompatibility(
          diagnostics, "package reload root module is missing",
          next.manifest.root_module);
    }
    return validate_module_reload_compatible(
        current_root, root->second, next.manifest.root_module, diagnostics);
  }

  if (manifest_identity_signature(current_package->manifest) !=
      manifest_identity_signature(next.manifest)) {
    return append_reload_incompatibility(
        diagnostics, "package reload changes manifest identity");
  }

  if (current_package->modules.size() != next.modules.size()) {
    return append_reload_incompatibility(diagnostics,
                                         "package reload changes module set");
  }
  for (const auto &[module_name, current_module] : current_package->modules) {
    const auto found = next.modules.find(module_name);
    if (found == next.modules.end()) {
      return append_reload_incompatibility(
          diagnostics, "package reload removes module: " + module_name,
          module_name);
    }
    if (!validate_module_reload_compatible(current_module, found->second,
                                           module_name, diagnostics)) {
      return false;
    }
  }
  return true;
}

} // namespace

ExecutionResult
RuntimeWorld::define_instance_method(std::uint32_t class_index,
                                     bytecode::BcMethod method) {
  RuntimeWorldTransaction tx;
  tx.target_index = class_index;
  if (impl_ != nullptr) {
    tx.target_kind = owner_kind_for_module_ptr(impl_->module, class_index);
  }
  tx.instance_methods.push_back(std::move(method));
  return commit_transaction(tx);
}

ExecutionResult RuntimeWorld::define_class_method(std::uint32_t class_index,
                                                  bytecode::BcMethod method) {
  RuntimeWorldTransaction tx;
  tx.target_kind = RuntimeOwnerKind::Class;
  tx.target_index = class_index;
  tx.class_methods.push_back(std::move(method));
  return commit_transaction(tx);
}

ExecutionResult RuntimeWorld::include_mixin(std::uint32_t class_index,
                                            std::uint32_t mixin_index) {
  RuntimeWorldTransaction tx;
  tx.target_index = class_index;
  if (impl_ != nullptr) {
    tx.target_kind = owner_kind_for_module_ptr(impl_->module, class_index);
  }
  tx.include_indices.push_back(mixin_index);
  return commit_transaction(tx);
}

ExecutionResult RuntimeWorld::extend_mixin(std::uint32_t class_index,
                                           std::uint32_t mixin_index) {
  RuntimeWorldTransaction tx;
  tx.target_kind = RuntimeOwnerKind::Class;
  tx.target_index = class_index;
  tx.extend_indices.push_back(mixin_index);
  return commit_transaction(tx);
}

ExecutionResult
RuntimeWorld::commit_transaction(const RuntimeWorldTransaction &tx) {
  if (impl_ == nullptr || impl_->module == nullptr || impl_->state == nullptr) {
    return {Value::null(),
            Fault{"VMError", "runtime world is not bound", 0, 0}};
  }
  const bytecode::BcModule &module = *impl_->module;
  RuntimeState &state = *impl_->state;
  if (state.classes.size() < module.classes.size()) {
    state.classes.resize(module.classes.size());
  }
  state.initialize_for_module(module);

  const ExecutionResult validation =
      validate_world_transaction(module, state, tx);
  if (!validation.ok()) {
    return validation;
  }

  ClassRuntimeState &runtime_owner = state.classes[tx.target_index];
  bool changed = false;
  for (bytecode::BcMethod method : tx.instance_methods) {
    method.owner_dispatch_ref = tx.target_index;
    method.flags = kMethodFlagInstance;
    runtime_owner.instance_method_table.entries[method.selector_sym_id] =
        std::move(method);
    changed = true;
  }
  for (bytecode::BcMethod method : tx.class_methods) {
    method.owner_dispatch_ref = tx.target_index;
    method.flags = kMethodFlagClass;
    runtime_owner.class_method_table.entries[method.selector_sym_id] =
        std::move(method);
    changed = true;
  }

  for (std::uint32_t mixin_index : tx.include_indices) {
    std::string message;
    if (!effective_direct_mixins_contain(module, state, tx.target_index,
                                         mixin_index, false, &message)) {
      runtime_owner.direct_include_indices.push_back(mixin_index);
      changed = true;
    }
  }
  for (std::uint32_t mixin_index : tx.extend_indices) {
    std::string message;
    if (!effective_direct_mixins_contain(module, state, tx.target_index,
                                         mixin_index, true, &message)) {
      runtime_owner.direct_extend_indices.push_back(mixin_index);
      changed = true;
    }
  }

  if (changed) {
    state.invalidate_dispatch_owner(tx.target_index);
    impl_->record_event(replay::make_event(
        "world.mutation", {{"target_index", std::to_string(tx.target_index)}}));
  }
  return {Value::null(), std::nullopt};
}

ExecutionResult RuntimeWorld::freeze_world() {
  if (impl_ == nullptr || impl_->module == nullptr || impl_->state == nullptr) {
    return {Value::null(),
            Fault{"VMError", "runtime world is not bound", 0, 0}};
  }
  impl_->state->initialize_for_module(*impl_->module);
  if (!impl_->state->world_frozen) {
    impl_->state->world_frozen = true;
    ++impl_->state->world_epoch;
    impl_->record_event(replay::make_event("world.freeze"));
  }
  return {Value::null(), std::nullopt};
}

RuntimePackageReloadResult
RuntimeWorld::reload_package_artifact(const pkg::PackageArtifact &artifact) {
  RuntimePackageReloadResult result;
  if (impl_ == nullptr || impl_->module == nullptr || impl_->state == nullptr) {
    result.diagnostics.push_back(
        runtime_reload_diagnostic("VMError", "runtime world is not bound"));
    return result;
  }

  result.previous_world_epoch = impl_->state->world_epoch;
  result.new_world_epoch = impl_->state->world_epoch;
  result.package_name = impl_->package.has_value()
                            ? impl_->package->manifest.name
                            : package_name_for_mirror(*impl_->module);
  result.previous_version =
      impl_->package.has_value() ? impl_->package->manifest.version : "";
  result.new_version = artifact.manifest.version;
  result.root_module = artifact.manifest.root_module;

  impl_->state->initialize_for_module(*impl_->module);
  if (impl_->state->world_frozen) {
    result.diagnostics.push_back(runtime_reload_diagnostic(
        "WorldFrozenError", "package reload after freeze barrier",
        artifact.manifest.root_module));
    return result;
  }

  RuntimePackageImageDecode decoded = decode_runtime_package_image(artifact);
  if (!decoded.ok) {
    result.diagnostics = std::move(decoded.diagnostics);
    return result;
  }

  const RuntimePackageImage *current_package =
      impl_->package.has_value() ? &*impl_->package : nullptr;
  if (!validate_package_reload_compatible(current_package, *impl_->module,
                                          decoded.image, &result.diagnostics)) {
    return result;
  }

  const auto root =
      decoded.image.modules.find(decoded.image.manifest.root_module);
  if (root == decoded.image.modules.end()) {
    result.diagnostics.push_back(runtime_reload_diagnostic(
        "PackageReloadError",
        "package root module is missing: " + decoded.image.manifest.root_module,
        decoded.image.manifest.root_module));
    return result;
  }

  auto next_state = std::make_shared<RuntimeState>(*impl_->state);
  next_state->replace_module_runtime_state(root->second);
  auto next_module =
      std::make_shared<bytecode::BcModule>(bytecode::BcModule(root->second));

  impl_->state = std::move(next_state);
  impl_->owned_module = std::move(next_module);
  impl_->module = impl_->owned_module.get();
  impl_->package = std::move(decoded.image);
  impl_->capabilities = capability::resolve_capabilities(
      impl_->module->capabilities, impl_->options.capability_grants);
  impl_->effects = effect::validate_effect_summaries(
      impl_->module->effects, impl_->options.allowed_effects,
      impl_->options.enforce_effects);
  impl_->schemas = data::validate_schemas(impl_->module->schemas,
                                          impl_->module->schema_migrations);
  impl_->table_plans = data::validate_table_plans(impl_->module->table_plans);
  impl_->wasm_components =
      wasm_accel::validate_wasm_components(impl_->module->wasm_components);
  impl_->accelerator_kernels = wasm_accel::validate_accelerator_kernels(
      impl_->module->accelerator_kernels);
  impl_->agent_metadata = modern::validate_agent_metadata(
      impl_->module->agent_symbols, impl_->module->agent_patches,
      impl_->module->provenance_records);
  impl_->contract_metadata = modern::validate_contract_metadata(
      impl_->module->contracts, impl_->module->properties);
  impl_->privacy_metadata = modern::validate_privacy_metadata(
      impl_->module->privacy_labels, impl_->module->privacy_policies,
      impl_->module->lineage_nodes);
  impl_->workflow_metadata = modern::validate_workflow_metadata(
      impl_->module->workflow_steps, impl_->module->workflow_history);

  result.ok = true;
  result.swapped = true;
  result.package_name = impl_->package->manifest.name;
  result.previous_version = result.previous_version.empty()
                                ? impl_->package->manifest.version
                                : result.previous_version;
  result.new_version = impl_->package->manifest.version;
  result.root_module = impl_->package->manifest.root_module;
  result.new_world_epoch = impl_->state->world_epoch;
  impl_->record_event(replay::make_event("loader.module.load",
                                         {{"module", result.root_module}}));
  impl_->record_event(
      replay::make_event("world.mutation", {{"package", result.package_name},
                                            {"version", result.new_version}}));
  return result;
}

RuntimeCapabilityCheckResult
RuntimeWorld::check_capability(const std::string &capability_name,
                               const std::string &target) const {
  RuntimeCapabilityCheckResult result;
  result.capability = capability_name;
  result.target = target;
  if (impl_ == nullptr || impl_->module == nullptr) {
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  impl_->record_event(
      replay::make_event("capability.check", {{"capability", capability_name},
                                              {"target", target}}));
  if (capability::capability_set_allows(impl_->capabilities.effective,
                                        capability_name, target)) {
    result.ok = true;
    return result;
  }
  result.error_name = "CapabilityError";
  result.message = "capability is not granted: " + capability_name;
  if (!target.empty()) {
    result.message += "=" + target;
  }
  impl_->record_event(
      replay::make_event("capability.denied", {{"capability", capability_name},
                                               {"target", target}}));
  return result;
}

RuntimeCapabilityResolution RuntimeWorld::capability_resolution() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->capabilities;
}

RuntimeEffectCheckResult
RuntimeWorld::check_effects(const std::vector<std::string> &requested) const {
  RuntimeEffectCheckResult result;
  result.effects = effect::normalize_effects(
      std::vector<std::string>(requested.begin(), requested.end()));
  if (impl_ == nullptr || impl_->module == nullptr) {
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  impl_->record_event(replay::make_event(
      "effect.boundary",
      {{"effects", effect::effect_row_to_text(result.effects)}}));
  if (!impl_->effects.ok && !impl_->effects.diagnostics.empty()) {
    result.error_name = "EffectViolationError";
    result.message = impl_->effects.diagnostics.front().message;
    return result;
  }
  for (const std::string &label : result.effects) {
    if (!effect::valid_effect_name(label)) {
      result.error_name = "EffectViolationError";
      result.message = "invalid effect label: " + label;
      return result;
    }
  }
  if (impl_->options.enforce_effects &&
      !effect::effects_subset_of(result.effects,
                                 impl_->options.allowed_effects)) {
    result.error_name = "EffectViolationError";
    result.message =
        "effect is not allowed: " + effect::effect_row_to_text(result.effects);
    return result;
  }
  result.ok = true;
  return result;
}

RuntimeEffectValidation RuntimeWorld::effect_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->effects;
}

RuntimeSchemaValidation RuntimeWorld::schema_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->schemas;
}

RuntimeTablePlanValidation RuntimeWorld::table_plan_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->table_plans;
}

RuntimeWasmValidation RuntimeWorld::wasm_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->wasm_components;
}

RuntimeAcceleratorValidation RuntimeWorld::accelerator_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->accelerator_kernels;
}

RuntimeAgentValidation RuntimeWorld::agent_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->agent_metadata;
}

RuntimeContractValidation RuntimeWorld::contract_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->contract_metadata;
}

RuntimePrivacyValidation RuntimeWorld::privacy_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->privacy_metadata;
}

RuntimeWorkflowValidation RuntimeWorld::workflow_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->workflow_metadata;
}

RuntimeTraceEvent RuntimeWorld::record_trace_event(RuntimeTraceEvent event) {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return event;
  }
  return impl_->record_event(std::move(event));
}

RuntimeReplayTrace RuntimeWorld::replay_trace() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return replay::normalize_trace(impl_->trace);
}

RuntimeReplayValidation RuntimeWorld::replay_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->current_replay_validation();
}

std::uint64_t RuntimeWorld::world_epoch() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->world_epoch;
}

RuntimeWorldState RuntimeWorld::world_state() const {
  if (impl_ == nullptr || impl_->state == nullptr ||
      !impl_->state->world_frozen) {
    return RuntimeWorldState::Open;
  }
  return RuntimeWorldState::Frozen;
}

bool RuntimeWorld::is_world_frozen() const {
  return world_state() == RuntimeWorldState::Frozen;
}

std::uint64_t RuntimeWorld::method_version(std::uint32_t class_index) const {
  if (impl_ == nullptr || impl_->state == nullptr ||
      class_index >= impl_->state->classes.size()) {
    return 0;
  }
  return impl_->state->classes[class_index].method_version;
}

std::size_t RuntimeWorld::method_table_size(std::uint32_t class_index,
                                            MethodTableSide side) const {
  if (impl_ == nullptr || impl_->state == nullptr ||
      class_index >= impl_->state->classes.size()) {
    return 0;
  }
  const ClassRuntimeState &owner = impl_->state->classes[class_index];
  return side == MethodTableSide::Class
             ? owner.class_method_table.entries.size()
             : owner.instance_method_table.entries.size();
}

RuntimePackageMirror RuntimeWorld::package_mirror() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return package_mirror_for(*impl_->module);
}

std::optional<RuntimeOwnerMirror>
RuntimeWorld::owner_mirror(std::uint32_t owner_index) const {
  if (impl_ == nullptr || impl_->module == nullptr || impl_->state == nullptr ||
      owner_index >= impl_->module->classes.size()) {
    return std::nullopt;
  }
  impl_->state->initialize_for_module(*impl_->module);
  return owner_mirror_for(*impl_->module, *impl_->state, owner_index);
}

std::optional<RuntimeOwnerMirror>
RuntimeWorld::class_mirror(std::uint32_t class_index) const {
  if (impl_ == nullptr || impl_->module == nullptr ||
      class_index >= impl_->module->classes.size() ||
      owner_kind_for_index(*impl_->module, class_index) !=
          RuntimeOwnerKind::Class) {
    return std::nullopt;
  }
  return owner_mirror(class_index);
}

std::optional<RuntimeOwnerMirror>
RuntimeWorld::mixin_mirror(std::uint32_t mixin_index) const {
  if (impl_ == nullptr || impl_->module == nullptr ||
      mixin_index >= impl_->module->classes.size() ||
      owner_kind_for_index(*impl_->module, mixin_index) !=
          RuntimeOwnerKind::Mixin) {
    return std::nullopt;
  }
  return owner_mirror(mixin_index);
}

RuntimeWorldMirror RuntimeWorld::world_mirror() const {
  RuntimeWorldMirror mirror;
  if (impl_ == nullptr || impl_->module == nullptr || impl_->state == nullptr) {
    return mirror;
  }
  impl_->state->initialize_for_module(*impl_->module);
  mirror.state = impl_->state->world_frozen ? RuntimeWorldState::Frozen
                                            : RuntimeWorldState::Open;
  mirror.world_epoch = impl_->state->world_epoch;
  mirror.package = package_mirror_for(*impl_->module);
  mirror.owners.reserve(impl_->module->classes.size());
  for (std::uint32_t index = 0; index < impl_->module->classes.size();
       ++index) {
    mirror.owners.push_back(
        owner_mirror_for(*impl_->module, *impl_->state, index));
  }
  return mirror;
}

RuntimeDispatchCacheStats RuntimeWorld::dispatch_cache_stats() const {
  RuntimeDispatchCacheStats stats;
  if (impl_ == nullptr || impl_->state == nullptr) {
    return stats;
  }
  stats.call_cache_entries =
      static_cast<std::uint64_t>(impl_->state->call_caches.size());
  stats.call_cache_hits = impl_->state->call_cache_hits;
  stats.call_cache_misses = impl_->state->call_cache_misses;
  stats.call_cache_updates = impl_->state->call_cache_updates;
  return stats;
}

RuntimeHeapStats RuntimeWorld::heap_stats() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  return impl_->state->heap.stats();
}

std::uint64_t RuntimeWorld::drain_remote_frees() {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->heap.drain_remote_frees();
}

std::uint64_t RuntimeWorld::drain_remote_frees(std::uint64_t worker_id) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->heap.drain_remote_frees(worker_id);
}

RuntimeWriteBarrierResult RuntimeWorld::write_barrier(const Value &owner,
                                                      const Value &value) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeWriteBarrierResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.write_barrier(owner, value);
}

RuntimeGcResult RuntimeWorld::collect_garbage(const std::vector<Value> &roots,
                                              RuntimeGcCycle cycle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  std::vector<Value> all_roots = roots;
  for (const ClassRuntimeState &klass : impl_->state->classes) {
    for (const auto &[name, value] : klass.cvars) {
      (void)name;
      if (value_has_heap_payload_tag(value)) {
        all_roots.push_back(value);
      }
    }
  }
  return impl_->state->heap.collect_garbage(all_roots, cycle);
}

void RuntimeWorld::request_garbage_collection(RuntimeGcCycle cycle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return;
  }
  impl_->state->heap.request_garbage_collection(cycle);
}

RuntimePinResult RuntimeWorld::pin(const Value &value,
                                   RuntimePinViewKind view_kind,
                                   RuntimePinPermission permissions) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimePinResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.pin(value, view_kind, permissions);
}

RuntimeUnpinResult RuntimeWorld::unpin(RuntimePinToken *token) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeUnpinResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.unpin(token);
}

std::uint64_t RuntimeWorld::pin_count(const Value &value) const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->heap.pin_count(value);
}

bool RuntimeWorld::is_pinned(const Value &value) const {
  return pin_count(value) > 0;
}

RuntimeOpaqueHandleResult
RuntimeWorld::opaque_handle_for(const RuntimePinToken &token) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeOpaqueHandleResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.opaque_handle_for(token);
}

RuntimeOpaqueHandleResult
RuntimeWorld::release_opaque_handle(RuntimeOpaqueHandle *handle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeOpaqueHandleResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.release_opaque_handle(handle);
}

RuntimeOpaqueHandleResult
RuntimeWorld::resolve_opaque_handle(const RuntimeOpaqueHandle &handle) const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeOpaqueHandleResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.resolve_opaque_handle(handle);
}

RuntimeValueBufferViewResult
RuntimeWorld::value_buffer_view(const RuntimePinToken &token) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeValueBufferViewResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.value_buffer_view(token);
}

RuntimeNativeWaitResult
RuntimeWorld::register_native_wait(const RuntimePinToken &token) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeNativeWaitResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.register_native_wait(token);
}

RuntimeNativeWaitResult
RuntimeWorld::cancel_native_wait(RuntimeNativeWaitHandle *handle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeNativeWaitResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.cancel_native_wait(handle);
}

RuntimeNativeWaitResult
RuntimeWorld::poll_native_wait(const RuntimeNativeWaitHandle &handle) const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeNativeWaitResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.poll_native_wait(handle);
}

RuntimeNativeWaitResult
RuntimeWorld::finish_native_wait(RuntimeNativeWaitHandle *handle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeNativeWaitResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.finish_native_wait(handle);
}

std::string value_to_debug_string(const Value &value,
                                  const bytecode::BcModule *module) {
  if (value.is_null()) {
    return "null";
  }
  if (value.is_bool()) {
    return value.as_bool() ? "true" : "false";
  }
  if (value.is_integer()) {
    return std::to_string(value.as_integer());
  }
  if (value.is_float()) {
    std::ostringstream out;
    out << value.as_float();
    return out.str();
  }
  if (value.is_symbol()) {
    const SymbolValue symbol = value.as_symbol();
    if (module != nullptr && symbol.symbol_id < module->symbols.size()) {
      return ":" + module->symbols[symbol.symbol_id];
    }
    return ":<invalid>";
  }
  if (value.is_string()) {
    const StringValue string = value.as_string();
    if (module != nullptr && string.string_id < module->strings.size()) {
      return "\"" + module->strings[string.string_id] + "\"";
    }
    return "\"<invalid>\"";
  }
  if (value.is_class_object()) {
    const ClassObjectValue klass = value.as_class_object();
    std::ostringstream out;
    out << "<class";
    if (module != nullptr && klass.class_index < module->classes.size()) {
      const std::uint32_t symbol_id =
          module->classes[klass.class_index].class_name_sym_id;
      if (symbol_id < module->symbols.size()) {
        out << " " << module->symbols[symbol_id];
      } else {
        out << " #" << klass.class_index;
      }
    } else {
      out << " #" << klass.class_index;
    }
    out << ">";
    return out.str();
  }
  if (value.is_closure()) {
    const std::shared_ptr<ClosureValue> closure = value.as_closure();
    std::ostringstream out;
    if (closure == nullptr) {
      out << "<closure null>";
      return out.str();
    }
    const std::string lifecycle = lifecycle_debug_label(closure->header);
    if (!lifecycle.empty()) {
      out << "<" << lifecycle << " closure>";
      return out.str();
    }
    out << "<closure c" << closure->code_id << ">";
    return out.str();
  }
  if (value.is_instance_object()) {
    const std::shared_ptr<InstanceValue> instance = value.as_instance_object();
    std::ostringstream out;
    out << "<instance";
    if (instance == nullptr) {
      out << " null>";
      return out.str();
    }
    const std::string lifecycle = lifecycle_debug_label(instance->header);
    if (!lifecycle.empty()) {
      out << " " << lifecycle << ">";
      return out.str();
    }
    if (module != nullptr && instance->class_index < module->classes.size()) {
      const std::uint32_t symbol_id =
          module->classes[instance->class_index].class_name_sym_id;
      if (symbol_id < module->symbols.size()) {
        out << " " << module->symbols[symbol_id];
      } else {
        out << " #" << instance->class_index;
      }
    } else {
      out << " #" << instance->class_index;
    }
    out << ">";
    return out.str();
  }
  if (value.is_list()) {
    const std::shared_ptr<ListValue> list = value.as_list();
    if (list == nullptr) {
      return "[<null-list>]";
    }
    const std::string lifecycle = lifecycle_debug_label(list->header);
    if (!lifecycle.empty()) {
      return "[<" + lifecycle + "-list>]";
    }
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < list->items.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << value_to_debug_string(list->items[i], module);
    }
    out << "]";
    return out.str();
  }
  if (value.is_tuple()) {
    const std::shared_ptr<TupleValue> tuple = value.as_tuple();
    if (tuple == nullptr) {
      return "(<null-tuple>)";
    }
    const std::string lifecycle = lifecycle_debug_label(tuple->header);
    if (!lifecycle.empty()) {
      return "(<" + lifecycle + "-tuple>)";
    }
    std::ostringstream out;
    out << "(";
    for (std::size_t i = 0; i < tuple->items.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << value_to_debug_string(tuple->items[i], module);
    }
    out << ")";
    return out.str();
  }
  if (value.is_map()) {
    const std::shared_ptr<MapValue> map = value.as_map();
    if (map == nullptr) {
      return "{<null-map>}";
    }
    const std::string lifecycle = lifecycle_debug_label(map->header);
    if (!lifecycle.empty()) {
      return "{<" + lifecycle + "-map>}";
    }
    std::ostringstream out;
    out << "{";
    for (std::size_t i = 0; i < map->entries.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      if (module != nullptr &&
          map->entries[i].symbol_id < module->symbols.size()) {
        out << module->symbols[map->entries[i].symbol_id];
      } else {
        out << "#" << map->entries[i].symbol_id;
      }
      out << ": " << value_to_debug_string(map->entries[i].value, module);
    }
    out << "}";
    return out.str();
  }
  return "<unknown>";
}

} // namespace amber::runtime
