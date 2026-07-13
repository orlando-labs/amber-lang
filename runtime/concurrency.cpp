#include "runtime/concurrency.h"

#include "runtime/context.h"
#include "runtime/heap.h"
#include "runtime/io.h"
#include "runtime/objects.h"
#include "runtime/reactor.h"
#include "runtime/stdlib_regexp.h"
#include "runtime/text.h"
#include "runtime/watch.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace amber::runtime {

namespace {

std::atomic<std::uint64_t> g_runtime_sync_owner_id{1};

struct RuntimeSyncBoundaryError {
  std::string error_name;
  std::string message;
};

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
  if (value.is_io_value()) {
    const std::shared_ptr<RuntimeIoValue> io_value = value.as_io_value();
    if (io_value == nullptr) {
      return RuntimeSyncBoundaryError{"TypeError", "IO value is null"};
    }
    if (!io_value->shareable()) {
      return RuntimeSyncBoundaryError{"IsolationError",
                                      "IO value is strand-confined"};
    }
    return std::nullopt;
  }
  if (value.is_watch_cell()) {
    const std::shared_ptr<RuntimeWatchCell> cell = value.as_watch_cell();
    if (cell == nullptr) {
      return RuntimeSyncBoundaryError{"TypeError", "watch cell is null"};
    }
    return runtime_value_shareability_error_impl(cell->read(), visited);
  }
  if (value.is_watch_handle()) {
    const std::shared_ptr<RuntimeWatchHandle> handle = value.as_watch_handle();
    if (handle == nullptr || handle->cell() == nullptr) {
      return std::nullopt;
    }
    return runtime_value_shareability_error_impl(handle->cell()->read(),
                                                 visited);
  }
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
    const IntrusivePtr<ClosureValue> closure = value.as_closure();
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
    const IntrusivePtr<ListValue> list = value.as_list();
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
    const IntrusivePtr<TupleValue> tuple = value.as_tuple();
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

  if (value.is_set()) {
    const IntrusivePtr<SetValue> set = value.as_set();
    if (set == nullptr) {
      return RuntimeSyncBoundaryError{"TypeError", "set value is null"};
    }
    if (!set->frozen) {
      return runtime_isolation_error();
    }
    for (const Value &item : set->items) {
      std::optional<RuntimeSyncBoundaryError> error =
          runtime_value_shareability_error_impl(item, visited);
      if (error.has_value()) {
        return error;
      }
    }
    return std::nullopt;
  }

  if (value.is_map()) {
    const IntrusivePtr<MapValue> map = value.as_map();
    if (map == nullptr) {
      return RuntimeSyncBoundaryError{"TypeError", "map value is null"};
    }
    if (!map->frozen) {
      return runtime_isolation_error();
    }
    for (const MapEntry &entry : map->entries) {
      std::optional<RuntimeSyncBoundaryError> error =
          runtime_value_shareability_error_impl(entry.key, visited);
      if (error.has_value()) {
        return error;
      }
      error = runtime_value_shareability_error_impl(entry.value, visited);
      if (error.has_value()) {
        return error;
      }
    }
    return std::nullopt;
  }

  if (value.is_instance_object()) {
    const IntrusivePtr<InstanceValue> instance = value.as_instance_object();
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
  if (value.is_watch_cell()) {
    const std::shared_ptr<RuntimeWatchCell> cell = value.as_watch_cell();
    if (cell != nullptr) {
      children->push_back(cell->read());
    }
    return;
  }
  if (value.is_watch_handle()) {
    const std::shared_ptr<RuntimeWatchHandle> handle = value.as_watch_handle();
    if (handle != nullptr && handle->cell() != nullptr) {
      children->push_back(handle->cell()->read());
    }
    return;
  }
  if (value.is_closure()) {
    const IntrusivePtr<ClosureValue> closure = value.as_closure();
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
    const IntrusivePtr<ListValue> list = value.as_list();
    if (list != nullptr) {
      children->insert(children->end(), list->items.begin(), list->items.end());
    }
    return;
  }
  if (value.is_tuple()) {
    const IntrusivePtr<TupleValue> tuple = value.as_tuple();
    if (tuple != nullptr) {
      children->insert(children->end(), tuple->items.begin(),
                       tuple->items.end());
    }
    return;
  }
  if (value.is_set()) {
    const IntrusivePtr<SetValue> set = value.as_set();
    if (set != nullptr) {
      children->insert(children->end(), set->items.begin(), set->items.end());
    }
    return;
  }
  if (value.is_map()) {
    const IntrusivePtr<MapValue> map = value.as_map();
    if (map != nullptr) {
      for (const MapEntry &entry : map->entries) {
        children->push_back(entry.key);
        children->push_back(entry.value);
      }
    }
    return;
  }
  if (value.is_instance_object()) {
    const IntrusivePtr<InstanceValue> instance = value.as_instance_object();
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
  if (tls_runtime_sync_owner_id != 0) {
    return (tls_runtime_sync_owner_id << 2U) | 1U;
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
      result.error_name = "OwnershipError";
      result.message = "mutex is not locked";
      return result;
    }
    if (owner_id_ != owner_id) {
      result.error_name = "OwnershipError";
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

  RuntimeMutexResult synchronize(std::function<Value()> function,
                                 std::chrono::milliseconds timeout) {
    RuntimeMutexResult acquired = lock(timeout);
    if (!acquired.ok) {
      return acquired;
    }

    if (!function) {
      RuntimeMutexResult released = unlock();
      RuntimeMutexResult result;
      result.error_name = released.ok ? "ArgumentError" : released.error_name;
      result.message =
          released.ok ? "mutex synchronize block is missing" : released.message;
      return result;
    }

    try {
      Value value = function();
      RuntimeMutexResult released = unlock();
      if (!released.ok) {
        released.value = std::move(value);
        return released;
      }

      RuntimeMutexResult result;
      result.ok = true;
      result.locked = true;
      result.unlocked = true;
      result.value = std::move(value);
      return result;
    } catch (...) {
      (void)unlock();
      throw;
    }
  }

  bool locked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return locked_;
  }

  bool owned() const {
    const std::uint64_t owner_id = runtime_sync_owner_id();
    std::lock_guard<std::mutex> lock(mutex_);
    return locked_ && owner_id_ == owner_id;
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

RuntimeMutexResult
RuntimeMutex::synchronize(std::function<Value()> function,
                          std::chrono::milliseconds timeout) {
  return impl_->synchronize(std::move(function), timeout);
}

bool RuntimeMutex::locked() const { return impl_->locked(); }

bool RuntimeMutex::owned() const { return impl_->owned(); }

RuntimeMutexStats RuntimeMutex::stats() const { return impl_->stats(); }

class RuntimeAtomic::Impl {
public:
  explicit Impl(Value value) : value_(std::move(value)) {
    RuntimeAtomic::Result compatibility = validate_payload(value_);
    if (!compatibility.ok) {
      throw RuntimeTaskFailure(compatibility.error_name, compatibility.message);
    }
  }

  Value get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    Value value = value_;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return value;
  }

  RuntimeAtomic::Result set(Value value) {
    RuntimeAtomic::Result compatibility = validate_payload(value);
    if (!compatibility.ok) {
      return compatibility;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    value_ = std::move(value);
    RuntimeAtomic::Result result;
    result.ok = true;
    result.updated = true;
    result.value = value_;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return result;
  }

  RuntimeAtomic::Result compare_and_set(const Value &expected, Value desired) {
    RuntimeAtomic::Result compatibility = validate_payload(desired);
    if (!compatibility.ok) {
      return compatibility;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    RuntimeAtomic::Result result;
    result.ok = true;
    if (atomic_value_equals(value_, expected)) {
      value_ = std::move(desired);
      result.matched = true;
      result.updated = true;
    }
    result.value = value_;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return result;
  }

  RuntimeAtomic::Result
  update(const std::function<Value(const Value &)> &function) {
    if (!function) {
      RuntimeAtomic::Result result;
      result.error_name = "ArgumentError";
      result.message = "atomic update block is missing";
      return result;
    }

    std::uint64_t attempts = 0;
    while (true) {
      ++attempts;
      Value current = get();
      Value replacement = function(current);
      RuntimeAtomic::Result result =
          compare_and_set(current, std::move(replacement));
      result.attempts = attempts;
      if (!result.ok || result.matched) {
        return result;
      }
      std::this_thread::yield();
    }
  }

private:
  static RuntimeAtomic::Result atomic_compatibility_error(
      const std::string &message = "atomic payload must be atomic-compatible") {
    RuntimeAtomic::Result result;
    result.error_name = "AtomicCompatibilityError";
    result.message = message;
    return result;
  }

  static RuntimeAtomic::Result validate_payload(const Value &value) {
    if (value.is_null() || value.is_bool() || value.is_integer() ||
        value.is_symbol() || value.is_class_object()) {
      RuntimeAtomic::Result result;
      result.ok = true;
      return result;
    }
    if (value.is_float() || value.is_string()) {
      return atomic_compatibility_error();
    }

    const std::optional<RuntimeSyncBoundaryError> shareability_error =
        runtime_value_shareability_error(value);
    if (shareability_error.has_value()) {
      return atomic_compatibility_error();
    }

    RuntimeAtomic::Result result;
    result.ok = true;
    return result;
  }

  static bool atomic_value_equals(const Value &lhs, const Value &rhs) {
    if (lhs.kind_index() != rhs.kind_index()) {
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
    if (lhs.is_native_type()) {
      return lhs.as_native_type().kind == rhs.as_native_type().kind;
    }
    if (lhs.is_closure()) {
      return lhs.as_closure() == rhs.as_closure();
    }
    if (lhs.is_task_module()) {
      return lhs.as_task_module() == rhs.as_task_module();
    }
    if (lhs.is_task_handle()) {
      return lhs.as_task_handle() == rhs.as_task_handle();
    }
    if (lhs.is_channel()) {
      return lhs.as_channel() == rhs.as_channel();
    }
    if (lhs.is_mutex()) {
      return lhs.as_mutex() == rhs.as_mutex();
    }
    if (lhs.is_atomic()) {
      return lhs.as_atomic() == rhs.as_atomic();
    }
    if (lhs.is_barrier()) {
      return lhs.as_barrier() == rhs.as_barrier();
    }
    if (lhs.is_flow_module()) {
      return lhs.as_flow_module() == rhs.as_flow_module();
    }
    if (lhs.is_threaded_collection()) {
      return lhs.as_threaded_collection() == rhs.as_threaded_collection();
    }
    if (lhs.is_text_writer()) {
      return lhs.as_text_writer() == rhs.as_text_writer();
    }
    if (lhs.is_logger()) {
      return lhs.as_logger() == rhs.as_logger();
    }
    if (lhs.is_io_value()) {
      return lhs.as_io_value() == rhs.as_io_value();
    }
    if (lhs.is_uuid()) {
      const std::shared_ptr<RuntimeUuidValue> left = lhs.as_uuid();
      const std::shared_ptr<RuntimeUuidValue> right = rhs.as_uuid();
      if (left == right) {
        return true;
      }
      return left != nullptr && right != nullptr && left->bytes == right->bytes;
    }
    if (lhs.is_regexp_pattern()) {
      const std::shared_ptr<RuntimeRegexpPatternValue> left =
          lhs.as_regexp_pattern();
      const std::shared_ptr<RuntimeRegexpPatternValue> right =
          rhs.as_regexp_pattern();
      if (left == right) {
        return true;
      }
      return left != nullptr && right != nullptr &&
             left->source == right->source && left->flags == right->flags;
    }
    if (lhs.is_regexp_match()) {
      return lhs.as_regexp_match() == rhs.as_regexp_match();
    }
    if (lhs.is_foreign_handle()) {
      return lhs.as_foreign_handle() == rhs.as_foreign_handle();
    }
    if (lhs.is_ast_node()) {
      return lhs.as_ast_node() == rhs.as_ast_node();
    }
    if (lhs.is_time()) {
      const std::shared_ptr<RuntimeTimeValue> left = lhs.as_time();
      const std::shared_ptr<RuntimeTimeValue> right = rhs.as_time();
      if (left == right) {
        return true;
      }
      return left != nullptr && right != nullptr &&
             left->epoch_seconds == right->epoch_seconds &&
             left->nanosecond == right->nanosecond;
    }
    if (lhs.is_time_zone()) {
      const std::shared_ptr<RuntimeTimeZoneValue> left = lhs.as_time_zone();
      const std::shared_ptr<RuntimeTimeZoneValue> right = rhs.as_time_zone();
      if (left == right) {
        return true;
      }
      return left != nullptr && right != nullptr &&
             left->offset_seconds == right->offset_seconds &&
             left->name == right->name &&
             left->fixed_offset == right->fixed_offset;
    }
    if (lhs.is_time_period()) {
      const std::shared_ptr<RuntimeTimePeriodValue> left = lhs.as_time_period();
      const std::shared_ptr<RuntimeTimePeriodValue> right =
          rhs.as_time_period();
      if (left == right) {
        return true;
      }
      return left != nullptr && right != nullptr &&
             left->months == right->months && left->days == right->days &&
             left->nanoseconds == right->nanoseconds;
    }
    if (lhs.is_instance_object()) {
      return lhs.as_instance_object() == rhs.as_instance_object();
    }
    if (lhs.is_list()) {
      return lhs.as_list() == rhs.as_list();
    }
    if (lhs.is_tuple()) {
      return lhs.as_tuple() == rhs.as_tuple();
    }
    if (lhs.is_set()) {
      return lhs.as_set() == rhs.as_set();
    }
    if (lhs.is_map()) {
      return lhs.as_map() == rhs.as_map();
    }
    return false;
  }

  mutable std::mutex mutex_;
  Value value_;
};

RuntimeAtomic::RuntimeAtomic(std::int64_t value)
    : impl_(std::make_shared<Impl>(Value::integer(value))) {}

RuntimeAtomic::RuntimeAtomic(Value value)
    : impl_(std::make_shared<Impl>(std::move(value))) {}

RuntimeAtomic::RuntimeAtomic(RuntimeAtomic &&) noexcept = default;

RuntimeAtomic &RuntimeAtomic::operator=(RuntimeAtomic &&) noexcept = default;

RuntimeAtomic::~RuntimeAtomic() = default;

std::int64_t RuntimeAtomic::get() const { return get_value().as_integer(); }

void RuntimeAtomic::set(std::int64_t value) {
  (void)impl_->set(Value::integer(value));
}

bool RuntimeAtomic::compare_and_set(std::int64_t expected,
                                    std::int64_t desired) {
  return impl_
      ->compare_and_set(Value::integer(expected), Value::integer(desired))
      .matched;
}

Value RuntimeAtomic::get_value() const { return impl_->get(); }

RuntimeAtomic::Result RuntimeAtomic::set_value(Value value) {
  return impl_->set(std::move(value));
}

RuntimeAtomic::Result
RuntimeAtomic::compare_and_set_value(const Value &expected, Value desired) {
  return impl_->compare_and_set(expected, std::move(desired));
}

RuntimeAtomic::Result
RuntimeAtomic::update(std::function<Value(const Value &)> function) {
  return impl_->update(function);
}

class RuntimeBarrier::Impl {
public:
  explicit Impl(std::size_t parties) : parties_(parties) {
    if (parties_ == 0) {
      throw RuntimeTaskFailure("ArgumentError",
                               "barrier parties must be positive");
    }
    stats_.parties = static_cast<std::uint64_t>(parties_);
  }

  RuntimeBarrierResult wait(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t generation = generation_;
    ++waiting_;
    ++stats_.arrivals;
    stats_.waiting = static_cast<std::uint64_t>(waiting_);

    if (waiting_ == parties_) {
      return release_generation_locked(generation, true);
    }

    const std::optional<std::chrono::steady_clock::time_point> deadline =
        runtime_sync_deadline(timeout);
    while (generation_ == generation) {
      if (current_runtime_task_cancel_requested()) {
        remove_waiter_locked();
        ++stats_.cancellations;
        cv_.notify_all();

        RuntimeBarrierResult result = base_result_locked();
        result.cancelled = true;
        result.error_name = "CancelledError";
        result.message = "barrier wait cancelled";
        return result;
      }
      if (runtime_sync_deadline_expired(deadline)) {
        remove_waiter_locked();
        ++stats_.timeouts;
        cv_.notify_all();

        RuntimeBarrierResult result = base_result_locked();
        result.timed_out = true;
        result.error_name = "TimeoutError";
        result.message = "barrier wait timed out";
        return result;
      }

      const std::chrono::steady_clock::duration wait_duration =
          runtime_sync_wait_duration(deadline);
      if (wait_duration <= std::chrono::steady_clock::duration::zero()) {
        continue;
      }
      cv_.wait_for(lock, wait_duration);
    }

    RuntimeBarrierResult result = base_result_locked();
    result.ok = true;
    result.passed = true;
    result.generation = generation_ - 1;
    return result;
  }

  RuntimeBarrierStats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeBarrierStats out = stats_;
    out.parties = static_cast<std::uint64_t>(parties_);
    out.waiting = static_cast<std::uint64_t>(waiting_);
    out.generation = generation_;
    return out;
  }

private:
  RuntimeBarrierResult release_generation_locked(std::uint64_t generation,
                                                 bool last) {
    waiting_ = 0;
    ++generation_;
    ++stats_.passes;
    stats_.waiting = 0;
    stats_.generation = generation_;
    cv_.notify_all();

    RuntimeBarrierResult result = base_result_locked();
    result.ok = true;
    result.passed = true;
    result.last = last;
    result.generation = generation;
    return result;
  }

  RuntimeBarrierResult base_result_locked() const {
    RuntimeBarrierResult result;
    result.parties = static_cast<std::uint64_t>(parties_);
    result.waiting = static_cast<std::uint64_t>(waiting_);
    result.generation = generation_;
    return result;
  }

  void remove_waiter_locked() {
    if (waiting_ > 0) {
      --waiting_;
    }
    stats_.waiting = static_cast<std::uint64_t>(waiting_);
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t parties_ = 0;
  std::size_t waiting_ = 0;
  std::uint64_t generation_ = 0;
  RuntimeBarrierStats stats_;
};

RuntimeBarrier::RuntimeBarrier(std::size_t parties)
    : impl_(std::make_shared<Impl>(parties)) {}

RuntimeBarrier::RuntimeBarrier(RuntimeBarrier &&) noexcept = default;

RuntimeBarrier &RuntimeBarrier::operator=(RuntimeBarrier &&) noexcept = default;

RuntimeBarrier::~RuntimeBarrier() = default;

RuntimeBarrierResult RuntimeBarrier::wait(std::chrono::milliseconds timeout) {
  return impl_->wait(timeout);
}

RuntimeBarrierStats RuntimeBarrier::stats() const { return impl_->stats(); }

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
    if (strand.state == RuntimeStrandState::Running && strand.park_pending) {
      // The strand asked to park (park_current) but worker_loop has not yet
      // transitioned it to Sleeping. Record the wake; the park transition will
      // re-enqueue it immediately rather than sleeping. mutex_ serializes this
      // against the park transition, so the wake cannot be lost either way.
      strand.park_wake_pending = true;
      return true;
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

  bool park_current(std::optional<std::chrono::milliseconds> wake_after) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t id = current_runtime_task_id();
    if (id == 0) {
      return false;
    }
    auto found = strands_.find(id);
    if (found == strands_.end() ||
        found->second.state != RuntimeStrandState::Running) {
      return false;
    }
    found->second.park_pending = true;
    if (wake_after.has_value()) {
      found->second.park_wake_deadline =
          std::chrono::steady_clock::now() + *wake_after;
    } else {
      found->second.park_wake_deadline.reset();
    }
    return true;
  }

  bool cancel_task(std::uint64_t task_id) {
    bool cancelled;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancelled = request_cancel_locked(task_id);
      cv_.notify_all();
    }
    // Wake the reactor so a strand parked on a blocking IO wait observes its
    // (or an ancestor's) freshly-set cancel flag now, rather than at the next
    // safety re-scan.
    if (cancelled) {
      RuntimeReactor::instance().kick();
    }
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
    std::uint64_t sync_owner_id = 0;
    RuntimeStrandState state = RuntimeStrandState::New;
    StrandFunction function;
    std::uint64_t worker_id = 0;
    std::uint64_t wake_generation = 0;
    bool wake_pending = false;
    bool queued = false;
    // Layer B: set by park_current() while the strand is Running; consumed by
    // worker_loop after the strand function returns to park (free the worker)
    // instead of finalizing. An optional deadline schedules a timer wake.
    bool park_pending = false;
    std::optional<std::chrono::steady_clock::time_point> park_wake_deadline;
    // A wake (wake_strand) that arrived after park_current() but before
    // worker_loop transitioned the strand to Sleeping. The park transition
    // honors it by re-enqueueing immediately instead of sleeping, so an
    // external wake (e.g. a reactor IO-readiness completion) racing the park
    // is never lost.
    bool park_wake_pending = false;
    bool parked_once = false;
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
    strand.sync_owner_id =
        (g_runtime_sync_owner_id.fetch_add(1, std::memory_order_relaxed)
         << 1U) |
        1U;
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
    if (strand.parked_once) {
      ++stats_.park_resumes;
    }
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
      std::uint64_t sync_owner_id = 0;
      StrandFunction function;
      std::shared_ptr<std::atomic<bool>> cancellation_requested;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!shutdown_requested_) {
          strand_id = next_runnable_locked(worker_index);
          if (strand_id != 0) {
            function = strands_[strand_id].function;
            sync_owner_id = strands_[strand_id].sync_owner_id;
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
        RuntimeTaskScope task_scope(strand_id, cancellation_requested.get(),
                                    sync_owner_id);
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
        if (found != strands_.end() && found->second.park_pending) {
          // Layer B: the strand parked itself. Release the worker without
          // finalizing; the strand's function is re-invoked (resumed) when the
          // strand is woken by its timer or an explicit wake_strand().
          StrandRecord &strand = found->second;
          strand.park_pending = false;
          strand.parked_once = true;
          strand.worker_id = 0;
          strand.state = RuntimeStrandState::Sleeping;
          strand.wake_pending = false;
          strand.queued = false;
          ++strand.wake_generation;
          ++stats_.parks;
          if (running_count_ > 0) {
            --running_count_;
          }
          if (strand.park_wake_pending) {
            // A wake raced the park (e.g. reactor IO readiness fired before we
            // got here). Resume immediately instead of sleeping.
            strand.park_wake_pending = false;
            enqueue_runnable_locked(strand_id, std::nullopt);
          } else if (strand.park_wake_deadline.has_value()) {
            timers_.push(TimerEntry{*strand.park_wake_deadline, strand_id,
                                    strand.wake_generation});
            strand.park_wake_deadline.reset();
          }
        } else {
          if (found != strands_.end()) {
            found->second.worker_id = 0;
          }
          if (running_count_ > 0) {
            --running_count_;
          }
          finish_or_wait_locked(strand_id, completion);
        }
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

bool RuntimeScheduler::park_current(
    std::optional<std::chrono::milliseconds> wake_after) {
  return impl_->park_current(wake_after);
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

namespace {

RuntimeTaskHandleState
task_handle_state_from_strand_state(RuntimeStrandState state) {
  switch (state) {
  case RuntimeStrandState::New:
    return RuntimeTaskHandleState::New;
  case RuntimeStrandState::Runnable:
    return RuntimeTaskHandleState::Runnable;
  case RuntimeStrandState::Running:
    return RuntimeTaskHandleState::Running;
  case RuntimeStrandState::Sleeping:
    return RuntimeTaskHandleState::Sleeping;
  case RuntimeStrandState::Waiting:
    return RuntimeTaskHandleState::Waiting;
  case RuntimeStrandState::Done:
    return RuntimeTaskHandleState::Done;
  case RuntimeStrandState::Failed:
    return RuntimeTaskHandleState::Failed;
  case RuntimeStrandState::Cancelled:
    return RuntimeTaskHandleState::Cancelled;
  }
  return RuntimeTaskHandleState::Inactive;
}

bool task_handle_state_is_ready(RuntimeTaskHandleState state) {
  return state == RuntimeTaskHandleState::Done ||
         state == RuntimeTaskHandleState::Failed ||
         state == RuntimeTaskHandleState::Cancelled;
}

} // namespace

struct RuntimeTaskHandle::State {
  mutable std::mutex mutex;
  Value value = Value::null();
  bool has_value = false;
  std::string error_name;
  std::string message;
};

RuntimeTaskHandle::RuntimeTaskHandle() = default;

RuntimeTaskHandle::RuntimeTaskHandle(
    std::shared_ptr<RuntimeScheduler> scheduler, std::uint64_t task_id,
    std::shared_ptr<State> state)
    : scheduler_(std::move(scheduler)), task_id_(task_id),
      state_(std::move(state)) {}

bool RuntimeTaskHandle::active() const {
  return scheduler_ != nullptr && task_id_ != 0;
}

std::uint64_t RuntimeTaskHandle::task_id() const { return task_id_; }

std::uint64_t RuntimeTaskHandle::strand_id() const { return task_id_; }

RuntimeTaskHandleState RuntimeTaskHandle::state() const {
  return snapshot().state;
}

RuntimeTaskHandleSnapshot RuntimeTaskHandle::snapshot() const {
  RuntimeTaskHandleSnapshot out;
  out.task_id = task_id_;
  out.strand_id = strand_id();
  if (!active()) {
    out.error_name = "LifetimeError";
    out.message = "task handle is not active";
    return out;
  }

  const std::optional<RuntimeTaskSnapshot> current =
      scheduler_->task_snapshot(task_id_);
  if (!current.has_value()) {
    out.error_name = "LifetimeError";
    out.message = "task handle is not active";
    return out;
  }

  out.active = true;
  out.task_id = current->task_id;
  out.strand_id = current->task_id;
  out.state = task_handle_state_from_strand_state(current->state);
  out.ready = task_handle_state_is_ready(out.state);
  out.succeeded = out.state == RuntimeTaskHandleState::Done;
  out.failed = out.state == RuntimeTaskHandleState::Failed;
  out.cancelled = out.state == RuntimeTaskHandleState::Cancelled;
  out.running = out.state == RuntimeTaskHandleState::Running;
  out.cancellation_requested = current->cancellation_requested;
  out.error_name = current->error_name;
  out.message = current->message;
  return out;
}

RuntimeTaskPublicResult
RuntimeTaskHandle::wait(std::chrono::milliseconds timeout) const {
  RuntimeTaskPublicResult out;
  if (!active()) {
    out.error_name = "LifetimeError";
    out.message = "task handle is not active";
    return out;
  }

  const RuntimeTaskJoinResult joined = scheduler_->join_task(task_id_, timeout);
  out.ready = joined.joined;
  out.timed_out = joined.timed_out;
  out.cancelled = joined.cancelled;
  out.failed = joined.joined && !joined.ok && !joined.cancelled;
  out.state = task_handle_state_from_strand_state(joined.state);
  out.error_name = joined.error_name;
  out.message = joined.message;
  if (!joined.ok) {
    return out;
  }

  out.ok = true;
  out.ready = true;
  if (state_ != nullptr) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    out.value = state_->has_value ? state_->value : Value::null();
  }
  return out;
}

RuntimeTaskPublicResult RuntimeTaskHandle::result() const {
  RuntimeTaskPublicResult out;
  const RuntimeTaskHandleSnapshot current = snapshot();
  out.state = current.state;
  if (!current.active) {
    out.error_name =
        current.error_name.empty() ? "LifetimeError" : current.error_name;
    out.message =
        current.message.empty() ? "task handle is not active" : current.message;
    return out;
  }

  if (current.state == RuntimeTaskHandleState::Done) {
    out.ok = true;
    out.ready = true;
    if (state_ != nullptr) {
      std::lock_guard<std::mutex> lock(state_->mutex);
      out.value = state_->has_value ? state_->value : Value::null();
    }
    return out;
  }

  if (current.state == RuntimeTaskHandleState::Failed) {
    out.ready = true;
    out.failed = true;
    out.error_name =
        current.error_name.empty() ? "TaskFailedError" : current.error_name;
    out.message = current.message.empty() ? "task failed" : current.message;
    return out;
  }

  if (current.state == RuntimeTaskHandleState::Cancelled) {
    out.ready = true;
    out.cancelled = true;
    out.error_name = "CancelledError";
    out.message = current.message.empty() ? "task cancelled" : current.message;
    return out;
  }

  out.error_name = "TaskNotDoneError";
  out.message = "task is not done";
  return out;
}

RuntimeTaskFailureInfo RuntimeTaskHandle::failure() const {
  RuntimeTaskFailureInfo out;
  const RuntimeTaskHandleSnapshot current = snapshot();
  out.state = current.state;
  if (!current.active) {
    out.ready = true;
    out.failed = true;
    out.error_name =
        current.error_name.empty() ? "LifetimeError" : current.error_name;
    out.message =
        current.message.empty() ? "task handle is not active" : current.message;
    return out;
  }

  if (current.state == RuntimeTaskHandleState::Failed) {
    out.ready = true;
    out.failed = true;
    out.error_name =
        current.error_name.empty() ? "RuntimeError" : current.error_name;
    out.message = current.message.empty() ? "task failed" : current.message;
    return out;
  }
  if (current.state == RuntimeTaskHandleState::Cancelled) {
    out.ready = true;
    out.cancelled = true;
    out.error_name = "CancelledError";
    out.message = current.message.empty() ? "task cancelled" : current.message;
    return out;
  }
  if (current.state == RuntimeTaskHandleState::Done) {
    out.ready = true;
    return out;
  }
  out.error_name = "TaskNotDoneError";
  out.message = "task is not done";
  return out;
}

bool RuntimeTaskHandle::cancel() const {
  return active() && scheduler_->cancel_task(task_id_);
}

bool RuntimeTaskHandle::resume() const {
  return active() && scheduler_->wake_strand(task_id_);
}

bool RuntimeTaskHandle::cancelled() const {
  if (!active()) {
    return false;
  }
  const std::optional<RuntimeTaskSnapshot> snapshot =
      scheduler_->task_snapshot(task_id_);
  return snapshot.has_value() &&
         (snapshot->state == RuntimeStrandState::Cancelled ||
          snapshot->cancellation_requested);
}

bool RuntimeTaskHandle::done() const {
  if (!active()) {
    return false;
  }
  const std::optional<RuntimeTaskSnapshot> snapshot =
      scheduler_->task_snapshot(task_id_);
  return snapshot.has_value() &&
         (snapshot->state == RuntimeStrandState::Done ||
          snapshot->state == RuntimeStrandState::Failed ||
          snapshot->state == RuntimeStrandState::Cancelled);
}

bool RuntimeTaskHandle::running() const {
  if (!active()) {
    return false;
  }
  const std::optional<RuntimeTaskSnapshot> snapshot =
      scheduler_->task_snapshot(task_id_);
  return snapshot.has_value() && snapshot->state == RuntimeStrandState::Running;
}

bool RuntimeTaskHandle::failed() const {
  if (!active()) {
    return false;
  }
  const std::optional<RuntimeTaskSnapshot> snapshot =
      scheduler_->task_snapshot(task_id_);
  return snapshot.has_value() && snapshot->state == RuntimeStrandState::Failed;
}
namespace {

// Layer B cooperative suspension. g_amber_task_parked is a same-thread
// handshake set by a parked VM/task driver and observed by the task body so the
// body returns without recording completion (the scheduler re-invokes it to
// resume). g_amber_cooperative_parks is the monotonic observability counter
// exposed by runtime_cooperative_task_park_count().
thread_local bool g_amber_task_parked = false;
std::atomic<std::uint64_t> g_amber_cooperative_parks{0};

} // namespace

void runtime_clear_task_parked() { g_amber_task_parked = false; }

bool runtime_task_is_parked() { return g_amber_task_parked; }

void runtime_mark_task_parked() {
  g_amber_task_parked = true;
  g_amber_cooperative_parks.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t runtime_cooperative_task_park_count() {
  return g_amber_cooperative_parks.load();
}

RuntimeTaskModule::RuntimeTaskModule(std::size_t worker_count)
    : RuntimeTaskModule(RuntimeSchedulerConfig{worker_count, 1}) {}

RuntimeTaskModule::RuntimeTaskModule(RuntimeSchedulerConfig config)
    : scheduler_(std::make_shared<RuntimeScheduler>(config)) {}

RuntimeTaskHandle RuntimeTaskModule::async(TaskFunction function) {
  return spawn_with_kind(SpawnKind::SameStrand, std::move(function));
}

RuntimeTaskHandle RuntimeTaskModule::spawn(TaskFunction function) {
  return spawn_with_kind(SpawnKind::NewStrand, std::move(function));
}

Value RuntimeTaskModule::sync(TaskFunction function) const {
  if (!function) {
    throw RuntimeTaskFailure("ArgumentError", "task sync block is missing");
  }
  throw_if_runtime_task_cancelled();
  RuntimeTaskSyncScope sync_scope;
  Value value = function();
  throw_if_runtime_task_cancelled();
  return value;
}

bool RuntimeTaskModule::sync_active() const {
  return current_runtime_task_sync_active();
}

void RuntimeTaskModule::sleep(std::chrono::milliseconds duration) const {
  throw_if_runtime_task_cancelled();
  if (duration.count() > 0) {
    std::this_thread::sleep_for(duration);
  } else if (!current_runtime_task_sync_active()) {
    std::this_thread::yield();
  }
  throw_if_runtime_task_cancelled();
}

void RuntimeTaskModule::yield_current() const {
  if (!current_runtime_task_sync_active()) {
    std::this_thread::yield();
  }
  throw_if_runtime_task_cancelled();
}

RuntimeScheduler &RuntimeTaskModule::scheduler() { return *scheduler_; }

const RuntimeScheduler &RuntimeTaskModule::scheduler() const {
  return *scheduler_;
}

RuntimeTaskHandle RuntimeTaskModule::spawn_with_kind(SpawnKind kind,
                                                     TaskFunction function) {
  auto state = std::make_shared<RuntimeTaskHandle::State>();
  const std::string inherited_annotation = current_runtime_task_annotation();
  auto task_body = [state, function = std::move(function),
                    inherited_annotation]() mutable {
    RuntimeTaskAnnotationScope annotation_scope(inherited_annotation);
    runtime_clear_task_parked();
    try {
      const Value value = function ? function() : Value::null();
      if (runtime_task_is_parked()) {
        // Layer B: the task body parked itself at a suspension point. Do not
        // record completion; worker_loop leaves the strand parked and the
        // scheduler re-invokes this body (resuming the same VM) when woken.
        runtime_clear_task_parked();
        return;
      }
      std::lock_guard<std::mutex> lock(state->mutex);
      state->value = value;
      state->has_value = true;
    } catch (const RuntimeTaskFailure &failure) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error_name = failure.error_name();
      state->message = failure.message();
      throw;
    } catch (const RuntimeTaskCancelled &) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error_name = "CancelledError";
      state->message = "task cancelled";
      throw;
    } catch (const std::exception &error) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error_name = "RuntimeError";
      state->message = error.what();
      throw;
    } catch (...) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error_name = "RuntimeError";
      state->message = "task failed";
      throw;
    }
  };

  const std::uint64_t task_id =
      kind == SpawnKind::SameStrand
          ? scheduler_->spawn_task(std::move(task_body))
          : scheduler_->spawn_strand(std::move(task_body));
  return RuntimeTaskHandle(scheduler_, task_id, std::move(state));
}

class RuntimeFlowModule::Impl {
public:
  explicit Impl(std::size_t worker_count) : task_(worker_count) {}

  explicit Impl(RuntimeSchedulerConfig config) : task_(config) {}

  RuntimeFlowGatherResult gather(std::vector<RuntimeTaskHandle> handles,
                                 RuntimeFlowOptions options) {
    RuntimeFlowOptions normalized = normalize_options(options, handles.size());
    RuntimeFlowGatherResult result;
    result.values.resize(handles.size(), Value::null());
    record_gather_started();

    if (handles.empty()) {
      result.ok = true;
      return result;
    }

    const std::optional<std::chrono::steady_clock::time_point> deadline =
        runtime_sync_deadline(normalized.timeout);
    std::vector<bool> done(handles.size(), false);
    std::vector<Value> completion_order;
    std::uint64_t remaining = static_cast<std::uint64_t>(handles.size());

    while (remaining != 0) {
      if (current_runtime_task_cancel_requested()) {
        cancel_unfinished(handles, done);
        result.cancelled = true;
        result.error_name = "CancelledError";
        result.message = "flow gather cancelled";
        record_gather_finished(result);
        return result;
      }
      if (runtime_sync_deadline_expired(deadline)) {
        cancel_unfinished(handles, done);
        result.timed_out = true;
        result.error_name = "TimeoutError";
        result.message = "flow gather timed out";
        record_gather_finished(result);
        return result;
      }

      bool progressed = false;
      for (std::size_t index = 0; index < handles.size(); ++index) {
        if (done[index]) {
          continue;
        }

        RuntimeTaskPublicResult task_result = handles[index].result();
        if (task_result.error_name == "TaskNotDoneError") {
          continue;
        }

        done[index] = true;
        --remaining;
        progressed = true;
        if (!accept_task_result(index, std::move(task_result), normalized,
                                &result, &completion_order)) {
          cancel_unfinished(handles, done);
          record_gather_finished(result);
          return result;
        }
      }

      if (!progressed) {
        const std::chrono::steady_clock::duration wait_duration =
            runtime_sync_wait_duration(deadline);
        if (wait_duration > std::chrono::steady_clock::duration::zero()) {
          std::this_thread::sleep_for(wait_duration);
        } else {
          std::this_thread::yield();
        }
      }
    }

    if (!normalized.ordered) {
      result.values = std::move(completion_order);
    } else if (normalized.failure_policy == RuntimeFlowFailurePolicy::Ignore) {
      std::vector<Value> compacted;
      compacted.reserve(static_cast<std::size_t>(result.completed_count));
      for (std::size_t index = 0; index < done.size(); ++index) {
        if (result_value_completed(index, result.failures)) {
          compacted.push_back(result.values[index]);
        }
      }
      result.values = std::move(compacted);
    }

    result.ok = true;
    result.failed = !result.failures.empty();
    result.failed_count = static_cast<std::uint64_t>(result.failures.size());
    record_gather_finished(result);
    return result;
  }

  RuntimeFlowGatherResult scatter(std::vector<Value> partitions,
                                  MapFunction function,
                                  RuntimeFlowOptions options) {
    return scatter_map(std::move(partitions), std::move(function), options);
  }

  RuntimeFlowGatherResult scatter_map(std::vector<Value> items,
                                      MapFunction function,
                                      RuntimeFlowOptions options) {
    RuntimeFlowOptions normalized = normalize_options(options, items.size());
    if (!function) {
      return argument_error("flow scatter_map block is missing");
    }

    RuntimeFlowGatherResult validation =
        validate_boundary_values(items, normalized, "flow partition");
    if (!validation.ok && !validation.error_name.empty()) {
      record_gather_finished(validation);
      return validation;
    }

    if (normalized.partition_policy != RuntimeFlowPartitionPolicy::Items) {
      return scatter_map_partitioned(std::move(items), std::move(function),
                                     normalized);
    }

    std::vector<RuntimeTaskHandle> handles;
    handles.reserve(items.size());
    record_flow_started(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
      Value item = items[index];
      handles.push_back(task_.spawn(
          [function, item, index]() { return function(item, index); }));
    }
    return gather(std::move(handles), normalized);
  }

  RuntimeFlowReduceResult scatter_reduce(std::vector<Value> items, Value init,
                                         MapFunction map, ReduceFunction reduce,
                                         RuntimeFlowOptions options) {
    RuntimeFlowReduceResult reduced;
    if (!map) {
      reduced.error_name = "ArgumentError";
      reduced.message = "flow scatter_reduce map block is missing";
      return reduced;
    }
    if (!reduce) {
      reduced.error_name = "ArgumentError";
      reduced.message = "flow scatter_reduce reduce block is missing";
      return reduced;
    }

    RuntimeFlowGatherResult gathered =
        scatter_map(std::move(items), std::move(map), options);
    reduced.gather = gathered;
    if (!gathered.ok || gathered.failed) {
      reduced.failed = true;
      reduced.error_name =
          gathered.error_name.empty() ? "FlowGatherError" : gathered.error_name;
      reduced.message =
          gathered.message.empty() ? "flow gather failed" : gathered.message;
      record_reduction();
      return reduced;
    }

    Value accumulator = std::move(init);
    for (const Value &value : gathered.values) {
      accumulator = reduce(accumulator, value);
    }

    reduced.ok = true;
    reduced.value = std::move(accumulator);
    record_reduction();
    return reduced;
  }

  RuntimeFlowGatherResult broadcast(Value value, std::size_t workers,
                                    BroadcastFunction function,
                                    RuntimeFlowOptions options) {
    if (workers == 0) {
      return argument_error("flow broadcast workers must be positive");
    }
    if (!function) {
      return argument_error("flow broadcast block is missing");
    }

    RuntimeFlowOptions normalized = options;
    normalized.workers = workers;
    std::vector<Value> values(workers, value);
    RuntimeFlowGatherResult validation =
        validate_boundary_values(values, normalized, "flow broadcast value");
    if (!validation.ok && !validation.error_name.empty()) {
      record_gather_finished(validation);
      return validation;
    }

    std::vector<RuntimeTaskHandle> handles;
    handles.reserve(workers);
    record_broadcast_started(workers);
    for (std::size_t index = 0; index < workers; ++index) {
      Value worker_value = value;
      handles.push_back(task_.spawn([function, worker_value, index]() {
        return function(worker_value, index);
      }));
    }
    return gather(std::move(handles), normalized);
  }

  RuntimeFlowStats stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
  }

private:
  struct PartitionedScatterState {
    std::mutex mutex;
    std::atomic<std::size_t> next_index{0};
    std::atomic<bool> stop_requested{false};
    RuntimeFlowGatherResult result;
    std::vector<bool> completed;
    std::vector<Value> completion_order;
  };

  static RuntimeFlowOptions normalize_options(RuntimeFlowOptions options,
                                              std::size_t item_count) {
    if (options.workers == 0) {
      const unsigned int hardware = std::thread::hardware_concurrency();
      options.workers = hardware == 0 ? 2 : static_cast<std::size_t>(hardware);
    }
    if (item_count != 0 && options.workers > item_count) {
      options.workers = item_count;
    }
    (void)options.partition_policy;
    return options;
  }

  static RuntimeFlowGatherResult argument_error(std::string message) {
    RuntimeFlowGatherResult result;
    result.error_name = "ArgumentError";
    result.message = std::move(message);
    return result;
  }

  RuntimeFlowGatherResult
  scatter_map_partitioned(std::vector<Value> items, MapFunction function,
                          const RuntimeFlowOptions &options) {
    const std::size_t worker_count = items.empty() ? 0 : options.workers;
    auto shared_items =
        std::make_shared<const std::vector<Value>>(std::move(items));
    auto shared_function = std::make_shared<MapFunction>(std::move(function));
    auto state = std::make_shared<PartitionedScatterState>();
    state->result.values.resize(shared_items->size(), Value::null());
    state->completed.resize(shared_items->size(), false);

    auto record_failure =
        [this, state, &options](std::size_t index, std::string error_name,
                                std::string message, bool isolation_failure) {
          if (options.failure_policy == RuntimeFlowFailurePolicy::First &&
              state->stop_requested.exchange(true, std::memory_order_relaxed)) {
            return;
          }
          if (isolation_failure) {
            record_isolation_rejection();
          }
          std::lock_guard<std::mutex> lock(state->mutex);
          RuntimeFlowFailure failure{index, std::move(error_name),
                                     std::move(message)};
          state->result.failures.push_back(failure);
          state->result.failed = true;
          if (state->result.error_name.empty()) {
            state->result.error_name = failure.error_name;
            state->result.message = failure.message;
          }
          state->result.failed_count =
              static_cast<std::uint64_t>(state->result.failures.size());
        };

    auto record_success = [state, &options, &record_failure](
                              std::size_t index, Value value) mutable {
      if (options.isolation == RuntimeFlowIsolationMode::Checked) {
        std::optional<RuntimeSyncBoundaryError> shareability_error =
            runtime_value_shareability_error(value);
        if (shareability_error.has_value()) {
          record_failure(index, shareability_error->error_name,
                         shareability_error->message, true);
          return;
        }
      }

      std::lock_guard<std::mutex> lock(state->mutex);
      state->completed[index] = true;
      ++state->result.completed_count;
      if (options.ordered) {
        state->result.values[index] = std::move(value);
      } else {
        state->completion_order.push_back(std::move(value));
      }
    };

    auto process_index = [shared_items, shared_function, &record_failure,
                          &record_success](std::size_t index) mutable {
      try {
        Value item = (*shared_items)[index];
        record_success(index, (*shared_function)(item, index));
      } catch (const RuntimeTaskFailure &failure) {
        record_failure(index, failure.error_name(), failure.message(), false);
      } catch (const RuntimeTaskCancelled &) {
        throw;
      } catch (const std::exception &error) {
        record_failure(index, "RuntimeError", error.what(), false);
      } catch (...) {
        record_failure(index, "RuntimeError", "flow worker failed", false);
      }
    };

    std::vector<RuntimeTaskHandle> handles;
    handles.reserve(worker_count);
    record_flow_started(worker_count);
    for (std::size_t worker_index = 0; worker_index < worker_count;
         ++worker_index) {
      handles.push_back(
          task_.spawn([state, process_index, worker_index, worker_count,
                       item_count = shared_items->size(),
                       policy = options.partition_policy]() mutable {
            if (policy == RuntimeFlowPartitionPolicy::Atomic) {
              while (!state->stop_requested.load(std::memory_order_relaxed)) {
                throw_if_runtime_task_cancelled();
                const std::size_t index =
                    state->next_index.fetch_add(1, std::memory_order_relaxed);
                if (index >= item_count) {
                  return Value::null();
                }
                process_index(index);
              }
              return Value::null();
            }

            if (policy == RuntimeFlowPartitionPolicy::Stride) {
              for (std::size_t index = worker_index; index < item_count;
                   index += worker_count) {
                if (state->stop_requested.load(std::memory_order_relaxed)) {
                  break;
                }
                throw_if_runtime_task_cancelled();
                process_index(index);
              }
              return Value::null();
            }

            const std::size_t chunk_size =
                (item_count + worker_count - 1U) / worker_count;
            const std::size_t begin = worker_index * chunk_size;
            const std::size_t end = std::min(begin + chunk_size, item_count);
            for (std::size_t index = begin; index < end; ++index) {
              if (state->stop_requested.load(std::memory_order_relaxed)) {
                break;
              }
              throw_if_runtime_task_cancelled();
              process_index(index);
            }
            return Value::null();
          }));
    }

    RuntimeFlowGatherResult worker_result = gather(std::move(handles), options);
    if (!worker_result.ok || worker_result.failed || worker_result.timed_out ||
        worker_result.cancelled) {
      return worker_result;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    RuntimeFlowGatherResult result = std::move(state->result);
    if (!options.ordered) {
      result.values = std::move(state->completion_order);
    } else if (options.failure_policy == RuntimeFlowFailurePolicy::Ignore) {
      std::vector<Value> compacted;
      compacted.reserve(static_cast<std::size_t>(result.completed_count));
      for (std::size_t index = 0; index < state->completed.size(); ++index) {
        if (state->completed[index]) {
          compacted.push_back(std::move(result.values[index]));
        }
      }
      result.values = std::move(compacted);
    }

    result.failed = !result.failures.empty();
    result.failed_count = static_cast<std::uint64_t>(result.failures.size());
    if (result.failed && result.error_name.empty()) {
      result.error_name = result.failures.front().error_name;
      result.message = result.failures.front().message;
    }
    result.ok = !result.failed ||
                options.failure_policy != RuntimeFlowFailurePolicy::First;
    return result;
  }

  static RuntimeFlowGatherResult isolation_error(std::size_t index,
                                                 std::string boundary,
                                                 std::string error_name,
                                                 std::string message) {
    RuntimeFlowGatherResult result;
    result.failed = true;
    result.error_name = std::move(error_name);
    result.message = std::move(message);
    result.failures.push_back(
        RuntimeFlowFailure{index, result.error_name, boundary + " rejected"});
    result.failed_count = 1;
    return result;
  }

  RuntimeFlowGatherResult
  validate_boundary_values(const std::vector<Value> &values,
                           const RuntimeFlowOptions &options,
                           const std::string &boundary) {
    RuntimeFlowGatherResult ok;
    ok.ok = true;
    if (options.isolation == RuntimeFlowIsolationMode::Unchecked) {
      return ok;
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
      std::optional<RuntimeSyncBoundaryError> shareability_error =
          runtime_value_shareability_error(values[index]);
      if (shareability_error.has_value()) {
        record_isolation_rejection();
        return isolation_error(index, boundary, shareability_error->error_name,
                               shareability_error->message);
      }
    }
    return ok;
  }

  bool accept_task_result(std::size_t index,
                          RuntimeTaskPublicResult task_result,
                          const RuntimeFlowOptions &options,
                          RuntimeFlowGatherResult *result,
                          std::vector<Value> *completion_order) {
    if (task_result.ok) {
      if (options.isolation == RuntimeFlowIsolationMode::Checked) {
        std::optional<RuntimeSyncBoundaryError> shareability_error =
            runtime_value_shareability_error(task_result.value);
        if (shareability_error.has_value()) {
          record_isolation_rejection();
          RuntimeFlowFailure failure{index, shareability_error->error_name,
                                     shareability_error->message};
          result->failures.push_back(failure);
          result->failed = true;
          result->error_name = failure.error_name;
          result->message = failure.message;
          result->failed_count =
              static_cast<std::uint64_t>(result->failures.size());
          return options.failure_policy != RuntimeFlowFailurePolicy::First;
        }
      }

      ++result->completed_count;
      if (options.ordered) {
        result->values[index] = std::move(task_result.value);
      } else if (completion_order != nullptr) {
        completion_order->push_back(std::move(task_result.value));
      }
      return true;
    }

    RuntimeFlowFailure failure;
    failure.index = index;
    failure.error_name = task_result.error_name.empty()
                             ? "FlowGatherError"
                             : task_result.error_name;
    failure.message = task_result.message.empty() ? "flow worker failed"
                                                  : task_result.message;
    result->failures.push_back(failure);
    result->failed = true;
    if (task_result.cancelled) {
      ++result->cancelled_count;
    }
    if (options.failure_policy == RuntimeFlowFailurePolicy::First) {
      result->error_name = failure.error_name;
      result->message = failure.message;
      result->failed_count =
          static_cast<std::uint64_t>(result->failures.size());
      return false;
    }
    return true;
  }

  static bool
  result_value_completed(std::size_t index,
                         const std::vector<RuntimeFlowFailure> &failures) {
    return std::none_of(failures.begin(), failures.end(),
                        [index](const RuntimeFlowFailure &failure) {
                          return failure.index == index;
                        });
  }

  static void cancel_unfinished(const std::vector<RuntimeTaskHandle> &handles,
                                const std::vector<bool> &done) {
    for (std::size_t index = 0; index < handles.size(); ++index) {
      if (!done[index]) {
        (void)handles[index].cancel();
      }
    }
  }

  void record_flow_started(std::size_t worker_count) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.flows;
    stats_.worker_tasks += static_cast<std::uint64_t>(worker_count);
  }

  void record_broadcast_started(std::size_t worker_count) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.flows;
    ++stats_.broadcasts;
    stats_.worker_tasks += static_cast<std::uint64_t>(worker_count);
  }

  void record_gather_started() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.gathers;
  }

  void record_gather_finished(const RuntimeFlowGatherResult &result) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.completed_workers += result.completed_count;
    stats_.failed_workers += static_cast<std::uint64_t>(result.failures.size());
    stats_.cancelled_workers += result.cancelled_count;
    if (result.timed_out) {
      ++stats_.timeouts;
    }
  }

  void record_reduction() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.reductions;
  }

  void record_isolation_rejection() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.isolation_rejections;
  }

  RuntimeTaskModule task_;
  mutable std::mutex stats_mutex_;
  RuntimeFlowStats stats_;
};

RuntimeFlowModule::RuntimeFlowModule(std::size_t worker_count)
    : impl_(std::make_shared<Impl>(worker_count)) {}

RuntimeFlowModule::RuntimeFlowModule(RuntimeSchedulerConfig config)
    : impl_(std::make_shared<Impl>(config)) {}

RuntimeFlowModule::RuntimeFlowModule(RuntimeFlowModule &&) noexcept = default;

RuntimeFlowModule &
RuntimeFlowModule::operator=(RuntimeFlowModule &&) noexcept = default;

RuntimeFlowModule::~RuntimeFlowModule() = default;

RuntimeFlowGatherResult
RuntimeFlowModule::gather(std::vector<RuntimeTaskHandle> handles,
                          RuntimeFlowOptions options) {
  return impl_->gather(std::move(handles), options);
}

RuntimeFlowGatherResult
RuntimeFlowModule::scatter(std::vector<Value> partitions, MapFunction function,
                           RuntimeFlowOptions options) {
  return impl_->scatter(std::move(partitions), std::move(function), options);
}

RuntimeFlowGatherResult
RuntimeFlowModule::scatter_map(std::vector<Value> items, MapFunction function,
                               RuntimeFlowOptions options) {
  return impl_->scatter_map(std::move(items), std::move(function), options);
}

RuntimeFlowReduceResult
RuntimeFlowModule::scatter_reduce(std::vector<Value> items, Value init,
                                  MapFunction map, ReduceFunction reduce,
                                  RuntimeFlowOptions options) {
  return impl_->scatter_reduce(std::move(items), std::move(init),
                               std::move(map), std::move(reduce), options);
}

RuntimeFlowGatherResult
RuntimeFlowModule::broadcast(Value value, std::size_t workers,
                             BroadcastFunction function,
                             RuntimeFlowOptions options) {
  return impl_->broadcast(std::move(value), workers, std::move(function),
                          options);
}

RuntimeFlowStats RuntimeFlowModule::stats() const { return impl_->stats(); }

class RuntimeThreadedCollection::Impl {
public:
  Impl(std::vector<Value> items, std::size_t workers,
       RuntimeFlowOptions options, RuntimeFlowPartitionPolicy scatter_policy)
      : items_(std::move(items)),
        flow_(RuntimeSchedulerConfig{workers == 0 ? options.workers : workers,
                                     1}),
        options_(options) {
    if (workers != 0) {
      options_.workers = workers;
    }
    options_.partition_policy = scatter_policy;
  }

  RuntimeFlowGatherResult each(EachFunction function) {
    if (!function) {
      return argument_error("threaded each block is missing");
    }
    RuntimeFlowGatherResult result = flow_.scatter_map(
        items_,
        [function](const Value &value, std::size_t index) {
          function(value, index);
          return Value::null();
        },
        options_);
    record([&](RuntimeThreadedCollectionStats *stats) {
      ++stats->each_operations;
      stats->generated_values += result.completed_count;
    });
    return result;
  }

  RuntimeFlowGatherResult map(MapFunction function) {
    if (!function) {
      return argument_error("threaded map block is missing");
    }
    RuntimeFlowGatherResult result =
        flow_.scatter_map(items_, std::move(function), options_);
    record([&](RuntimeThreadedCollectionStats *stats) {
      ++stats->map_operations;
      stats->generated_values +=
          static_cast<std::uint64_t>(result.values.size());
    });
    return result;
  }

  RuntimeFlowGatherResult select(PredicateFunction function) {
    return filter(std::move(function), false);
  }

  RuntimeFlowGatherResult reject(PredicateFunction function) {
    return filter(std::move(function), true);
  }

  RuntimeFlowGatherResult filter_map(MapFunction function) {
    if (!function) {
      return argument_error("threaded filter_map block is missing");
    }

    RuntimeFlowGatherResult gathered =
        flow_.scatter_map(items_, std::move(function), options_);
    if (!gathered.ok || gathered.failed) {
      record([&](RuntimeThreadedCollectionStats *stats) {
        ++stats->filter_map_operations;
      });
      return gathered;
    }

    RuntimeFlowGatherResult result = gathered;
    result.values.clear();
    for (const Value &value : gathered.values) {
      const bool truthy =
          !value.is_null() && !(value.is_bool() && !value.as_bool());
      if (truthy) {
        result.values.push_back(value);
      }
    }
    result.completed_count = static_cast<std::uint64_t>(result.values.size());
    record([&](RuntimeThreadedCollectionStats *stats) {
      ++stats->filter_map_operations;
      stats->generated_values +=
          static_cast<std::uint64_t>(result.values.size());
    });
    return result;
  }

  RuntimeFlowGatherResult flat_map(FlatMapFunction function) {
    if (!function) {
      return argument_error("threaded flat_map block is missing");
    }

    RuntimeFlowGatherResult gathered = flow_.scatter_map(
        items_,
        [function](const Value &value, std::size_t index) {
          return make_list_value(function(value, index), true);
        },
        options_);
    if (!gathered.ok || gathered.failed) {
      record([&](RuntimeThreadedCollectionStats *stats) {
        ++stats->flat_map_operations;
      });
      return gathered;
    }

    RuntimeFlowGatherResult result = gathered;
    result.values.clear();
    for (const Value &value : gathered.values) {
      if (!value.is_list() || value.as_list() == nullptr) {
        result.ok = false;
        result.failed = true;
        result.error_name = "TypeError";
        result.message = "threaded flat_map worker returned non-list value";
        result.failures.push_back(RuntimeFlowFailure{
            result.values.size(), result.error_name, result.message});
        result.failed_count =
            static_cast<std::uint64_t>(result.failures.size());
        record([&](RuntimeThreadedCollectionStats *stats) {
          ++stats->flat_map_operations;
          stats->generated_values +=
              static_cast<std::uint64_t>(result.values.size());
        });
        return result;
      }
      const IntrusivePtr<ListValue> list = value.as_list();
      result.values.insert(result.values.end(), list->items.begin(),
                           list->items.end());
    }
    result.completed_count = static_cast<std::uint64_t>(result.values.size());
    record([&](RuntimeThreadedCollectionStats *stats) {
      ++stats->flat_map_operations;
      stats->generated_values +=
          static_cast<std::uint64_t>(result.values.size());
    });
    return result;
  }

  RuntimeFlowGatherResult combination(std::size_t count) {
    std::vector<std::vector<std::size_t>> indexes =
        combination_indexes(items_.size(), count);
    RuntimeFlowGatherResult result =
        map_index_rows(std::move(indexes), "threaded combination");
    record([&](RuntimeThreadedCollectionStats *stats) {
      ++stats->combination_operations;
      stats->generated_values +=
          static_cast<std::uint64_t>(result.values.size());
    });
    return result;
  }

  RuntimeFlowGatherResult permutation(std::size_t count) {
    std::vector<std::vector<std::size_t>> indexes =
        permutation_indexes(items_.size(), count);
    RuntimeFlowGatherResult result =
        map_index_rows(std::move(indexes), "threaded permutation");
    record([&](RuntimeThreadedCollectionStats *stats) {
      ++stats->permutation_operations;
      stats->generated_values +=
          static_cast<std::uint64_t>(result.values.size());
    });
    return result;
  }

  RuntimeThreadedCollectionStats stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    RuntimeThreadedCollectionStats out = stats_;
    out.flow = flow_.stats();
    return out;
  }

private:
  static RuntimeFlowGatherResult argument_error(std::string message) {
    RuntimeFlowGatherResult result;
    result.error_name = "ArgumentError";
    result.message = std::move(message);
    return result;
  }

  RuntimeFlowGatherResult filter(PredicateFunction function, bool invert) {
    if (!function) {
      return argument_error(invert ? "threaded reject block is missing"
                                   : "threaded select block is missing");
    }

    RuntimeFlowGatherResult gathered = flow_.scatter_map(
        items_,
        [function, invert](const Value &value, std::size_t index) {
          const bool matched = function(value, index);
          const bool keep = invert ? !matched : matched;
          return make_tuple_value({Value::boolean(keep), value});
        },
        options_);
    if (!gathered.ok || gathered.failed) {
      record([&](RuntimeThreadedCollectionStats *stats) {
        ++stats->filter_operations;
      });
      return gathered;
    }

    RuntimeFlowGatherResult result = gathered;
    result.values.clear();
    for (std::size_t index = 0; index < gathered.values.size(); ++index) {
      const Value &value = gathered.values[index];
      if (!value.is_tuple() || value.as_tuple() == nullptr ||
          value.as_tuple()->items.size() != 2U ||
          !value.as_tuple()->items[0].is_bool()) {
        result.ok = false;
        result.failed = true;
        result.error_name = "TypeError";
        result.message = "threaded filter worker returned invalid marker";
        result.failures.push_back(
            RuntimeFlowFailure{index, result.error_name, result.message});
        result.failed_count =
            static_cast<std::uint64_t>(result.failures.size());
        record([&](RuntimeThreadedCollectionStats *stats) {
          ++stats->filter_operations;
          stats->generated_values +=
              static_cast<std::uint64_t>(result.values.size());
        });
        return result;
      }
      const std::vector<Value> &tuple_items = value.as_tuple()->items;
      if (tuple_items[0].as_bool()) {
        result.values.push_back(tuple_items[1]);
      }
    }
    result.completed_count = static_cast<std::uint64_t>(result.values.size());
    record([&](RuntimeThreadedCollectionStats *stats) {
      ++stats->filter_operations;
      stats->generated_values +=
          static_cast<std::uint64_t>(result.values.size());
    });
    return result;
  }

  RuntimeFlowGatherResult
  map_index_rows(std::vector<std::vector<std::size_t>> rows,
                 const std::string &context) {
    RuntimeFlowGatherResult source_validation = validate_source_items(context);
    if (!source_validation.ok && !source_validation.error_name.empty()) {
      return source_validation;
    }

    std::vector<Value> row_values;
    row_values.reserve(rows.size());
    for (const std::vector<std::size_t> &row : rows) {
      std::vector<Value> index_values;
      index_values.reserve(row.size());
      for (std::size_t index : row) {
        index_values.push_back(
            Value::integer(static_cast<std::int64_t>(index)));
      }
      row_values.push_back(make_list_value(std::move(index_values), true));
    }

    RuntimeFlowGatherResult result = flow_.scatter_map(
        std::move(row_values),
        [this, context](const Value &value, std::size_t index) {
          if (!value.is_list() || value.as_list() == nullptr) {
            throw RuntimeTaskFailure("TypeError",
                                     context + " index row is invalid");
          }
          std::vector<Value> values;
          values.reserve(value.as_list()->items.size());
          for (const Value &item : value.as_list()->items) {
            if (!item.is_integer()) {
              throw RuntimeTaskFailure("TypeError",
                                       context + " index row is invalid");
            }
            const std::int64_t raw_index = item.as_integer();
            if (raw_index < 0 ||
                static_cast<std::uint64_t>(raw_index) >= items_.size()) {
              throw RuntimeTaskFailure("IndexError",
                                       context + " index is out of bounds");
            }
            values.push_back(items_[static_cast<std::size_t>(raw_index)]);
          }
          (void)index;
          return make_list_value(std::move(values), true);
        },
        options_);
    return result;
  }

  RuntimeFlowGatherResult validate_source_items(const std::string &context) {
    RuntimeFlowGatherResult ok;
    ok.ok = true;
    if (options_.isolation == RuntimeFlowIsolationMode::Unchecked) {
      return ok;
    }
    for (std::size_t index = 0; index < items_.size(); ++index) {
      std::optional<RuntimeSyncBoundaryError> shareability_error =
          runtime_value_shareability_error(items_[index]);
      if (shareability_error.has_value()) {
        RuntimeFlowGatherResult result;
        result.failed = true;
        result.error_name = shareability_error->error_name;
        result.message = shareability_error->message;
        result.failures.push_back(RuntimeFlowFailure{
            index, result.error_name, context + " source rejected"});
        result.failed_count = 1;
        return result;
      }
    }
    return ok;
  }

  static std::vector<std::vector<std::size_t>>
  combination_indexes(std::size_t size, std::size_t count) {
    std::vector<std::vector<std::size_t>> rows;
    if (count > size) {
      return rows;
    }

    std::vector<std::size_t> current;
    current.reserve(count);
    std::function<void(std::size_t)> visit = [&](std::size_t start) {
      if (current.size() == count) {
        rows.push_back(current);
        return;
      }
      const std::size_t remaining = count - current.size();
      for (std::size_t index = start; index + remaining <= size; ++index) {
        current.push_back(index);
        visit(index + 1U);
        current.pop_back();
      }
    };
    visit(0U);
    return rows;
  }

  static std::vector<std::vector<std::size_t>>
  permutation_indexes(std::size_t size, std::size_t count) {
    std::vector<std::vector<std::size_t>> rows;
    if (count > size) {
      return rows;
    }

    std::vector<std::size_t> current;
    std::vector<bool> used(size, false);
    current.reserve(count);
    std::function<void()> visit = [&]() {
      if (current.size() == count) {
        rows.push_back(current);
        return;
      }
      for (std::size_t index = 0; index < size; ++index) {
        if (used[index]) {
          continue;
        }
        used[index] = true;
        current.push_back(index);
        visit();
        current.pop_back();
        used[index] = false;
      }
    };
    visit();
    return rows;
  }

  void
  record(const std::function<void(RuntimeThreadedCollectionStats *)> &update) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.operations;
    if (update) {
      update(&stats_);
    }
  }

  std::vector<Value> items_;
  RuntimeFlowModule flow_;
  RuntimeFlowOptions options_;
  mutable std::mutex stats_mutex_;
  RuntimeThreadedCollectionStats stats_;
};

RuntimeThreadedCollection::RuntimeThreadedCollection(
    std::vector<Value> items, std::size_t workers, RuntimeFlowOptions options,
    RuntimeFlowPartitionPolicy scatter_policy)
    : impl_(std::make_shared<Impl>(std::move(items), workers, options,
                                   scatter_policy)) {}

RuntimeThreadedCollection::RuntimeThreadedCollection(
    RuntimeThreadedCollection &&) noexcept = default;

RuntimeThreadedCollection &RuntimeThreadedCollection::operator=(
    RuntimeThreadedCollection &&) noexcept = default;

RuntimeThreadedCollection::~RuntimeThreadedCollection() = default;

RuntimeFlowGatherResult RuntimeThreadedCollection::each(EachFunction function) {
  return impl_->each(std::move(function));
}

RuntimeFlowGatherResult RuntimeThreadedCollection::map(MapFunction function) {
  return impl_->map(std::move(function));
}

RuntimeFlowGatherResult
RuntimeThreadedCollection::filter_map(MapFunction function) {
  return impl_->filter_map(std::move(function));
}

RuntimeFlowGatherResult
RuntimeThreadedCollection::select(PredicateFunction function) {
  return impl_->select(std::move(function));
}

RuntimeFlowGatherResult
RuntimeThreadedCollection::reject(PredicateFunction function) {
  return impl_->reject(std::move(function));
}

RuntimeFlowGatherResult
RuntimeThreadedCollection::flat_map(FlatMapFunction function) {
  return impl_->flat_map(std::move(function));
}

RuntimeFlowGatherResult
RuntimeThreadedCollection::combination(std::size_t count) {
  return impl_->combination(count);
}

RuntimeFlowGatherResult
RuntimeThreadedCollection::permutation(std::size_t count) {
  return impl_->permutation(count);
}

RuntimeThreadedCollectionStats RuntimeThreadedCollection::stats() const {
  return impl_->stats();
}
} // namespace amber::runtime
