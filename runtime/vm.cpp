#include "runtime/vm.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace amber::runtime {

namespace {

thread_local std::uint64_t tls_runtime_worker_id = 0;
thread_local std::uint64_t tls_runtime_strand_id = 0;
thread_local std::uint64_t tls_runtime_task_id = 0;
thread_local const std::atomic<bool> *tls_runtime_task_cancel_flag = nullptr;
thread_local std::uint32_t tls_runtime_task_sync_depth = 0;
thread_local std::shared_ptr<RuntimeTextWriter> tls_runtime_stdout;
thread_local std::shared_ptr<RuntimeTextWriter> tls_runtime_stderr;
thread_local std::string tls_runtime_task_annotation;
thread_local std::uint64_t tls_runtime_native_thread_id = 0;
std::atomic<std::uint64_t> g_runtime_output_order{1};
std::atomic<std::uint64_t> g_runtime_native_thread_ids{1};

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

class RuntimeTaskSyncScope {
public:
  RuntimeTaskSyncScope() { ++tls_runtime_task_sync_depth; }
  RuntimeTaskSyncScope(const RuntimeTaskSyncScope &) = delete;
  RuntimeTaskSyncScope &operator=(const RuntimeTaskSyncScope &) = delete;
  ~RuntimeTaskSyncScope() {
    if (tls_runtime_task_sync_depth > 0) {
      --tls_runtime_task_sync_depth;
    }
  }
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
  case HeapObjectKind::Set:
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
std::shared_ptr<RuntimeWatchCell> watch_cell_from_value(const Value &value);
Value unwrap_watch_value(const Value &value);

struct RuntimeSyncBoundaryError {
  std::string error_name;
  std::string message;
};

std::optional<RuntimeSyncBoundaryError>
runtime_value_shareability_error(const Value &value);
bool value_equals(const Value &lhs, const Value &rhs);

} // namespace

std::uint64_t current_runtime_worker_id() { return tls_runtime_worker_id; }

std::uint64_t current_runtime_strand_id() { return tls_runtime_strand_id; }

std::uint64_t current_runtime_task_id() { return tls_runtime_task_id; }

std::uint64_t current_runtime_native_thread_id() {
  if (tls_runtime_native_thread_id == 0) {
    tls_runtime_native_thread_id =
        g_runtime_native_thread_ids.fetch_add(1, std::memory_order_relaxed);
  }
  return tls_runtime_native_thread_id;
}

std::string current_runtime_task_annotation() {
  return tls_runtime_task_annotation;
}

bool current_runtime_task_cancel_requested() {
  return tls_runtime_task_cancel_flag != nullptr &&
         tls_runtime_task_cancel_flag->load();
}

bool current_runtime_task_sync_active() {
  return tls_runtime_task_sync_depth != 0;
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

RuntimeTaskAnnotationScope::RuntimeTaskAnnotationScope(std::string annotation)
    : previous_annotation_(std::move(tls_runtime_task_annotation)) {
  tls_runtime_task_annotation = std::move(annotation);
}

RuntimeTaskAnnotationScope::~RuntimeTaskAnnotationScope() {
  tls_runtime_task_annotation = std::move(previous_annotation_);
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

  if (value.is_set()) {
    const std::shared_ptr<SetValue> set = value.as_set();
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
  if (value.is_set()) {
    const std::shared_ptr<SetValue> set = value.as_set();
    if (set != nullptr) {
      children->insert(children->end(), set->items.begin(), set->items.end());
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

class RuntimeWatchCell::Impl {
public:
  Impl(Value value, std::uint64_t cell_id, std::string target_name)
      : value_(std::move(value)), cell_id_(cell_id),
        target_name_(std::move(target_name)) {}

  Value read() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return value_;
  }

  RuntimeWatchWriteResult write(Value value) {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchWriteResult result;
    result.old_value = value_;
    result.new_value = value;
    result.old_revision = revision_;
    result.new_revision = revision_;
    if (watched_) {
      value_ = std::move(value);
      ++revision_;
      result.changed = true;
      result.new_revision = revision_;
      result.new_value = value_;
      return result;
    }
    value_ = std::move(value);
    result.new_value = value_;
    return result;
  }

  RuntimeWatchCellSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchCellSnapshot snapshot;
    snapshot.cell_id = cell_id_;
    snapshot.revision = revision_;
    snapshot.subscriber_count = subscriber_count_;
    snapshot.target_name = target_name_;
    snapshot.watched = watched_;
    snapshot.value = value_;
    return snapshot;
  }

  bool watched() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return watched_;
  }

  void enable_watch(std::string target_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    watched_ = true;
    if (!target_name.empty()) {
      target_name_ = std::move(target_name);
    }
  }

  void subscribe() {
    std::lock_guard<std::mutex> lock(mutex_);
    watched_ = true;
    ++subscriber_count_;
  }

  void unsubscribe() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscriber_count_ > 0) {
      --subscriber_count_;
    }
  }

  std::uint64_t cell_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cell_id_;
  }

private:
  mutable std::mutex mutex_;
  Value value_ = Value::null();
  std::uint64_t cell_id_ = 0;
  std::uint64_t revision_ = 0;
  std::uint64_t subscriber_count_ = 0;
  std::string target_name_;
  bool watched_ = false;
};

RuntimeWatchCell::RuntimeWatchCell(Value value, std::uint64_t cell_id,
                                   std::string target_name)
    : impl_(std::make_shared<Impl>(std::move(value), cell_id,
                                   std::move(target_name))) {}

Value RuntimeWatchCell::read() const { return impl_->read(); }

RuntimeWatchWriteResult RuntimeWatchCell::write(Value value) {
  return impl_->write(std::move(value));
}

RuntimeWatchCellSnapshot RuntimeWatchCell::snapshot() const {
  return impl_->snapshot();
}

bool RuntimeWatchCell::watched() const { return impl_->watched(); }

void RuntimeWatchCell::enable_watch(std::string target_name) {
  impl_->enable_watch(std::move(target_name));
}

void RuntimeWatchCell::subscribe() { impl_->subscribe(); }

void RuntimeWatchCell::unsubscribe() { impl_->unsubscribe(); }

std::uint64_t RuntimeWatchCell::cell_id() const { return impl_->cell_id(); }

class RuntimeWatchObjectState::Impl {
public:
  explicit Impl(std::uint64_t object_id) : object_id_(object_id) {}

  RuntimeWatchObjectStateSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchObjectStateSnapshot snapshot;
    snapshot.object_id = object_id_;
    snapshot.object_revision = object_revision_;
    snapshot.subscriber_count = subscriber_count_;
    for (const auto &[name, field] : fields_) {
      snapshot.field_revisions[name] = field.revision;
    }
    return snapshot;
  }

  RuntimeWatchIvarSnapshot snapshot_field(const std::string &field_name,
                                          Value current_value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchIvarSnapshot snapshot;
    snapshot.object_id = object_id_;
    snapshot.object_revision = object_revision_;
    snapshot.field_name = field_name;
    snapshot.value = std::move(current_value);
    const auto found = fields_.find(field_name);
    if (found != fields_.end()) {
      snapshot.field_revision = found->second.revision;
      snapshot.subscriber_count = found->second.subscriber_count;
      snapshot.watched = found->second.watched;
    }
    return snapshot;
  }

  RuntimeWatchIvarWriteResult write_field(std::string field_name,
                                          Value old_value, Value new_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchIvarWriteResult result;
    result.field_name = field_name;
    result.old_value = std::move(old_value);
    result.new_value = std::move(new_value);
    FieldState &field = fields_[field_name];
    result.old_revision = field.revision;
    result.new_revision = field.revision;
    result.old_object_revision = object_revision_;
    result.new_object_revision = object_revision_;
    if (!field.watched) {
      return result;
    }
    ++field.revision;
    ++object_revision_;
    result.changed = true;
    result.new_revision = field.revision;
    result.new_object_revision = object_revision_;
    return result;
  }

  void subscribe_field(std::string field_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    FieldState &field = fields_[std::move(field_name)];
    field.watched = true;
    ++field.subscriber_count;
    ++subscriber_count_;
  }

  void unsubscribe_field(const std::string &field_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = fields_.find(field_name);
    if (found == fields_.end()) {
      return;
    }
    if (found->second.subscriber_count > 0) {
      --found->second.subscriber_count;
    }
    if (subscriber_count_ > 0) {
      --subscriber_count_;
    }
  }

  bool field_watched(const std::string &field_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = fields_.find(field_name);
    return found != fields_.end() && found->second.watched;
  }

  std::uint64_t object_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return object_id_;
  }

private:
  struct FieldState {
    std::uint64_t revision = 0;
    std::uint64_t subscriber_count = 0;
    bool watched = false;
  };

  mutable std::mutex mutex_;
  std::uint64_t object_id_ = 0;
  std::uint64_t object_revision_ = 0;
  std::uint64_t subscriber_count_ = 0;
  std::unordered_map<std::string, FieldState> fields_;
};

RuntimeWatchObjectState::RuntimeWatchObjectState(std::uint64_t object_id)
    : impl_(std::make_shared<Impl>(object_id)) {}

RuntimeWatchObjectStateSnapshot RuntimeWatchObjectState::snapshot() const {
  return impl_->snapshot();
}

RuntimeWatchIvarSnapshot
RuntimeWatchObjectState::snapshot_field(const std::string &field_name,
                                        Value current_value) const {
  return impl_->snapshot_field(field_name, std::move(current_value));
}

RuntimeWatchIvarWriteResult
RuntimeWatchObjectState::write_field(std::string field_name, Value old_value,
                                     Value new_value) {
  return impl_->write_field(std::move(field_name), std::move(old_value),
                            std::move(new_value));
}

void RuntimeWatchObjectState::subscribe_field(std::string field_name) {
  impl_->subscribe_field(std::move(field_name));
}

void RuntimeWatchObjectState::unsubscribe_field(const std::string &field_name) {
  impl_->unsubscribe_field(field_name);
}

bool RuntimeWatchObjectState::field_watched(
    const std::string &field_name) const {
  return impl_->field_watched(field_name);
}

std::uint64_t RuntimeWatchObjectState::object_id() const {
  return impl_->object_id();
}

class RuntimeWatchHandle::Impl {
public:
  Impl(std::shared_ptr<RuntimeWatchCell> cell, std::uint64_t handle_id,
       std::string target_name)
      : cell_(std::move(cell)), handle_id_(handle_id),
        target_name_(std::move(target_name)) {
    if (cell_ != nullptr) {
      cell_->enable_watch(target_name_);
      cell_->subscribe();
      active_ = true;
    }
  }

  Impl(std::shared_ptr<RuntimeWatchObjectState> object_state,
       std::uint64_t handle_id, std::string target_name, std::string field_name)
      : object_state_(std::move(object_state)), handle_id_(handle_id),
        target_name_(std::move(target_name)),
        field_name_(std::move(field_name)) {
    if (object_state_ != nullptr) {
      object_state_->subscribe_field(field_name_);
      active_ = true;
    }
  }

  ~Impl() { unwatch(); }

  bool active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
  }

  bool unwatch() {
    std::shared_ptr<RuntimeWatchCell> cell;
    std::shared_ptr<RuntimeWatchObjectState> object_state;
    std::string field_name;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!active_) {
        return false;
      }
      active_ = false;
      cell = cell_;
      object_state = object_state_;
      field_name = field_name_;
    }
    if (cell != nullptr) {
      cell->unsubscribe();
    }
    if (object_state != nullptr) {
      object_state->unsubscribe_field(field_name);
    }
    return true;
  }

  std::uint64_t handle_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handle_id_;
  }

  std::shared_ptr<RuntimeWatchCell> cell() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cell_;
  }

  RuntimeWatchCellSnapshot snapshot() const {
    std::shared_ptr<RuntimeWatchCell> cell;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cell = cell_;
    }
    return cell == nullptr ? RuntimeWatchCellSnapshot{} : cell->snapshot();
  }

private:
  mutable std::mutex mutex_;
  std::shared_ptr<RuntimeWatchCell> cell_;
  std::shared_ptr<RuntimeWatchObjectState> object_state_;
  std::uint64_t handle_id_ = 0;
  std::string target_name_;
  std::string field_name_;
  bool active_ = false;
};

RuntimeWatchHandle::RuntimeWatchHandle()
    : impl_(std::make_shared<Impl>(nullptr, 0, "")) {}

RuntimeWatchHandle::RuntimeWatchHandle(std::shared_ptr<RuntimeWatchCell> cell,
                                       std::uint64_t handle_id,
                                       std::string target_name)
    : impl_(std::make_shared<Impl>(std::move(cell), handle_id,
                                   std::move(target_name))) {}

RuntimeWatchHandle::RuntimeWatchHandle(
    std::shared_ptr<RuntimeWatchObjectState> object_state,
    std::uint64_t handle_id, std::string target_name, std::string field_name)
    : impl_(std::make_shared<Impl>(std::move(object_state), handle_id,
                                   std::move(target_name),
                                   std::move(field_name))) {}

RuntimeWatchHandle::RuntimeWatchHandle(RuntimeWatchHandle &&) noexcept =
    default;

RuntimeWatchHandle &
RuntimeWatchHandle::operator=(RuntimeWatchHandle &&) noexcept = default;

RuntimeWatchHandle::~RuntimeWatchHandle() = default;

bool RuntimeWatchHandle::active() const { return impl_->active(); }

bool RuntimeWatchHandle::unwatch() { return impl_->unwatch(); }

std::uint64_t RuntimeWatchHandle::handle_id() const {
  return impl_->handle_id();
}

std::shared_ptr<RuntimeWatchCell> RuntimeWatchHandle::cell() const {
  return impl_->cell();
}

RuntimeWatchCellSnapshot RuntimeWatchHandle::snapshot() const {
  return impl_->snapshot();
}

namespace {

std::shared_ptr<RuntimeWatchCell> watch_cell_from_value(const Value &value) {
  if (!value.is_watch_cell()) {
    return nullptr;
  }
  return value.as_watch_cell();
}

Value unwrap_watch_value(const Value &value) {
  const std::shared_ptr<RuntimeWatchCell> cell = watch_cell_from_value(value);
  return cell == nullptr ? value : cell->read();
}

} // namespace

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
    try {
      const Value value = function ? function() : Value::null();
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
      const std::shared_ptr<ListValue> list = value.as_list();
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
    case HeapObjectKind::Set:
      return &static_cast<SetValue *>(record.ptr)->header;
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
    case HeapObjectKind::Set: {
      const auto *set = static_cast<const SetValue *>(record.ptr);
      out->insert(out->end(), set->items.begin(), set->items.end());
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
    case HeapObjectKind::Set: {
      auto *set = static_cast<SetValue *>(record.ptr);
      move_values_for_deferred_release(&set->items, deferred);
      set->frozen = false;
      set->header.shape = dead_shape;
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

Value RuntimeHeap::make_set_value(std::vector<Value> items, bool frozen) {
  std::vector<Value> unique_items;
  unique_items.reserve(items.size());
  for (Value &item : items) {
    const bool exists = std::find_if(unique_items.begin(), unique_items.end(),
                                     [&](const Value &seen) {
                                       return value_equals(seen, item);
                                     }) != unique_items.end();
    if (!exists) {
      unique_items.push_back(std::move(item));
    }
  }

  auto value = impl_->allocate<SetValue>(
      HeapObjectKind::Set, [frozen, &unique_items](SetValue &value) {
        value.header.flags =
            frozen ? kObjectFlagFrozen | kObjectFlagShareable : 0U;
        value.header.owner.kind =
            frozen ? OwnerTokenKind::Shareable : OwnerTokenKind::Confined;
        value.header.generation =
            frozen ? ObjectGeneration::Shared : ObjectGeneration::Young;
        value.items = std::move(unique_items);
        value.frozen = frozen;
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

namespace {

enum class RuntimeTextWriterKind { HostStdout, HostStderr, Buffer, CellStream };

RuntimeTextWriteResult text_write_ok() { return {}; }

RuntimeTextWriteResult text_write_error(std::string error_name,
                                        std::string message) {
  RuntimeTextWriteResult result;
  result.ok = false;
  result.error_name = std::move(error_name);
  result.message = std::move(message);
  return result;
}

} // namespace

class RuntimeTextWriter::Impl {
public:
  Impl(RuntimeTextWriterKind writer_kind, std::string writer_stream)
      : kind(writer_kind), stream(std::move(writer_stream)) {}

  RuntimeTextWriterKind kind = RuntimeTextWriterKind::Buffer;
  std::string stream;
  mutable std::mutex mutex;
  bool closed = false;
  std::string buffer;
  std::vector<RuntimeTextOutputEvent> events;
};

RuntimeTextWriter::RuntimeTextWriter()
    : impl_(std::make_shared<Impl>(RuntimeTextWriterKind::Buffer, "")) {}

RuntimeTextWriter::RuntimeTextWriter(RuntimeTextWriter &&) noexcept = default;

RuntimeTextWriter &
RuntimeTextWriter::operator=(RuntimeTextWriter &&) noexcept = default;

RuntimeTextWriter::~RuntimeTextWriter() = default;

std::shared_ptr<RuntimeTextWriter> RuntimeTextWriter::host_stdout() {
  static std::shared_ptr<RuntimeTextWriter> writer = [] {
    auto out = std::make_shared<RuntimeTextWriter>();
    out->impl_ =
        std::make_shared<Impl>(RuntimeTextWriterKind::HostStdout, "stdout");
    return out;
  }();
  return writer;
}

std::shared_ptr<RuntimeTextWriter> RuntimeTextWriter::host_stderr() {
  static std::shared_ptr<RuntimeTextWriter> writer = [] {
    auto out = std::make_shared<RuntimeTextWriter>();
    out->impl_ =
        std::make_shared<Impl>(RuntimeTextWriterKind::HostStderr, "stderr");
    return out;
  }();
  return writer;
}

std::shared_ptr<RuntimeTextWriter> RuntimeTextWriter::buffer() {
  return std::make_shared<RuntimeTextWriter>();
}

std::shared_ptr<RuntimeTextWriter>
RuntimeTextWriter::cell_stream(std::string stream_name) {
  auto out = std::make_shared<RuntimeTextWriter>();
  out->impl_ = std::make_shared<Impl>(RuntimeTextWriterKind::CellStream,
                                      std::move(stream_name));
  return out;
}

RuntimeTextWriteResult RuntimeTextWriter::write_str(const std::string &text) {
  if (impl_ == nullptr) {
    return text_write_error("IOError", "text writer is not initialized");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->closed) {
    return text_write_error("ClosedResourceError", "text writer is closed");
  }
  switch (impl_->kind) {
  case RuntimeTextWriterKind::HostStdout:
    std::cout << text;
    if (!std::cout.good()) {
      return text_write_error("IOError", "stdout write failed");
    }
    return text_write_ok();
  case RuntimeTextWriterKind::HostStderr:
    std::cerr << text;
    if (!std::cerr.good()) {
      return text_write_error("IOError", "stderr write failed");
    }
    return text_write_ok();
  case RuntimeTextWriterKind::Buffer:
    impl_->buffer += text;
    return text_write_ok();
  case RuntimeTextWriterKind::CellStream:
    impl_->buffer += text;
    impl_->events.push_back(RuntimeTextOutputEvent{
        impl_->stream.empty() ? "stdout" : impl_->stream, text,
        g_runtime_output_order.fetch_add(1, std::memory_order_relaxed)});
    return text_write_ok();
  }
  return text_write_error("IOError", "unknown text writer kind");
}

RuntimeTextWriteResult RuntimeTextWriter::write_line(const std::string &text) {
  RuntimeTextWriteResult result = write_str(text);
  if (!result.ok) {
    return result;
  }
  return write_str("\n");
}

RuntimeTextWriteResult RuntimeTextWriter::flush() {
  if (impl_ == nullptr) {
    return text_write_error("IOError", "text writer is not initialized");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->closed) {
    return text_write_error("ClosedResourceError", "text writer is closed");
  }
  if (impl_->kind == RuntimeTextWriterKind::HostStdout) {
    std::cout.flush();
    if (!std::cout.good()) {
      return text_write_error("IOError", "stdout flush failed");
    }
  } else if (impl_->kind == RuntimeTextWriterKind::HostStderr) {
    std::cerr.flush();
    if (!std::cerr.good()) {
      return text_write_error("IOError", "stderr flush failed");
    }
  }
  return text_write_ok();
}

RuntimeTextWriteResult RuntimeTextWriter::close() {
  if (impl_ == nullptr) {
    return text_write_error("IOError", "text writer is not initialized");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->closed = true;
  return text_write_ok();
}

bool RuntimeTextWriter::closed() const {
  if (impl_ == nullptr) {
    return true;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->closed;
}

bool RuntimeTextWriter::buffered() const {
  if (impl_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->kind == RuntimeTextWriterKind::Buffer ||
         impl_->kind == RuntimeTextWriterKind::CellStream;
}

bool RuntimeTextWriter::xterm_color_available() const {
  if (impl_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->kind != RuntimeTextWriterKind::HostStdout &&
      impl_->kind != RuntimeTextWriterKind::HostStderr) {
    return false;
  }
  if (std::getenv("NO_COLOR") != nullptr) {
    return false;
  }
  const char *term = std::getenv("TERM");
  if (term == nullptr || std::string(term) == "dumb") {
    return false;
  }
#if defined(_WIN32)
  const int fd = impl_->kind == RuntimeTextWriterKind::HostStdout ? 1 : 2;
  return _isatty(fd) != 0;
#else
  const int fd = impl_->kind == RuntimeTextWriterKind::HostStdout
                     ? STDOUT_FILENO
                     : STDERR_FILENO;
  return ::isatty(fd) != 0;
#endif
}

std::string RuntimeTextWriter::to_string() const {
  if (impl_ == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->buffer;
}

std::vector<RuntimeTextOutputEvent> RuntimeTextWriter::events() const {
  if (impl_ == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->events;
}

std::string RuntimeTextWriter::stream_name() const {
  if (impl_ == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->stream;
}

std::shared_ptr<RuntimeTextWriter> current_runtime_stdout() {
  return tls_runtime_stdout != nullptr ? tls_runtime_stdout
                                       : RuntimeTextWriter::host_stdout();
}

std::shared_ptr<RuntimeTextWriter> current_runtime_stderr() {
  return tls_runtime_stderr != nullptr ? tls_runtime_stderr
                                       : RuntimeTextWriter::host_stderr();
}

RuntimeOutputScope::RuntimeOutputScope(
    std::shared_ptr<RuntimeTextWriter> stdout_writer,
    std::shared_ptr<RuntimeTextWriter> stderr_writer)
    : previous_stdout_(tls_runtime_stdout),
      previous_stderr_(tls_runtime_stderr) {
  if (stdout_writer != nullptr) {
    tls_runtime_stdout = std::move(stdout_writer);
  }
  if (stderr_writer != nullptr) {
    tls_runtime_stderr = std::move(stderr_writer);
  }
}

RuntimeOutputScope::~RuntimeOutputScope() {
  tls_runtime_stdout = std::move(previous_stdout_);
  tls_runtime_stderr = std::move(previous_stderr_);
}

namespace {

const char *runtime_log_level_name(RuntimeLogLevel level) {
  switch (level) {
  case RuntimeLogLevel::Fatal:
    return "FATAL";
  case RuntimeLogLevel::Error:
    return "ERROR";
  case RuntimeLogLevel::Warn:
    return "WARN";
  case RuntimeLogLevel::Info:
    return "INFO";
  case RuntimeLogLevel::Debug:
    return "DEBUG";
  }
  return "LOG";
}

const char *runtime_log_level_color(RuntimeLogLevel level) {
  switch (level) {
  case RuntimeLogLevel::Fatal:
    return "\033[1;35m";
  case RuntimeLogLevel::Error:
    return "\033[31m";
  case RuntimeLogLevel::Warn:
    return "\033[33m";
  case RuntimeLogLevel::Info:
    return "\033[32m";
  case RuntimeLogLevel::Debug:
    return "\033[36m";
  }
  return "";
}

bool runtime_log_level_enabled(RuntimeLogLevel threshold,
                               RuntimeLogLevel level) {
  return static_cast<int>(level) <= static_cast<int>(threshold);
}

std::string runtime_log_context_label() {
  std::vector<std::string> labels;
  const std::string annotation = current_runtime_task_annotation();
  if (!annotation.empty()) {
    labels.push_back(annotation);
  }
  const std::uint64_t task_id = current_runtime_task_id();
  if (task_id != 0) {
    labels.push_back("task=" + std::to_string(task_id));
  }
  labels.push_back("thread=" +
                   std::to_string(current_runtime_native_thread_id()));

  std::string out;
  for (const std::string &label : labels) {
    if (!out.empty()) {
      out += " ";
    }
    out += label;
  }
  return out;
}

std::string format_runtime_log_line(RuntimeLogLevel level, bool color,
                                    const std::string &message) {
  std::string level_label = runtime_log_level_name(level);
  if (color) {
    level_label =
        std::string(runtime_log_level_color(level)) + level_label + "\033[0m";
  }
  return "[" + level_label + "] [" + runtime_log_context_label() + "] " +
         message;
}

} // namespace

class RuntimeLogger::Impl {
public:
  Impl(std::shared_ptr<RuntimeTextWriter> writer, RuntimeLogLevel level,
       RuntimeLogColorMode color_mode)
      : writer_(writer == nullptr ? current_runtime_stderr()
                                  : std::move(writer)),
        level_(level),
        color_enabled_(color_mode == RuntimeLogColorMode::Always ||
                       (color_mode == RuntimeLogColorMode::Auto &&
                        writer_->xterm_color_available())),
        worker_([this]() { drain_loop(); }) {}

  ~Impl() { close(); }

  RuntimeTextWriteResult log(RuntimeLogLevel level,
                             const std::string &message) {
    if (!runtime_log_level_enabled(level_, level)) {
      return text_write_ok();
    }
    std::string line = format_runtime_log_line(level, color_enabled_, message);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return text_write_error("ClosedResourceError", "logger is closed");
      }
      queue_.push_back(std::move(line));
    }
    cv_.notify_one();
    return text_write_ok();
  }

  RuntimeTextWriteResult flush() {
    RuntimeTextWriteResult result;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      drained_cv_.wait(lock, [this]() { return queue_.empty() && !writing_; });
      result = last_error_;
    }
    if (!result.ok) {
      return result;
    }
    return writer_->flush();
  }

  RuntimeTextWriteResult close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return last_error_;
      }
      closed_ = true;
      stop_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) {
      if (worker_.get_id() == std::this_thread::get_id()) {
        worker_.detach();
      } else {
        worker_.join();
      }
    }
    RuntimeTextWriteResult result;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      result = last_error_;
    }
    if (!result.ok) {
      return result;
    }
    return writer_->flush();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  RuntimeLogLevel level() const { return level_; }

private:
  void drain_loop() {
    while (true) {
      std::deque<std::string> batch;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
        if (queue_.empty() && stop_) {
          drained_cv_.notify_all();
          return;
        }
        batch.swap(queue_);
        writing_ = true;
      }

      for (const std::string &line : batch) {
        RuntimeTextWriteResult result = writer_->write_str(line + "\n");
        if (!result.ok) {
          std::lock_guard<std::mutex> lock(mutex_);
          if (last_error_.ok) {
            last_error_ = std::move(result);
          }
          break;
        }
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        writing_ = false;
      }
      drained_cv_.notify_all();
    }
  }

  std::shared_ptr<RuntimeTextWriter> writer_;
  RuntimeLogLevel level_ = RuntimeLogLevel::Info;
  bool color_enabled_ = false;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable drained_cv_;
  std::deque<std::string> queue_;
  bool closed_ = false;
  bool stop_ = false;
  bool writing_ = false;
  RuntimeTextWriteResult last_error_;
  std::thread worker_;
};

RuntimeLogger::RuntimeLogger(std::shared_ptr<RuntimeTextWriter> writer,
                             RuntimeLogLevel level,
                             RuntimeLogColorMode color_mode)
    : impl_(std::make_shared<Impl>(std::move(writer), level, color_mode)) {}

RuntimeLogger::RuntimeLogger(RuntimeLogger &&) noexcept = default;

RuntimeLogger &RuntimeLogger::operator=(RuntimeLogger &&) noexcept = default;

RuntimeLogger::~RuntimeLogger() {
  if (impl_ != nullptr) {
    impl_->close();
  }
}

RuntimeTextWriteResult RuntimeLogger::log(RuntimeLogLevel level,
                                          const std::string &message) {
  if (impl_ == nullptr) {
    return text_write_error("IOError", "logger is not initialized");
  }
  return impl_->log(level, message);
}

RuntimeTextWriteResult RuntimeLogger::fatal(const std::string &message) {
  return log(RuntimeLogLevel::Fatal, message);
}

RuntimeTextWriteResult RuntimeLogger::error(const std::string &message) {
  return log(RuntimeLogLevel::Error, message);
}

RuntimeTextWriteResult RuntimeLogger::warn(const std::string &message) {
  return log(RuntimeLogLevel::Warn, message);
}

RuntimeTextWriteResult RuntimeLogger::info(const std::string &message) {
  return log(RuntimeLogLevel::Info, message);
}

RuntimeTextWriteResult RuntimeLogger::debug(const std::string &message) {
  return log(RuntimeLogLevel::Debug, message);
}

RuntimeTextWriteResult RuntimeLogger::flush() {
  if (impl_ == nullptr) {
    return text_write_error("IOError", "logger is not initialized");
  }
  return impl_->flush();
}

RuntimeTextWriteResult RuntimeLogger::close() {
  if (impl_ == nullptr) {
    return text_write_error("IOError", "logger is not initialized");
  }
  return impl_->close();
}

bool RuntimeLogger::closed() const {
  return impl_ == nullptr || impl_->closed();
}

RuntimeLogLevel RuntimeLogger::level() const {
  return impl_ == nullptr ? RuntimeLogLevel::Info : impl_->level();
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

Value Value::native_type(RuntimeNativeTypeKind kind) {
  return {NativeTypeValue{kind}};
}

Value Value::native_function(RuntimeNativeFunctionKind kind) {
  return {NativeFunctionValue{kind}};
}

Value Value::task_module(std::shared_ptr<RuntimeTaskModule> value) {
  return {std::move(value)};
}

Value Value::task_handle(std::shared_ptr<RuntimeTaskHandle> value) {
  return {std::move(value)};
}

Value Value::channel(std::shared_ptr<RuntimeChannel> value) {
  return {std::move(value)};
}

Value Value::mutex(std::shared_ptr<RuntimeMutex> value) {
  return {std::move(value)};
}

Value Value::atomic(std::shared_ptr<RuntimeAtomic> value) {
  return {std::move(value)};
}

Value Value::barrier(std::shared_ptr<RuntimeBarrier> value) {
  return {std::move(value)};
}

Value Value::flow_module(std::shared_ptr<RuntimeFlowModule> value) {
  return {std::move(value)};
}

Value Value::threaded_collection(
    std::shared_ptr<RuntimeThreadedCollection> value) {
  return {std::move(value)};
}

Value Value::text_writer(std::shared_ptr<RuntimeTextWriter> value) {
  return {std::move(value)};
}

Value Value::logger(std::shared_ptr<RuntimeLogger> value) {
  return {std::move(value)};
}

Value Value::watch_cell(std::shared_ptr<RuntimeWatchCell> value) {
  return {std::move(value)};
}

Value Value::watch_handle(std::shared_ptr<RuntimeWatchHandle> value) {
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

bool Value::is_set() const {
  return std::holds_alternative<std::shared_ptr<SetValue>>(payload);
}

bool Value::is_map() const {
  return std::holds_alternative<std::shared_ptr<MapValue>>(payload);
}

bool Value::is_native_type() const {
  return std::holds_alternative<NativeTypeValue>(payload);
}

bool Value::is_native_function() const {
  return std::holds_alternative<NativeFunctionValue>(payload);
}

bool Value::is_task_module() const {
  return std::holds_alternative<std::shared_ptr<RuntimeTaskModule>>(payload);
}

bool Value::is_task_handle() const {
  return std::holds_alternative<std::shared_ptr<RuntimeTaskHandle>>(payload);
}

bool Value::is_channel() const {
  return std::holds_alternative<std::shared_ptr<RuntimeChannel>>(payload);
}

bool Value::is_mutex() const {
  return std::holds_alternative<std::shared_ptr<RuntimeMutex>>(payload);
}

bool Value::is_atomic() const {
  return std::holds_alternative<std::shared_ptr<RuntimeAtomic>>(payload);
}

bool Value::is_barrier() const {
  return std::holds_alternative<std::shared_ptr<RuntimeBarrier>>(payload);
}

bool Value::is_flow_module() const {
  return std::holds_alternative<std::shared_ptr<RuntimeFlowModule>>(payload);
}

bool Value::is_threaded_collection() const {
  return std::holds_alternative<std::shared_ptr<RuntimeThreadedCollection>>(
      payload);
}

bool Value::is_text_writer() const {
  return std::holds_alternative<std::shared_ptr<RuntimeTextWriter>>(payload);
}

bool Value::is_logger() const {
  return std::holds_alternative<std::shared_ptr<RuntimeLogger>>(payload);
}

bool Value::is_watch_cell() const {
  return std::holds_alternative<std::shared_ptr<RuntimeWatchCell>>(payload);
}

bool Value::is_watch_handle() const {
  return std::holds_alternative<std::shared_ptr<RuntimeWatchHandle>>(payload);
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

std::shared_ptr<SetValue> Value::as_set() const {
  return std::get<std::shared_ptr<SetValue>>(payload);
}

std::shared_ptr<MapValue> Value::as_map() const {
  return std::get<std::shared_ptr<MapValue>>(payload);
}

NativeTypeValue Value::as_native_type() const {
  return std::get<NativeTypeValue>(payload);
}

NativeFunctionValue Value::as_native_function() const {
  return std::get<NativeFunctionValue>(payload);
}

std::shared_ptr<RuntimeTaskModule> Value::as_task_module() const {
  return std::get<std::shared_ptr<RuntimeTaskModule>>(payload);
}

std::shared_ptr<RuntimeTaskHandle> Value::as_task_handle() const {
  return std::get<std::shared_ptr<RuntimeTaskHandle>>(payload);
}

std::shared_ptr<RuntimeChannel> Value::as_channel() const {
  return std::get<std::shared_ptr<RuntimeChannel>>(payload);
}

std::shared_ptr<RuntimeMutex> Value::as_mutex() const {
  return std::get<std::shared_ptr<RuntimeMutex>>(payload);
}

std::shared_ptr<RuntimeAtomic> Value::as_atomic() const {
  return std::get<std::shared_ptr<RuntimeAtomic>>(payload);
}

std::shared_ptr<RuntimeBarrier> Value::as_barrier() const {
  return std::get<std::shared_ptr<RuntimeBarrier>>(payload);
}

std::shared_ptr<RuntimeFlowModule> Value::as_flow_module() const {
  return std::get<std::shared_ptr<RuntimeFlowModule>>(payload);
}

std::shared_ptr<RuntimeThreadedCollection>
Value::as_threaded_collection() const {
  return std::get<std::shared_ptr<RuntimeThreadedCollection>>(payload);
}

std::shared_ptr<RuntimeTextWriter> Value::as_text_writer() const {
  return std::get<std::shared_ptr<RuntimeTextWriter>>(payload);
}

std::shared_ptr<RuntimeLogger> Value::as_logger() const {
  return std::get<std::shared_ptr<RuntimeLogger>>(payload);
}

std::shared_ptr<RuntimeWatchCell> Value::as_watch_cell() const {
  return std::get<std::shared_ptr<RuntimeWatchCell>>(payload);
}

std::shared_ptr<RuntimeWatchHandle> Value::as_watch_handle() const {
  return std::get<std::shared_ptr<RuntimeWatchHandle>>(payload);
}

Value make_list_value(std::vector<Value> items, bool frozen) {
  return default_runtime_heap().make_list_value(std::move(items), frozen);
}

Value make_tuple_value(std::vector<Value> items) {
  return default_runtime_heap().make_tuple_value(std::move(items));
}

Value make_set_value(std::vector<Value> items, bool frozen) {
  return default_runtime_heap().make_set_value(std::move(items), frozen);
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

constexpr std::uint32_t kMethodFlagInstance =
    amber::bytecode::kMethodFlagInstance;
constexpr std::uint32_t kMethodFlagClass = amber::bytecode::kMethodFlagClass;
constexpr std::uint32_t kMethodFlagPropertyGetter =
    amber::bytecode::kMethodFlagPropertyGetter;
constexpr std::uint32_t kMethodFlagPropertySetter =
    amber::bytecode::kMethodFlagPropertySetter;
constexpr std::int64_t kPatternFailModeSoft = 0;
constexpr std::int64_t kPatternFailModeMatchError = 1;

const char *native_type_name(RuntimeNativeTypeKind kind) {
  switch (kind) {
  case RuntimeNativeTypeKind::TaskModule:
    return "TaskModule";
  case RuntimeNativeTypeKind::Channel:
    return "Channel";
  case RuntimeNativeTypeKind::Mutex:
    return "Mutex";
  case RuntimeNativeTypeKind::Atomic:
    return "Atomic";
  case RuntimeNativeTypeKind::Barrier:
    return "Barrier";
  case RuntimeNativeTypeKind::Flow:
    return "Flow";
  case RuntimeNativeTypeKind::ThreadedCollection:
    return "ThreadedCollection";
  case RuntimeNativeTypeKind::Kernel:
    return "Kernel";
  case RuntimeNativeTypeKind::Io:
    return "io";
  case RuntimeNativeTypeKind::TextBuffer:
    return "io.Buffer";
  case RuntimeNativeTypeKind::Logger:
    return "io.Logger";
  case RuntimeNativeTypeKind::Amber:
    return "Amber";
  case RuntimeNativeTypeKind::Str:
    return "Str";
  case RuntimeNativeTypeKind::Int:
    return "Int";
  case RuntimeNativeTypeKind::Float:
    return "Float";
  case RuntimeNativeTypeKind::Bool:
    return "Bool";
  case RuntimeNativeTypeKind::Symbol:
    return "Symbol";
  case RuntimeNativeTypeKind::Array:
    return "Array";
  case RuntimeNativeTypeKind::Tuple:
    return "Tuple";
  case RuntimeNativeTypeKind::Set:
    return "Set";
  case RuntimeNativeTypeKind::Map:
    return "Map";
  case RuntimeNativeTypeKind::Null:
    return "Null";
  case RuntimeNativeTypeKind::Object:
    return "Object";
  }
  return "NativeType";
}

const char *native_function_name(RuntimeNativeFunctionKind kind) {
  switch (kind) {
  case RuntimeNativeFunctionKind::Print:
    return "print";
  case RuntimeNativeFunctionKind::P:
    return "p";
  case RuntimeNativeFunctionKind::Pp:
    return "pp";
  }
  return "native_function";
}

struct RuntimeStringifyContext {
  const BcModule *module = nullptr;
  const std::vector<std::string> *runtime_strings = nullptr;
  const std::vector<std::string> *runtime_symbols = nullptr;
  RuntimePrettyPrintOptions options;
  std::unordered_set<const void *> active;
};

const std::vector<std::string> *
string_table_for(const RuntimeStringifyContext &context) {
  return context.runtime_strings != nullptr && !context.runtime_strings->empty()
             ? context.runtime_strings
             : (context.module == nullptr ? nullptr : &context.module->strings);
}

const std::vector<std::string> *
symbol_table_for(const RuntimeStringifyContext &context) {
  return context.runtime_symbols != nullptr && !context.runtime_symbols->empty()
             ? context.runtime_symbols
             : (context.module == nullptr ? nullptr : &context.module->symbols);
}

std::optional<std::string>
string_text_for(const RuntimeStringifyContext &context,
                std::uint32_t string_id) {
  const std::vector<std::string> *strings = string_table_for(context);
  if (strings == nullptr || string_id >= strings->size()) {
    return std::nullopt;
  }
  return (*strings)[string_id];
}

std::optional<std::string>
symbol_text_for(const RuntimeStringifyContext &context,
                std::uint32_t symbol_id) {
  const std::vector<std::string> *symbols = symbol_table_for(context);
  if (symbols == nullptr || symbol_id >= symbols->size()) {
    return std::nullopt;
  }
  return (*symbols)[symbol_id];
}

std::string escape_string_literal(const std::string &text) {
  std::string out;
  out.reserve(text.size() + 2U);
  for (unsigned char c : text) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20U) {
        constexpr char hex[] = "0123456789abcdef";
        out += "\\x";
        out.push_back(hex[(c >> 4U) & 0xFU]);
        out.push_back(hex[c & 0xFU]);
      } else {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  return out;
}

std::string indent_text(std::size_t depth) {
  return std::string(depth * 2U, ' ');
}

std::string lifecycle_debug_label(const ObjHeader &header);

std::string type_label_for_cycle(const Value &value) {
  if (value.is_list()) {
    return "Array";
  }
  if (value.is_tuple()) {
    return "Tuple";
  }
  if (value.is_set()) {
    return "Set";
  }
  if (value.is_map()) {
    return "Map";
  }
  if (value.is_instance_object()) {
    return "Object";
  }
  if (value.is_closure()) {
    return "Closure";
  }
  return "Object";
}

const void *heap_identity_for(const Value &value) {
  if (value.is_list()) {
    return value.as_list().get();
  }
  if (value.is_tuple()) {
    return value.as_tuple().get();
  }
  if (value.is_set()) {
    return value.as_set().get();
  }
  if (value.is_map()) {
    return value.as_map().get();
  }
  if (value.is_instance_object()) {
    return value.as_instance_object().get();
  }
  if (value.is_closure()) {
    return value.as_closure().get();
  }
  return nullptr;
}

class RuntimeStringifyGuard {
public:
  RuntimeStringifyGuard(RuntimeStringifyContext *context, const void *identity)
      : context_(context), identity_(identity) {
    if (context_ != nullptr && identity_ != nullptr) {
      inserted_ = context_->active.insert(identity_).second;
    }
  }

  RuntimeStringifyGuard(const RuntimeStringifyGuard &) = delete;
  RuntimeStringifyGuard &operator=(const RuntimeStringifyGuard &) = delete;

  ~RuntimeStringifyGuard() {
    if (context_ != nullptr && identity_ != nullptr && inserted_) {
      context_->active.erase(identity_);
    }
  }

  bool inserted() const { return inserted_; }

private:
  RuntimeStringifyContext *context_ = nullptr;
  const void *identity_ = nullptr;
  bool inserted_ = true;
};

std::string runtime_stringify_value_impl(RuntimeStringifyContext *context,
                                         const Value &value,
                                         RuntimeStringifyMode mode,
                                         std::size_t depth);

std::string compact_join_values(RuntimeStringifyContext *context,
                                const std::vector<Value> &items,
                                RuntimeStringifyMode mode, std::size_t depth) {
  std::ostringstream out;
  const std::size_t limit = std::min(items.size(), context->options.max_items);
  for (std::size_t i = 0; i < limit; ++i) {
    if (i != 0U) {
      out << ", ";
    }
    out << runtime_stringify_value_impl(context, items[i], mode, depth + 1U);
  }
  if (items.size() > limit) {
    if (limit != 0U) {
      out << ", ";
    }
    out << "... " << (items.size() - limit) << " more";
  }
  return out.str();
}

std::string pretty_join_values(RuntimeStringifyContext *context,
                               const std::vector<Value> &items,
                               RuntimeStringifyMode mode, std::size_t depth) {
  std::ostringstream out;
  const std::size_t limit = std::min(items.size(), context->options.max_items);
  for (std::size_t i = 0; i < limit; ++i) {
    out << indent_text(depth + 1U)
        << runtime_stringify_value_impl(context, items[i], mode, depth + 1U)
        << ",\n";
  }
  if (items.size() > limit) {
    out << indent_text(depth + 1U) << "... " << (items.size() - limit)
        << " more,\n";
  }
  return out.str();
}

std::string runtime_stringify_value_impl(RuntimeStringifyContext *context,
                                         const Value &value,
                                         RuntimeStringifyMode mode,
                                         std::size_t depth) {
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
    const std::optional<std::string> text =
        symbol_text_for(*context, value.as_symbol().symbol_id);
    if (!text.has_value()) {
      return mode == RuntimeStringifyMode::Display ? "<invalid-symbol>"
                                                   : ":<invalid>";
    }
    return mode == RuntimeStringifyMode::Display ? *text : ":" + *text;
  }
  if (value.is_string()) {
    const std::optional<std::string> text =
        string_text_for(*context, value.as_string().string_id);
    if (!text.has_value()) {
      return mode == RuntimeStringifyMode::Display ? "<invalid-string>"
                                                   : "\"<invalid>\"";
    }
    if (mode == RuntimeStringifyMode::Display) {
      return *text;
    }
    return "\"" + escape_string_literal(*text) + "\"";
  }
  if (value.is_native_type()) {
    return std::string("<type ") +
           native_type_name(value.as_native_type().kind) + ">";
  }
  if (value.is_native_function()) {
    return std::string("<function ") +
           native_function_name(value.as_native_function().kind) + ">";
  }
  if (value.is_text_writer()) {
    const std::shared_ptr<RuntimeTextWriter> writer = value.as_text_writer();
    if (writer == nullptr) {
      return "<io.TextWriter null>";
    }
    if (writer->buffered()) {
      return "<io.Buffer>";
    }
    const std::string stream = writer->stream_name();
    return stream.empty() ? "<io.TextWriter>"
                          : "<io.TextWriter " + stream + ">";
  }
  if (value.is_logger()) {
    return value.as_logger() == nullptr ? "<io.Logger null>" : "<io.Logger>";
  }

  const void *identity = heap_identity_for(value);
  if (identity != nullptr &&
      context->active.find(identity) != context->active.end()) {
    return "#<cycle " + type_label_for_cycle(value) + ">";
  }
  if (identity != nullptr && depth >= context->options.max_depth) {
    return "#<max-depth " + type_label_for_cycle(value) + ">";
  }
  RuntimeStringifyGuard guard(context, identity);
  if (identity != nullptr && !guard.inserted()) {
    return "#<cycle " + type_label_for_cycle(value) + ">";
  }

  if (value.is_class_object()) {
    const ClassObjectValue klass = value.as_class_object();
    std::ostringstream out;
    out << "<class";
    if (context->module != nullptr &&
        klass.class_index < context->module->classes.size()) {
      const std::uint32_t symbol_id =
          context->module->classes[klass.class_index].class_name_sym_id;
      const std::optional<std::string> name =
          symbol_text_for(*context, symbol_id);
      out << " " << (name.has_value() ? *name : "?");
    } else {
      out << " #" << klass.class_index;
    }
    out << ">";
    return out.str();
  }
  if (value.is_watch_cell()) {
    const std::shared_ptr<RuntimeWatchCell> cell = value.as_watch_cell();
    if (cell == nullptr) {
      return "<watch-cell null>";
    }
    const RuntimeWatchCellSnapshot snapshot = cell->snapshot();
    std::ostringstream out;
    out << "<watch-cell #" << snapshot.cell_id << " r" << snapshot.revision
        << " "
        << runtime_stringify_value_impl(context, snapshot.value, mode,
                                        depth + 1U)
        << ">";
    return out.str();
  }
  if (value.is_watch_handle()) {
    const std::shared_ptr<RuntimeWatchHandle> handle = value.as_watch_handle();
    if (handle == nullptr) {
      return "<watch null>";
    }
    const RuntimeWatchCellSnapshot snapshot = handle->snapshot();
    std::ostringstream out;
    out << "<watch #" << handle->handle_id();
    if (snapshot.cell_id != 0) {
      out << " cell:" << snapshot.cell_id << " r" << snapshot.revision;
    }
    out << ">";
    return out.str();
  }
  if (value.is_closure()) {
    const std::shared_ptr<ClosureValue> closure = value.as_closure();
    if (closure == nullptr) {
      return "<closure null>";
    }
    const std::string lifecycle = lifecycle_debug_label(closure->header);
    return lifecycle.empty()
               ? "<closure c" + std::to_string(closure->code_id) + ">"
               : "<" + lifecycle + " closure>";
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
    if (context->module != nullptr &&
        instance->class_index < context->module->classes.size()) {
      const std::uint32_t symbol_id =
          context->module->classes[instance->class_index].class_name_sym_id;
      const std::optional<std::string> name =
          symbol_text_for(*context, symbol_id);
      out << " " << (name.has_value() ? *name : "?");
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
    if (mode == RuntimeStringifyMode::Pretty && !list->items.empty()) {
      std::ostringstream out;
      out << "[\n"
          << pretty_join_values(context, list->items, mode, depth)
          << indent_text(depth) << "]";
      return out.str();
    }
    return "[" + compact_join_values(context, list->items, mode, depth) + "]";
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
    if (mode == RuntimeStringifyMode::Pretty && !tuple->items.empty()) {
      std::ostringstream out;
      out << "(\n"
          << pretty_join_values(context, tuple->items, mode, depth)
          << indent_text(depth) << ")";
      return out.str();
    }
    return "(" + compact_join_values(context, tuple->items, mode, depth) + ")";
  }
  if (value.is_set()) {
    const std::shared_ptr<SetValue> set = value.as_set();
    if (set == nullptr) {
      return "{<null-set>}";
    }
    const std::string lifecycle = lifecycle_debug_label(set->header);
    if (!lifecycle.empty()) {
      return "{<" + lifecycle + "-set>}";
    }
    if (set->items.empty()) {
      return "Set{}";
    }
    if (mode == RuntimeStringifyMode::Pretty) {
      std::ostringstream out;
      out << "{\n"
          << pretty_join_values(context, set->items, mode, depth)
          << indent_text(depth) << "}";
      return out.str();
    }
    std::string body = compact_join_values(context, set->items, mode, depth);
    if (set->items.size() == 1U) {
      body += ",";
    }
    return "{" + body + "}";
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
    if (map->entries.empty()) {
      return "{}";
    }
    std::ostringstream out;
    if (mode == RuntimeStringifyMode::Pretty) {
      out << "{\n";
      const std::size_t limit =
          std::min(map->entries.size(), context->options.max_items);
      for (std::size_t i = 0; i < limit; ++i) {
        const std::optional<std::string> key =
            symbol_text_for(*context, map->entries[i].symbol_id);
        out << indent_text(depth + 1U)
            << (key.has_value()
                    ? *key
                    : "#" + std::to_string(map->entries[i].symbol_id))
            << ": "
            << runtime_stringify_value_impl(context, map->entries[i].value,
                                            mode, depth + 1U)
            << ",\n";
      }
      if (map->entries.size() > limit) {
        out << indent_text(depth + 1U) << "... "
            << (map->entries.size() - limit) << " more,\n";
      }
      out << indent_text(depth) << "}";
      return out.str();
    }
    out << "{";
    const std::size_t limit =
        std::min(map->entries.size(), context->options.max_items);
    for (std::size_t i = 0; i < limit; ++i) {
      if (i != 0U) {
        out << ", ";
      }
      const std::optional<std::string> key =
          symbol_text_for(*context, map->entries[i].symbol_id);
      out << (key.has_value() ? *key
                              : "#" + std::to_string(map->entries[i].symbol_id))
          << ": "
          << runtime_stringify_value_impl(context, map->entries[i].value, mode,
                                          depth + 1U);
    }
    if (map->entries.size() > limit) {
      if (limit != 0U) {
        out << ", ";
      }
      out << "... " << (map->entries.size() - limit) << " more";
    }
    out << "}";
    return out.str();
  }
  return "<unknown>";
}

std::string runtime_stringify_value(
    const Value &value, RuntimeStringifyMode mode,
    const BcModule *module = nullptr,
    const std::vector<std::string> *runtime_strings = nullptr,
    const std::vector<std::string> *runtime_symbols = nullptr,
    RuntimePrettyPrintOptions options = {}) {
  RuntimeStringifyContext context;
  context.module = module;
  context.runtime_strings = runtime_strings;
  context.runtime_symbols = runtime_symbols;
  context.options = options;
  return runtime_stringify_value_impl(&context, value, mode, 0);
}

bool value_has_heap_payload_tag(const Value &value) {
  return value.is_closure() || value.is_instance_object() || value.is_list() ||
         value.is_tuple() || value.is_set() || value.is_map();
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
  if (value.is_set()) {
    const std::shared_ptr<SetValue> object = value.as_set();
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
  if (value.is_set()) {
    const std::shared_ptr<SetValue> object = value.as_set();
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

enum class LazySeqOpKind : std::int64_t {
  Map = 1,
  FlatMap = 2,
  Select = 3,
  Reject = 4,
};

struct LazySeqOp {
  LazySeqOpKind kind = LazySeqOpKind::Map;
  Value block = Value::null();
};

struct LazySeqState {
  Value source = Value::null();
  std::vector<LazySeqOp> ops;
};

enum class LazySeqVisitStatus { Continue, Stop, Faulted };

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

enum class QuickOpcode : std::uint8_t {
  Fallback,
  LoadK,
  LoadNull,
  LoadBool,
  Move,
  LoadSelf,
  GetLast,
  SetLast,
  LoadUpval,
  StoreUpval,
  IAdd,
  ISub,
  ILt,
  IGt,
  IMul,
  IDiv,
  IMod,
  IFloorDiv,
  ILe,
  IGe,
  IEq,
  INe,
  ICmp,
  IAddK,
  ISubK,
  ILtK,
  IGtK,
  IMulK,
  IDivK,
  IModK,
  IFloorDivK,
  ILeK,
  IGeK,
  IEqK,
  INeK,
  ICmpK,
  Jump,
  JumpIfTrue,
  JumpIfFalse,
  JumpIfNull,
  Return,
  Raise,
  CloseUpvalues,
  Safepoint,
  ILtJumpIfFalse,
  IGtJumpIfFalse,
  ILtKJumpIfFalse,
  IGtKJumpIfFalse,
  SendIAdd,
  SendISub,
  SendIMul,
  SendIDiv,
  SendIMod,
  SendIFloorDiv,
  SendILt,
  SendIGt,
  SendILe,
  SendIGe,
  SendIEq,
  SendINe,
  SendICmp,
  SendSeqIndex,
  SendSeqCount,
  SendSeqFirst,
};

struct QuickInsn {
  QuickOpcode quick_opcode = QuickOpcode::Fallback;
  Opcode opcode = Opcode::Return;
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  std::uint32_t c = 0;
  std::int64_t imm = 0;
};

struct QuickCode {
  std::vector<QuickInsn> instructions;
};

struct Frame {
  const BcCode *code = nullptr;
  const QuickCode *quick_code = nullptr;
  std::size_t pc = 0;
  std::vector<Value> regs;
  std::vector<std::uint8_t> initialized;
  std::vector<std::int64_t> int64_regs;
  std::vector<std::uint8_t> int_valid;
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
  std::uint64_t watch_epoch = 0;
  std::uint64_t next_watch_cell_id = 1;
  std::uint64_t next_watch_handle_id = 1;
  std::vector<RuntimeWatchEvent> watch_events;
  bool dependency_capture_active = false;
  RuntimeDependencySet dependency_capture;
  std::unordered_map<std::string, std::size_t> dependency_capture_index;
  std::unordered_map<std::uint64_t, CallCacheEntry> call_caches;
  std::uint64_t call_cache_hits = 0;
  std::uint64_t call_cache_misses = 0;
  std::uint64_t call_cache_updates = 0;
  std::unordered_map<std::uint64_t, IvarCacheEntry> ivar_caches;
  bool module_init_completed = false;
  std::unordered_map<std::string, Value> module_bindings;
  std::uint64_t next_shape_id = 1;
  std::vector<std::shared_ptr<ShapeDescriptor>> root_shapes;
  std::unordered_map<std::string, std::shared_ptr<ShapeDescriptor>>
      shape_transitions;
  std::shared_ptr<ShapeDescriptor> dead_shape;

  static std::string shape_transition_key(std::uint64_t parent_id,
                                          const std::string &name) {
    return std::to_string(parent_id) + "\x1f" + name;
  }

  std::shared_ptr<RuntimeWatchCell> make_watch_cell(Value value,
                                                    std::string target_name) {
    return std::make_shared<RuntimeWatchCell>(
        std::move(value), next_watch_cell_id++, std::move(target_name));
  }

  std::shared_ptr<RuntimeWatchHandle>
  make_watch_handle(std::shared_ptr<RuntimeWatchCell> cell,
                    std::string target_name) {
    return std::make_shared<RuntimeWatchHandle>(
        std::move(cell), next_watch_handle_id++, std::move(target_name));
  }

  std::shared_ptr<RuntimeWatchHandle>
  make_watch_handle(std::shared_ptr<RuntimeWatchObjectState> object_state,
                    std::string target_name, std::string field_name) {
    return std::make_shared<RuntimeWatchHandle>(
        std::move(object_state), next_watch_handle_id++, std::move(target_name),
        std::move(field_name));
  }

  RuntimeWatchEvent record_watch_event(RuntimeWatchEvent event) {
    event.watch_epoch = ++watch_epoch;
    watch_events.push_back(event);
    return event;
  }

  static std::string dependency_key(const RuntimeDependency &dependency) {
    switch (dependency.kind) {
    case RuntimeDependencyKind::Binding:
      return "binding:" + std::to_string(dependency.cell_id);
    case RuntimeDependencyKind::Ivar:
      return "ivar:" + std::to_string(dependency.object_id) + ":" +
             dependency.field_name;
    case RuntimeDependencyKind::Object:
      return "object:" + std::to_string(dependency.object_id);
    }
    return {};
  }

  void begin_dependency_capture(std::uint64_t notebook_cell_id) {
    dependency_capture_active = true;
    dependency_capture = RuntimeDependencySet{};
    dependency_capture.notebook_cell_id = notebook_cell_id;
    dependency_capture_index.clear();
  }

  RuntimeDependencySet end_dependency_capture() {
    RuntimeDependencySet result = dependency_capture;
    dependency_capture_active = false;
    dependency_capture = RuntimeDependencySet{};
    dependency_capture_index.clear();
    return result;
  }

  RuntimeDependencySet dependency_capture_snapshot() const {
    return dependency_capture_active ? dependency_capture
                                     : RuntimeDependencySet{};
  }

  void record_dependency(RuntimeDependency dependency) {
    if (!dependency_capture_active) {
      return;
    }
    const std::string key = dependency_key(dependency);
    if (key.empty()) {
      return;
    }
    const auto found = dependency_capture_index.find(key);
    if (found == dependency_capture_index.end()) {
      dependency_capture_index.emplace(key,
                                       dependency_capture.dependencies.size());
      dependency_capture.dependencies.push_back(std::move(dependency));
      return;
    }
    dependency_capture.dependencies[found->second] = std::move(dependency);
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
        MethodTableDescriptor &table = (method.flags & kMethodFlagClass) != 0U
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
        MethodTableDescriptor &table = (method.flags & kMethodFlagClass) != 0U
                                           ? runtime.class_method_table
                                           : runtime.instance_method_table;
        table.entries[method.selector_sym_id] = std::move(method);
      }
    }

    owners_initialized = true;
    call_caches.clear();
    ivar_caches.clear();
    module_init_completed = false;
    module_bindings.clear();
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

bool values_are_numeric(const Value &lhs, const Value &rhs) {
  return (lhs.is_integer() || lhs.is_float()) &&
         (rhs.is_integer() || rhs.is_float());
}

double numeric_value_as_double(const Value &value) {
  return value.is_integer() ? static_cast<double>(value.as_integer())
                            : value.as_float();
}

std::int64_t compare_int64(std::int64_t lhs, std::int64_t rhs) {
  if (lhs < rhs) {
    return -1;
  }
  if (lhs > rhs) {
    return 1;
  }
  return 0;
}

std::int64_t compare_double(double lhs, double rhs) {
  if (lhs < rhs) {
    return -1;
  }
  if (lhs > rhs) {
    return 1;
  }
  return 0;
}

std::int64_t floor_div_int64(std::int64_t lhs, std::int64_t rhs) {
  std::int64_t quotient = lhs / rhs;
  const std::int64_t remainder = lhs % rhs;
  if (remainder != 0 && ((remainder < 0) != (rhs < 0))) {
    --quotient;
  }
  return quotient;
}

std::int64_t floor_mod_int64(std::int64_t lhs, std::int64_t rhs) {
  return lhs - floor_div_int64(lhs, rhs) * rhs;
}

double floor_mod_double(double lhs, double rhs) {
  return lhs - std::floor(lhs / rhs) * rhs;
}

bool value_equals(const Value &lhs, const Value &rhs) {
  if (lhs.is_watch_cell() || rhs.is_watch_cell()) {
    return value_equals(unwrap_watch_value(lhs), unwrap_watch_value(rhs));
  }
  if (values_are_numeric(lhs, rhs)) {
    if (lhs.is_integer() && rhs.is_integer()) {
      return lhs.as_integer() == rhs.as_integer();
    }
    return numeric_value_as_double(lhs) == numeric_value_as_double(rhs);
  }
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
  if (lhs.is_native_type()) {
    return lhs.as_native_type().kind == rhs.as_native_type().kind;
  }
  if (lhs.is_native_function()) {
    return lhs.as_native_function().kind == rhs.as_native_function().kind;
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
  if (lhs.is_watch_handle()) {
    return lhs.as_watch_handle() == rhs.as_watch_handle();
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
  if (lhs.is_set()) {
    const std::shared_ptr<SetValue> left = lhs.as_set();
    const std::shared_ptr<SetValue> right = rhs.as_set();
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    if (left->items.size() != right->items.size()) {
      return false;
    }
    std::vector<bool> matched(right->items.size(), false);
    for (const Value &left_item : left->items) {
      bool found = false;
      for (std::size_t i = 0; i < right->items.size(); ++i) {
        if (!matched[i] && value_equals(left_item, right->items[i])) {
          matched[i] = true;
          found = true;
          break;
        }
      }
      if (!found) {
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

enum class FastSendStatus { Matched, NotHandled, Faulted };

enum class FastCallStatus { Matched, NotHandled, Faulted };

enum class DirectClosureKind : std::uint8_t {
  None,
  SequenceIndex,
  LaneIndex,
  WrapSubtract,
  MixLinearWrap,
  ScoreRow,
};

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

struct FastCallArg {
  Value value = Value::null();
  std::int64_t int_value = 0;
  bool int_valid = false;
};

struct NestedExecution {
  Value value = Value::null();
  std::vector<Value> regs;
  std::vector<std::uint8_t> initialized;
  std::optional<Fault> fault;

  bool ok() const { return !fault.has_value(); }
};

struct DirectEntryClosure {
  std::vector<Value> captures;
  Value self = Value::null();
};

class Vm {
public:
  explicit Vm(const BcModule &module,
              std::shared_ptr<RuntimeState> state = nullptr,
              std::string module_id = {})
      : module_(module), initial_string_count_(module.strings.size()),
        initial_symbol_count_(module.symbols.size()),
        state_(state == nullptr ? std::make_shared<RuntimeState>()
                                : std::move(state)),
        module_id_(std::move(module_id)) {
    state_->initialize_for_module(module_);
  }

  ExecutionResult execute(std::uint32_t code_id, const std::vector<Value> &args,
                          Value self, Value block) {
    const std::size_t watch_event_start = state_->watch_events.size();
    const BcCode *entry = find_code(module_, code_id);
    if (entry == nullptr) {
      return with_runtime_names(fail("VMError", "unknown code id", code_id, 0));
    }
    std::vector<Value> entry_captures;
    if (!prepare_direct_entry_captures(*entry, code_id, &entry_captures,
                                       &self)) {
      state_->heap.drain_remote_frees();
      return with_runtime_names({Value::null(), fault_});
    }
    push_frame(*entry, args, std::move(entry_captures), std::move(self),
               std::move(block), std::nullopt);
    while (fault_ == std::nullopt && !frames_.empty()) {
      step();
    }
    state_->heap.drain_remote_frees();
    std::vector<RuntimeWatchEvent> watch_events;
    if (watch_event_start <= state_->watch_events.size()) {
      watch_events.assign(state_->watch_events.begin() +
                              static_cast<std::ptrdiff_t>(watch_event_start),
                          state_->watch_events.end());
    }
    if (fault_.has_value()) {
      return with_runtime_names({Value::null(),
                                 fault_,
                                 {},
                                 std::move(watch_events),
                                 state_->watch_epoch});
    }
    return with_runtime_names({final_value_, std::nullopt,
                               completed_locals_for(*entry),
                               std::move(watch_events), state_->watch_epoch});
  }

private:
  ExecutionResult with_runtime_names(ExecutionResult result) const {
    if (module_.strings.size() != initial_string_count_) {
      result.runtime_strings = module_.strings;
    }
    if (module_.symbols.size() != initial_symbol_count_) {
      result.runtime_symbols = module_.symbols;
    }
    return result;
  }

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

  std::string local_name_for_slot(const BcCode &code,
                                  std::uint32_t slot) const {
    for (const SlotLayoutEntry &entry : code.local_layout) {
      if (entry.slot == slot) {
        return string_or_empty(entry.name_str_id);
      }
    }
    return "l" + std::to_string(slot);
  }

  std::string capture_name_for_slot(const BcCode &code,
                                    std::uint32_t slot) const {
    for (const bytecode::CaptureLayoutEntry &entry : code.capture_layout) {
      if (entry.slot == slot) {
        return string_or_empty(entry.name_str_id);
      }
    }
    return "u" + std::to_string(slot);
  }

  void persist_module_bindings(const BcCode &code,
                               const std::vector<Value> &regs,
                               const std::vector<std::uint8_t> &initialized) {
    if (code.kind != CodeKind::Module) {
      return;
    }
    state_->module_bindings.clear();
    for (const SlotLayoutEntry &entry : code.local_layout) {
      if (entry.slot >= regs.size() || entry.slot >= initialized.size() ||
          initialized[entry.slot] == 0U) {
        continue;
      }
      const std::string name = string_or_empty(entry.name_str_id);
      if (name.empty()) {
        continue;
      }
      state_->module_bindings[name] = regs[entry.slot];
    }
    state_->module_init_completed = true;
  }

  std::optional<DirectEntryClosure>
  module_closure_for_code(std::uint32_t code_id) const {
    for (const auto &[name, storage] : state_->module_bindings) {
      (void)name;
      const Value value = unwrap_watch_value(storage);
      if (!value.is_closure()) {
        continue;
      }
      const std::shared_ptr<ClosureValue> closure = value.as_closure();
      if (closure != nullptr && closure->code_id == code_id) {
        return DirectEntryClosure{closure->captures, closure->self};
      }
    }
    return std::nullopt;
  }

  bool prepare_direct_entry_captures(const BcCode &entry, std::uint32_t code_id,
                                     std::vector<Value> *captures,
                                     Value *self) {
    captures->clear();
    if (entry.capture_layout.empty()) {
      return true;
    }

    if (const std::optional<DirectEntryClosure> closure =
            module_closure_for_code(code_id)) {
      *captures = closure->captures;
      *self = closure->self;
      return true;
    }

    if (!state_->module_init_completed && module_.init.has_entry_code_id &&
        module_.init.entry_code_id != code_id) {
      Vm init_vm(module_, state_, module_id_);
      ExecutionResult init_result = init_vm.execute(
          module_.init.entry_code_id, {}, Value::null(), Value::null());
      if (!init_result.ok()) {
        fault_ = init_result.fault;
        return false;
      }
    }

    if (const std::optional<DirectEntryClosure> closure =
            module_closure_for_code(code_id)) {
      *captures = closure->captures;
      *self = closure->self;
    }
    return true;
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
      local.initialized = entry.slot < last_completed_initialized_.size() &&
                          last_completed_initialized_[entry.slot] != 0U &&
                          entry.slot < last_completed_regs_.size();
      if (local.initialized) {
        const Value storage = last_completed_regs_[entry.slot];
        if (storage.is_watch_cell()) {
          const std::shared_ptr<RuntimeWatchCell> cell =
              storage.as_watch_cell();
          if (cell != nullptr) {
            const RuntimeWatchCellSnapshot snapshot = cell->snapshot();
            local.value = snapshot.value;
            local.watched = snapshot.watched;
            local.watch_cell_id = snapshot.cell_id;
            local.watch_revision = snapshot.revision;
          }
        } else {
          local.value = storage;
        }
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
    if (value.is_set()) {
      const std::shared_ptr<SetValue> set = value.as_set();
      if (set != nullptr) {
        set->items.clear();
        set->items.shrink_to_fit();
        set->frozen = false;
        set->header.shape = state_->dead_shape;
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

  Value make_set_value(std::vector<Value> items, bool frozen = false) {
    return state_->heap.make_set_value(std::move(items), frozen);
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
    if (value.is_watch_cell()) {
      const std::shared_ptr<RuntimeWatchCell> cell = value.as_watch_cell();
      if (cell != nullptr) {
        append_value_root(roots, cell->read());
      }
      return;
    }
    if (value.is_watch_handle()) {
      const std::shared_ptr<RuntimeWatchHandle> handle =
          value.as_watch_handle();
      if (handle != nullptr && handle->cell() != nullptr) {
        append_value_root(roots, handle->cell()->read());
      }
      return;
    }
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

  std::vector<Value> collect_gc_roots() {
    std::vector<Value> roots;
    for (Frame &frame : frames_) {
      materialize_integer_regs(frame);
      append_frame_roots(&roots, frame);
    }
    for (std::size_t index = 0; index < last_completed_regs_.size(); ++index) {
      if (index < last_completed_initialized_.size() &&
          last_completed_initialized_[index] != 0U) {
        append_value_root(&roots, last_completed_regs_[index]);
      }
    }
    append_value_root(&roots, final_value_);
    for (const auto &[name, value] : state_->module_bindings) {
      (void)name;
      append_value_root(&roots, value);
    }
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

  static bool quick_operand_u32(const Instruction &insn, std::size_t idx,
                                std::uint32_t *out) {
    if (idx >= insn.operands.size()) {
      return false;
    }
    const std::int64_t value = insn.operands[idx].value;
    if (value < 0) {
      return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
  }

  static bool quick_operand_i64(const Instruction &insn, std::size_t idx,
                                std::int64_t *out) {
    if (idx >= insn.operands.size()) {
      return false;
    }
    *out = insn.operands[idx].value;
    return true;
  }

  static bool quick_operand_reg_equals(const Instruction &insn, std::size_t idx,
                                       std::uint32_t reg) {
    std::uint32_t value = 0;
    return quick_operand_u32(insn, idx, &value) && value == reg;
  }

  static bool quick_register_range_contains(std::uint32_t first,
                                            std::uint32_t count,
                                            std::uint32_t reg) {
    return count != 0U && reg >= first && reg - first < count;
  }

  static bool quick_register_is_debug_local(const BcCode &code,
                                            std::uint32_t reg) {
    for (const SlotLayoutEntry &entry : code.local_layout) {
      if (entry.slot == reg) {
        return true;
      }
    }
    return false;
  }

  static bool quick_instruction_reads_reg(const BcCode &code,
                                          const Instruction &insn,
                                          std::uint32_t reg) {
    (void)code;
    switch (insn.opcode) {
    case Opcode::LoadK:
    case Opcode::LoadNull:
    case Opcode::LoadBool:
    case Opcode::LoadSelf:
    case Opcode::GetLast:
    case Opcode::LoadUpval:
    case Opcode::LookupConst:
    case Opcode::WatchUpval:
    case Opcode::CloseUpvalues:
    case Opcode::Jump:
    case Opcode::Safepoint:
    case Opcode::PCommit:
    case Opcode::PFail:
      return false;
    case Opcode::Move:
    case Opcode::Freeze:
    case Opcode::ObjDestroy:
    case Opcode::ObjDealloc:
    case Opcode::SetLast:
    case Opcode::Raise:
    case Opcode::Return:
    case Opcode::TypeCheck:
      return quick_operand_reg_equals(insn, 0, reg);
    case Opcode::StoreUpval:
      return quick_operand_reg_equals(insn, 1, reg);
    case Opcode::LoadIvar:
    case Opcode::LoadCvar:
    case Opcode::WatchIvar:
      return quick_operand_reg_equals(insn, 1, reg);
    case Opcode::StoreIvar:
    case Opcode::StoreCvar:
      return quick_operand_reg_equals(insn, 0, reg) ||
             quick_operand_reg_equals(insn, 2, reg);
    case Opcode::TripleEq:
    case Opcode::InOp:
      return quick_operand_reg_equals(insn, 1, reg) ||
             quick_operand_reg_equals(insn, 2, reg);
    case Opcode::MakeList:
    case Opcode::MakeSet:
    case Opcode::MakeTuple: {
      std::uint32_t first_reg = 0;
      std::uint32_t count = 0;
      return quick_operand_u32(insn, 1, &first_reg) &&
             quick_operand_u32(insn, 2, &count) &&
             quick_register_range_contains(first_reg, count, reg);
    }
    case Opcode::MakeMap: {
      std::uint32_t count = 0;
      if (!quick_operand_u32(insn, 1, &count)) {
        return true;
      }
      std::size_t operand_index = 2;
      for (std::uint32_t i = 0; i < count; ++i) {
        ++operand_index;
        if (quick_operand_reg_equals(insn, operand_index++, reg)) {
          return true;
        }
      }
      return false;
    }
    case Opcode::MakeClosure: {
      std::uint32_t dst = 0;
      std::uint32_t capture_count = 0;
      if (!quick_operand_u32(insn, 0, &dst) ||
          !quick_operand_u32(insn, 2, &capture_count)) {
        return true;
      }
      std::size_t operand_index = 3;
      for (std::uint32_t i = 0; i < capture_count; ++i) {
        std::uint32_t kind = 0;
        std::uint32_t slot = 0;
        if (!quick_operand_u32(insn, operand_index++, &kind) ||
            !quick_operand_u32(insn, operand_index++, &slot)) {
          return true;
        }
        if (kind == 0U && slot != dst && slot == reg) {
          return true;
        }
      }
      return false;
    }
    case Opcode::WatchLocal:
      return quick_operand_reg_equals(insn, 1, reg);
    case Opcode::Send:
    case Opcode::SendDyn:
    case Opcode::Call: {
      const bool is_call = insn.opcode == Opcode::Call;
      const bool is_dynamic = insn.opcode == Opcode::SendDyn;
      std::size_t operand_index = 0;
      ++operand_index;
      if (quick_operand_reg_equals(insn, operand_index++, reg)) {
        return true;
      }
      if (!is_call) {
        if (is_dynamic && quick_operand_reg_equals(insn, operand_index, reg)) {
          return true;
        }
        ++operand_index;
      }
      std::uint32_t pos_count = 0;
      if (!quick_operand_u32(insn, operand_index++, &pos_count)) {
        return true;
      }
      for (std::uint32_t i = 0; i < pos_count; ++i) {
        if (quick_operand_reg_equals(insn, operand_index++, reg)) {
          return true;
        }
      }
      std::uint32_t kw_count = 0;
      if (!quick_operand_u32(insn, operand_index++, &kw_count)) {
        return true;
      }
      for (std::uint32_t i = 0; i < kw_count; ++i) {
        ++operand_index;
        if (quick_operand_reg_equals(insn, operand_index++, reg)) {
          return true;
        }
      }
      std::int64_t block_reg = -1;
      if (!quick_operand_i64(insn, operand_index++, &block_reg)) {
        return true;
      }
      return block_reg >= 0 &&
             block_reg != static_cast<std::int64_t>(
                              std::numeric_limits<std::uint32_t>::max()) &&
             static_cast<std::uint32_t>(block_reg) == reg;
    }
    case Opcode::IAdd:
    case Opcode::ISub:
    case Opcode::ILt:
    case Opcode::IGt:
    case Opcode::IMul:
    case Opcode::IDiv:
    case Opcode::IMod:
    case Opcode::IFloorDiv:
    case Opcode::ILe:
    case Opcode::IGe:
    case Opcode::IEq:
    case Opcode::INe:
    case Opcode::ICmp:
      return quick_operand_reg_equals(insn, 1, reg) ||
             quick_operand_reg_equals(insn, 2, reg);
    case Opcode::IAddK:
    case Opcode::ISubK:
    case Opcode::ILtK:
    case Opcode::IGtK:
    case Opcode::IMulK:
    case Opcode::IDivK:
    case Opcode::IModK:
    case Opcode::IFloorDivK:
    case Opcode::ILeK:
    case Opcode::IGeK:
    case Opcode::IEqK:
    case Opcode::INeK:
    case Opcode::ICmpK:
      return quick_operand_reg_equals(insn, 1, reg);
    case Opcode::JumpIfTrue:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull:
      return quick_operand_reg_equals(insn, 0, reg);
    case Opcode::PPrepSeq:
    case Opcode::PPrepMap:
      return quick_operand_reg_equals(insn, 1, reg);
    case Opcode::PCheckEq:
    case Opcode::PCheckLenEq:
    case Opcode::PCheckLenGte:
    case Opcode::PHasKey:
      return quick_operand_reg_equals(insn, 0, reg);
    case Opcode::PCheckPin:
    case Opcode::PTripleEq:
      return quick_operand_reg_equals(insn, 0, reg) ||
             quick_operand_reg_equals(insn, 1, reg);
    case Opcode::PGetIndex:
    case Opcode::PGetKey:
    case Opcode::PBind:
      return quick_operand_reg_equals(insn, 1, reg);
    }
    return true;
  }

  static bool quick_instruction_writes_reg(const Instruction &insn,
                                           std::uint32_t reg) {
    switch (insn.opcode) {
    case Opcode::LoadK:
    case Opcode::LoadNull:
    case Opcode::LoadBool:
    case Opcode::Move:
    case Opcode::LoadSelf:
    case Opcode::GetLast:
    case Opcode::MakeList:
    case Opcode::MakeTuple:
    case Opcode::MakeMap:
    case Opcode::Freeze:
    case Opcode::MakeSet:
    case Opcode::LoadUpval:
    case Opcode::LoadIvar:
    case Opcode::LoadCvar:
    case Opcode::LookupConst:
    case Opcode::MakeClosure:
    case Opcode::ObjDestroy:
    case Opcode::ObjDealloc:
    case Opcode::WatchLocal:
    case Opcode::WatchUpval:
    case Opcode::WatchIvar:
    case Opcode::Send:
    case Opcode::SendDyn:
    case Opcode::Call:
    case Opcode::InOp:
    case Opcode::TripleEq:
    case Opcode::IAdd:
    case Opcode::ISub:
    case Opcode::ILt:
    case Opcode::IGt:
    case Opcode::IMul:
    case Opcode::IDiv:
    case Opcode::IMod:
    case Opcode::IFloorDiv:
    case Opcode::ILe:
    case Opcode::IGe:
    case Opcode::IEq:
    case Opcode::INe:
    case Opcode::ICmp:
    case Opcode::IAddK:
    case Opcode::ISubK:
    case Opcode::ILtK:
    case Opcode::IGtK:
    case Opcode::IMulK:
    case Opcode::IDivK:
    case Opcode::IModK:
    case Opcode::IFloorDivK:
    case Opcode::ILeK:
    case Opcode::IGeK:
    case Opcode::IEqK:
    case Opcode::INeK:
    case Opcode::ICmpK:
    case Opcode::PPrepSeq:
    case Opcode::PPrepMap:
    case Opcode::PGetIndex:
    case Opcode::PGetKey:
      return quick_operand_reg_equals(insn, 0, reg);
    case Opcode::PCommit: {
      std::uint32_t base_slot = 0;
      std::uint32_t count = 0;
      return quick_operand_u32(insn, 0, &base_slot) &&
             quick_operand_u32(insn, 1, &count) &&
             quick_register_range_contains(base_slot, count, reg);
    }
    case Opcode::StoreUpval:
    case Opcode::StoreIvar:
    case Opcode::StoreCvar:
    case Opcode::CloseUpvalues:
    case Opcode::SetLast:
    case Opcode::Jump:
    case Opcode::JumpIfTrue:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull:
    case Opcode::Return:
    case Opcode::Raise:
    case Opcode::Safepoint:
    case Opcode::PCheckEq:
    case Opcode::PCheckPin:
    case Opcode::PCheckLenEq:
    case Opcode::PCheckLenGte:
    case Opcode::PHasKey:
    case Opcode::PTripleEq:
    case Opcode::PBind:
    case Opcode::PFail:
    case Opcode::TypeCheck:
      return false;
    }
    return false;
  }

  static bool quick_add_target_successor(const BcCode &code,
                                         const Instruction &insn,
                                         std::size_t operand_index,
                                         std::vector<std::size_t> *out) {
    std::uint32_t target = 0;
    if (!quick_operand_u32(insn, operand_index, &target) ||
        target >= code.instructions.size()) {
      return false;
    }
    out->push_back(target);
    return true;
  }

  static bool quick_instruction_successors(const BcCode &code, std::size_t pc,
                                           const Instruction &insn,
                                           std::vector<std::size_t> *out) {
    const auto add_fallthrough = [&]() -> bool {
      if (pc + 1U < code.instructions.size()) {
        out->push_back(pc + 1U);
      }
      return true;
    };

    switch (insn.opcode) {
    case Opcode::Jump:
      return quick_add_target_successor(code, insn, 0, out);
    case Opcode::JumpIfTrue:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull:
      if (!add_fallthrough()) {
        return false;
      }
      return quick_add_target_successor(code, insn, 1, out);
    case Opcode::PPrepSeq:
      if (!add_fallthrough()) {
        return false;
      }
      return quick_add_target_successor(code, insn, 3, out);
    case Opcode::PPrepMap:
      if (!add_fallthrough()) {
        return false;
      }
      return quick_add_target_successor(code, insn, 4, out);
    case Opcode::PCheckEq:
    case Opcode::PCheckPin:
    case Opcode::PCheckLenEq:
    case Opcode::PCheckLenGte:
    case Opcode::PHasKey:
    case Opcode::PTripleEq:
      if (!add_fallthrough()) {
        return false;
      }
      return quick_add_target_successor(code, insn, 2, out);
    case Opcode::Return:
    case Opcode::Raise:
      return true;
    case Opcode::PFail: {
      std::int64_t mode = kPatternFailModeMatchError;
      if (!quick_operand_i64(insn, 0, &mode)) {
        return false;
      }
      if (mode == kPatternFailModeSoft) {
        return add_fallthrough();
      }
      return true;
    }
    default:
      return add_fallthrough();
    }
  }

  static bool quick_register_dead_from(const BcCode &code, std::uint32_t reg,
                                       std::size_t fallthrough,
                                       std::size_t target) {
    std::vector<std::size_t> pending;
    if (fallthrough < code.instructions.size()) {
      pending.push_back(fallthrough);
    }
    if (target < code.instructions.size()) {
      pending.push_back(target);
    } else {
      return false;
    }

    std::vector<std::uint8_t> seen(code.instructions.size(), 0U);
    while (!pending.empty()) {
      const std::size_t pc = pending.back();
      pending.pop_back();
      if (pc >= code.instructions.size() || seen[pc] != 0U) {
        continue;
      }
      seen[pc] = 1U;
      const Instruction &insn = code.instructions[pc];
      if (quick_instruction_reads_reg(code, insn, reg)) {
        return false;
      }
      if (quick_instruction_writes_reg(insn, reg)) {
        continue;
      }
      std::vector<std::size_t> successors;
      if (!quick_instruction_successors(code, pc, insn, &successors)) {
        return false;
      }
      pending.insert(pending.end(), successors.begin(), successors.end());
    }
    return true;
  }

  static QuickInsn quicken_instruction(const BcModule &module,
                                       const BcCode &code,
                                       const Instruction &insn) {
    QuickInsn out;
    out.opcode = insn.opcode;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::int64_t imm = 0;

    switch (insn.opcode) {
    case Opcode::LoadK:
      if (quick_operand_u32(insn, 0, &a) && quick_operand_u32(insn, 1, &b)) {
        out.quick_opcode = QuickOpcode::LoadK;
      }
      break;
    case Opcode::LoadNull:
      if (quick_operand_u32(insn, 0, &a)) {
        out.quick_opcode = QuickOpcode::LoadNull;
      }
      break;
    case Opcode::LoadBool:
      if (quick_operand_u32(insn, 0, &a) && quick_operand_i64(insn, 1, &imm)) {
        out.quick_opcode = QuickOpcode::LoadBool;
      }
      break;
    case Opcode::Move:
      if (quick_operand_u32(insn, 0, &a) && quick_operand_u32(insn, 1, &b)) {
        out.quick_opcode = QuickOpcode::Move;
      }
      break;
    case Opcode::LoadSelf:
      if (quick_operand_u32(insn, 0, &a)) {
        out.quick_opcode = QuickOpcode::LoadSelf;
      }
      break;
    case Opcode::GetLast:
      if (quick_operand_u32(insn, 0, &a)) {
        out.quick_opcode = QuickOpcode::GetLast;
      }
      break;
    case Opcode::SetLast:
      if (quick_operand_u32(insn, 0, &a)) {
        out.quick_opcode = QuickOpcode::SetLast;
      }
      break;
    case Opcode::LoadUpval:
      if (quick_operand_u32(insn, 0, &a) && quick_operand_u32(insn, 1, &b)) {
        out.quick_opcode = QuickOpcode::LoadUpval;
      }
      break;
    case Opcode::StoreUpval:
      if (quick_operand_u32(insn, 0, &a) && quick_operand_u32(insn, 1, &b)) {
        out.quick_opcode = QuickOpcode::StoreUpval;
      }
      break;
    case Opcode::IAdd:
    case Opcode::ISub:
    case Opcode::ILt:
    case Opcode::IGt:
    case Opcode::IMul:
    case Opcode::IDiv:
    case Opcode::IMod:
    case Opcode::IFloorDiv:
    case Opcode::ILe:
    case Opcode::IGe:
    case Opcode::IEq:
    case Opcode::INe:
    case Opcode::ICmp:
    case Opcode::IAddK:
    case Opcode::ISubK:
    case Opcode::ILtK:
    case Opcode::IGtK:
    case Opcode::IMulK:
    case Opcode::IDivK:
    case Opcode::IModK:
    case Opcode::IFloorDivK:
    case Opcode::ILeK:
    case Opcode::IGeK:
    case Opcode::IEqK:
    case Opcode::INeK:
    case Opcode::ICmpK:
      if (quick_operand_u32(insn, 0, &a) && quick_operand_u32(insn, 1, &b) &&
          quick_operand_u32(insn, 2, &c)) {
        switch (insn.opcode) {
        case Opcode::IAdd:
          out.quick_opcode = QuickOpcode::IAdd;
          break;
        case Opcode::ISub:
          out.quick_opcode = QuickOpcode::ISub;
          break;
        case Opcode::ILt:
          out.quick_opcode = QuickOpcode::ILt;
          break;
        case Opcode::IGt:
          out.quick_opcode = QuickOpcode::IGt;
          break;
        case Opcode::IMul:
          out.quick_opcode = QuickOpcode::IMul;
          break;
        case Opcode::IDiv:
          out.quick_opcode = QuickOpcode::IDiv;
          break;
        case Opcode::IMod:
          out.quick_opcode = QuickOpcode::IMod;
          break;
        case Opcode::IFloorDiv:
          out.quick_opcode = QuickOpcode::IFloorDiv;
          break;
        case Opcode::ILe:
          out.quick_opcode = QuickOpcode::ILe;
          break;
        case Opcode::IGe:
          out.quick_opcode = QuickOpcode::IGe;
          break;
        case Opcode::IEq:
          out.quick_opcode = QuickOpcode::IEq;
          break;
        case Opcode::INe:
          out.quick_opcode = QuickOpcode::INe;
          break;
        case Opcode::ICmp:
          out.quick_opcode = QuickOpcode::ICmp;
          break;
        case Opcode::IAddK:
          out.quick_opcode = QuickOpcode::IAddK;
          break;
        case Opcode::ISubK:
          out.quick_opcode = QuickOpcode::ISubK;
          break;
        case Opcode::ILtK:
          out.quick_opcode = QuickOpcode::ILtK;
          break;
        case Opcode::IGtK:
          out.quick_opcode = QuickOpcode::IGtK;
          break;
        case Opcode::IMulK:
          out.quick_opcode = QuickOpcode::IMulK;
          break;
        case Opcode::IDivK:
          out.quick_opcode = QuickOpcode::IDivK;
          break;
        case Opcode::IModK:
          out.quick_opcode = QuickOpcode::IModK;
          break;
        case Opcode::IFloorDivK:
          out.quick_opcode = QuickOpcode::IFloorDivK;
          break;
        case Opcode::ILeK:
          out.quick_opcode = QuickOpcode::ILeK;
          break;
        case Opcode::IGeK:
          out.quick_opcode = QuickOpcode::IGeK;
          break;
        case Opcode::IEqK:
          out.quick_opcode = QuickOpcode::IEqK;
          break;
        case Opcode::INeK:
          out.quick_opcode = QuickOpcode::INeK;
          break;
        case Opcode::ICmpK:
          out.quick_opcode = QuickOpcode::ICmpK;
          break;
        default:
          break;
        }
      }
      break;
    case Opcode::Jump:
      if (quick_operand_u32(insn, 0, &a)) {
        out.quick_opcode = QuickOpcode::Jump;
      }
      break;
    case Opcode::JumpIfTrue:
      if (quick_operand_u32(insn, 0, &a) && quick_operand_u32(insn, 1, &b)) {
        out.quick_opcode = QuickOpcode::JumpIfTrue;
      }
      break;
    case Opcode::JumpIfFalse:
      if (quick_operand_u32(insn, 0, &a) && quick_operand_u32(insn, 1, &b)) {
        out.quick_opcode = QuickOpcode::JumpIfFalse;
      }
      break;
    case Opcode::JumpIfNull:
      if (quick_operand_u32(insn, 0, &a) && quick_operand_u32(insn, 1, &b)) {
        out.quick_opcode = QuickOpcode::JumpIfNull;
      }
      break;
    case Opcode::Return:
      if (quick_operand_u32(insn, 0, &a)) {
        out.quick_opcode = QuickOpcode::Return;
      }
      break;
    case Opcode::Raise:
      if (quick_operand_u32(insn, 0, &a)) {
        out.quick_opcode = QuickOpcode::Raise;
      }
      break;
    case Opcode::CloseUpvalues:
      out.quick_opcode = QuickOpcode::CloseUpvalues;
      break;
    case Opcode::Safepoint:
      out.quick_opcode = QuickOpcode::Safepoint;
      break;
    case Opcode::Send: {
      std::uint32_t selector_id = 0;
      std::uint32_t pos_count = 0;
      if (!quick_operand_u32(insn, 0, &a) || !quick_operand_u32(insn, 1, &b) ||
          !quick_operand_u32(insn, 2, &selector_id) ||
          !quick_operand_u32(insn, 3, &pos_count) ||
          selector_id >= module.symbols.size() || pos_count > 1U) {
        break;
      }

      std::size_t operand_index = 4;
      if (pos_count == 1U && !quick_operand_u32(insn, operand_index++, &c)) {
        break;
      }

      std::uint32_t kw_count = 0;
      if (!quick_operand_u32(insn, operand_index++, &kw_count) ||
          kw_count != 0U) {
        break;
      }

      std::int64_t block_reg = -1;
      if (!quick_operand_i64(insn, operand_index++, &block_reg) ||
          (block_reg >= 0 &&
           block_reg != static_cast<std::int64_t>(
                            std::numeric_limits<std::uint32_t>::max()))) {
        break;
      }

      std::uint32_t site_flags = 0;
      std::uint32_t site_id = 0;
      if (quick_operand_u32(insn, operand_index++, &site_id) &&
          site_id < code.call_site_table.size()) {
        site_flags = code.call_site_table[site_id].flags;
      }
      if ((site_flags & (bytecode::kCallSiteFlagPropertyAccess |
                         bytecode::kCallSiteFlagPropertyAssignment)) != 0U) {
        break;
      }

      const std::string &selector = module.symbols[selector_id];
      const std::string collection_selector =
          canonical_collection_selector(selector);
      imm = static_cast<std::int64_t>(pos_count);
      if (pos_count == 1U) {
        if (selector == "+") {
          out.quick_opcode = QuickOpcode::SendIAdd;
          out.opcode = Opcode::IAdd;
        } else if (selector == "-") {
          out.quick_opcode = QuickOpcode::SendISub;
          out.opcode = Opcode::ISub;
        } else if (selector == "*") {
          out.quick_opcode = QuickOpcode::SendIMul;
          out.opcode = Opcode::IMul;
        } else if (selector == "/") {
          out.quick_opcode = QuickOpcode::SendIDiv;
          out.opcode = Opcode::IDiv;
        } else if (selector == "%") {
          out.quick_opcode = QuickOpcode::SendIMod;
          out.opcode = Opcode::IMod;
        } else if (selector == "//") {
          out.quick_opcode = QuickOpcode::SendIFloorDiv;
          out.opcode = Opcode::IFloorDiv;
        } else if (selector == "<") {
          out.quick_opcode = QuickOpcode::SendILt;
          out.opcode = Opcode::ILt;
        } else if (selector == ">") {
          out.quick_opcode = QuickOpcode::SendIGt;
          out.opcode = Opcode::IGt;
        } else if (selector == "<=") {
          out.quick_opcode = QuickOpcode::SendILe;
          out.opcode = Opcode::ILe;
        } else if (selector == ">=") {
          out.quick_opcode = QuickOpcode::SendIGe;
          out.opcode = Opcode::IGe;
        } else if (selector == "==") {
          out.quick_opcode = QuickOpcode::SendIEq;
          out.opcode = Opcode::IEq;
        } else if (selector == "!=") {
          out.quick_opcode = QuickOpcode::SendINe;
          out.opcode = Opcode::INe;
        } else if (selector == "<=>") {
          out.quick_opcode = QuickOpcode::SendICmp;
          out.opcode = Opcode::ICmp;
        } else if (collection_selector == "[]") {
          out.quick_opcode = QuickOpcode::SendSeqIndex;
        } else if (collection_selector == "first") {
          out.quick_opcode = QuickOpcode::SendSeqFirst;
        }
      } else {
        if (collection_selector == "count") {
          out.quick_opcode = QuickOpcode::SendSeqCount;
        } else if (collection_selector == "first") {
          out.quick_opcode = QuickOpcode::SendSeqFirst;
        }
      }
      break;
    }
    default:
      break;
    }

    out.a = a;
    out.b = b;
    out.c = c;
    out.imm = imm;
    return out;
  }

  static QuickCode build_quick_code(const BcModule &module,
                                    const BcCode &code) {
    QuickCode quick;
    quick.instructions.reserve(code.instructions.size());
    for (const Instruction &insn : code.instructions) {
      quick.instructions.push_back(quicken_instruction(module, code, insn));
    }

    for (std::size_t pc = 0; pc + 1U < quick.instructions.size(); ++pc) {
      QuickInsn &compare = quick.instructions[pc];
      const QuickInsn &branch = quick.instructions[pc + 1U];
      if (branch.quick_opcode != QuickOpcode::JumpIfFalse ||
          branch.a != compare.a ||
          quick_register_is_debug_local(code, compare.a) ||
          !quick_register_dead_from(code, compare.a, pc + 2U, branch.b)) {
        continue;
      }

      switch (compare.quick_opcode) {
      case QuickOpcode::ILt:
        compare.quick_opcode = QuickOpcode::ILtJumpIfFalse;
        break;
      case QuickOpcode::IGt:
        compare.quick_opcode = QuickOpcode::IGtJumpIfFalse;
        break;
      case QuickOpcode::ILtK:
        compare.quick_opcode = QuickOpcode::ILtKJumpIfFalse;
        break;
      case QuickOpcode::IGtK:
        compare.quick_opcode = QuickOpcode::IGtKJumpIfFalse;
        break;
      default:
        continue;
      }
      compare.a = quick.instructions[pc].b;
      compare.b = quick.instructions[pc].c;
      compare.c = branch.b;
    }

    return quick;
  }

  const QuickCode &quick_code_for(const BcCode &code) {
    const auto found = quick_codes_.find(code.code_id);
    if (found != quick_codes_.end()) {
      return found->second;
    }
    auto inserted =
        quick_codes_.emplace(code.code_id, build_quick_code(module_, code));
    return inserted.first->second;
  }

  bool instruction_selector_is(const Instruction &insn,
                               const std::string &selector) const {
    if (insn.opcode != Opcode::Send) {
      return false;
    }
    std::uint32_t selector_id = 0;
    return quick_operand_u32(insn, 2, &selector_id) &&
           selector_id < module_.symbols.size() &&
           module_.symbols[selector_id] == selector;
  }

  DirectClosureKind classify_direct_closure(const BcCode &code) const {
    if (code.kind != CodeKind::Method) {
      return DirectClosureKind::None;
    }

    if (code.capture_layout.empty() && code.instructions.size() == 3U &&
        instruction_selector_is(code.instructions[0], "[]") &&
        code.instructions[1].opcode == Opcode::CloseUpvalues &&
        code.instructions[2].opcode == Opcode::Return &&
        quick_operand_reg_equals(code.instructions[0], 0, 2) &&
        quick_operand_reg_equals(code.instructions[0], 1, 0) &&
        quick_operand_reg_equals(code.instructions[0], 4, 1) &&
        quick_operand_reg_equals(code.instructions[2], 0, 2)) {
      return DirectClosureKind::SequenceIndex;
    }

    if (code.capture_layout.empty() && code.instructions.size() == 5U &&
        instruction_selector_is(code.instructions[0], "/") &&
        instruction_selector_is(code.instructions[1], "*") &&
        instruction_selector_is(code.instructions[2], "-") &&
        code.instructions[3].opcode == Opcode::CloseUpvalues &&
        code.instructions[4].opcode == Opcode::Return &&
        quick_operand_reg_equals(code.instructions[0], 0, 4) &&
        quick_operand_reg_equals(code.instructions[0], 1, 0) &&
        quick_operand_reg_equals(code.instructions[0], 4, 1) &&
        quick_operand_reg_equals(code.instructions[1], 0, 3) &&
        quick_operand_reg_equals(code.instructions[1], 1, 4) &&
        quick_operand_reg_equals(code.instructions[1], 4, 1) &&
        quick_operand_reg_equals(code.instructions[2], 0, 2) &&
        quick_operand_reg_equals(code.instructions[2], 1, 0) &&
        quick_operand_reg_equals(code.instructions[2], 4, 3) &&
        quick_operand_reg_equals(code.instructions[4], 0, 2)) {
      return DirectClosureKind::LaneIndex;
    }

    if (code.capture_layout.empty() && code.instructions.size() == 8U &&
        code.instructions[0].opcode == Opcode::Safepoint &&
        instruction_selector_is(code.instructions[1], ">") &&
        code.instructions[2].opcode == Opcode::JumpIfFalse &&
        instruction_selector_is(code.instructions[3], "-") &&
        code.instructions[4].opcode == Opcode::Move &&
        code.instructions[5].opcode == Opcode::Jump &&
        code.instructions[6].opcode == Opcode::CloseUpvalues &&
        code.instructions[7].opcode == Opcode::Return &&
        quick_operand_reg_equals(code.instructions[1], 0, 2) &&
        quick_operand_reg_equals(code.instructions[1], 1, 0) &&
        quick_operand_reg_equals(code.instructions[1], 4, 1) &&
        quick_operand_reg_equals(code.instructions[2], 0, 2) &&
        quick_operand_reg_equals(code.instructions[3], 0, 3) &&
        quick_operand_reg_equals(code.instructions[3], 1, 0) &&
        quick_operand_reg_equals(code.instructions[3], 4, 1) &&
        quick_operand_reg_equals(code.instructions[4], 0, 0) &&
        quick_operand_reg_equals(code.instructions[4], 1, 3) &&
        quick_operand_reg_equals(code.instructions[7], 0, 0)) {
      std::uint32_t false_target = 0;
      std::uint32_t loop_target = 0;
      if (quick_operand_u32(code.instructions[2], 1, &false_target) &&
          quick_operand_u32(code.instructions[5], 0, &loop_target) &&
          false_target == 6U && loop_target == 0U) {
        return DirectClosureKind::WrapSubtract;
      }
    }

    if (code.capture_layout.size() == 1U && code.instructions.size() == 12U &&
        instruction_selector_is(code.instructions[0], "+") &&
        code.instructions[1].opcode == Opcode::LoadK &&
        instruction_selector_is(code.instructions[2], "+") &&
        code.instructions[3].opcode == Opcode::Move &&
        code.instructions[4].opcode == Opcode::LoadK &&
        instruction_selector_is(code.instructions[5], "*") &&
        code.instructions[6].opcode == Opcode::Move &&
        code.instructions[7].opcode == Opcode::LoadUpval &&
        code.instructions[8].opcode == Opcode::LoadK &&
        code.instructions[9].opcode == Opcode::Call &&
        code.instructions[10].opcode == Opcode::CloseUpvalues &&
        code.instructions[11].opcode == Opcode::Return) {
      return DirectClosureKind::MixLinearWrap;
    }

    if (code.capture_layout.size() == 2U && code.instructions.size() == 35U &&
        instruction_selector_is(code.instructions[1], "[]") &&
        instruction_selector_is(code.instructions[4], "[]") &&
        instruction_selector_is(code.instructions[7], "[]") &&
        code.instructions[9].opcode == Opcode::LoadUpval &&
        code.instructions[10].opcode == Opcode::LoadUpval &&
        code.instructions[12].opcode == Opcode::Call &&
        instruction_selector_is(code.instructions[13], "+") &&
        instruction_selector_is(code.instructions[14], "+") &&
        code.instructions[15].opcode == Opcode::Call &&
        instruction_selector_is(code.instructions[17], ">") &&
        instruction_selector_is(code.instructions[19], "-") &&
        instruction_selector_is(code.instructions[22], "-") &&
        code.instructions[24].opcode == Opcode::LoadUpval &&
        code.instructions[26].opcode == Opcode::Call &&
        instruction_selector_is(code.instructions[27], "*") &&
        instruction_selector_is(code.instructions[28], "+") &&
        code.instructions[29].opcode == Opcode::LoadUpval &&
        code.instructions[31].opcode == Opcode::Call &&
        instruction_selector_is(code.instructions[32], "+") &&
        code.instructions[34].opcode == Opcode::Return) {
      return DirectClosureKind::ScoreRow;
    }

    return DirectClosureKind::None;
  }

  DirectClosureKind direct_closure_kind_for(const BcCode &code) {
    const auto found = direct_closure_kinds_.find(code.code_id);
    if (found != direct_closure_kinds_.end()) {
      return found->second;
    }
    const DirectClosureKind kind = classify_direct_closure(code);
    direct_closure_kinds_[code.code_id] = kind;
    return kind;
  }

  static bool fast_call_arg_integer(const FastCallArg &arg, std::int64_t *out) {
    if (arg.int_valid) {
      *out = arg.int_value;
      return true;
    }
    if (arg.value.is_integer()) {
      *out = arg.value.as_integer();
      return true;
    }
    return false;
  }

  bool integer_constant_loaded_at(const BcCode &code, std::size_t pc,
                                  std::int64_t *out) const {
    if (pc >= code.instructions.size()) {
      return false;
    }
    const Instruction &insn = code.instructions[pc];
    if (insn.opcode != Opcode::LoadK) {
      return false;
    }
    std::uint32_t const_id = 0;
    if (!quick_operand_u32(insn, 1, &const_id) ||
        const_id >= module_.const_pool.size()) {
      return false;
    }
    const Constant &constant = module_.const_pool[const_id];
    if (constant.kind != ConstantKind::Integer) {
      return false;
    }
    *out = constant.int_value;
    return true;
  }

  bool captured_closure_has_kind(Frame &frame, const Value &capture,
                                 DirectClosureKind expected) {
    const Value value = unwrap_watch_value_for_read(capture);
    if (!value.is_closure()) {
      return false;
    }
    if (!ensure_lifecycle_access(frame, value)) {
      return false;
    }
    const std::shared_ptr<ClosureValue> closure = value.as_closure();
    if (closure == nullptr) {
      return false;
    }
    const BcCode *code = find_code(module_, closure->code_id);
    return code != nullptr && direct_closure_kind_for(*code) == expected;
  }

  bool integer_from_sequence_item(const std::vector<Value> &items,
                                  std::size_t index, std::int64_t *out) const {
    if (index >= items.size()) {
      return false;
    }
    if (!items[index].is_integer()) {
      return false;
    }
    *out = items[index].as_integer();
    return true;
  }

  FastCallStatus try_evaluate_direct_closure(
      Frame &frame, const BcCode &code, const FastCallArg *args,
      std::uint32_t arg_count, const std::vector<Value> &captures,
      Value *value_out, std::int64_t *int_out, bool *int_result) {
    *int_result = false;
    const DirectClosureKind kind = direct_closure_kind_for(code);
    if (kind == DirectClosureKind::None) {
      return FastCallStatus::NotHandled;
    }

    if (kind == DirectClosureKind::SequenceIndex) {
      if (arg_count != 2U || args[0].int_valid) {
        return FastCallStatus::NotHandled;
      }
      std::int64_t index = 0;
      if (!fast_call_arg_integer(args[1], &index)) {
        return FastCallStatus::NotHandled;
      }
      const std::vector<Value> *items =
          sequence_items_view(frame, args[0].value);
      if (fault_.has_value()) {
        return FastCallStatus::Faulted;
      }
      if (items == nullptr) {
        return FastCallStatus::NotHandled;
      }
      if (index < 0 || static_cast<std::size_t>(index) >= items->size()) {
        set_fault(frame, "IndexError", "collection index is out of bounds");
        return FastCallStatus::Faulted;
      }
      *value_out = (*items)[static_cast<std::size_t>(index)];
      return FastCallStatus::Matched;
    }

    std::int64_t first = 0;
    std::int64_t second = 0;
    if (kind != DirectClosureKind::ScoreRow &&
        (arg_count != 2U || !fast_call_arg_integer(args[0], &first) ||
         !fast_call_arg_integer(args[1], &second))) {
      return FastCallStatus::NotHandled;
    }

    if (kind == DirectClosureKind::LaneIndex) {
      if (second == 0) {
        set_fault(frame, "TypeError", "division by zero");
        return FastCallStatus::Faulted;
      }
      *int_out = first - (first / second) * second;
      *int_result = true;
      return FastCallStatus::Matched;
    }

    if (kind == DirectClosureKind::WrapSubtract) {
      if (second <= 0) {
        return FastCallStatus::NotHandled;
      }
      while (first > second) {
        first -= second;
      }
      *int_out = first;
      *int_result = true;
      return FastCallStatus::Matched;
    }

    if (kind == DirectClosureKind::MixLinearWrap) {
      if (captures.size() != 1U ||
          !captured_closure_has_kind(frame, captures[0],
                                     DirectClosureKind::WrapSubtract)) {
        if (fault_.has_value()) {
          return FastCallStatus::Faulted;
        }
        return FastCallStatus::NotHandled;
      }
      std::int64_t add_constant = 0;
      std::int64_t multiply_constant = 0;
      std::int64_t limit = 0;
      if (!integer_constant_loaded_at(code, 1, &add_constant) ||
          !integer_constant_loaded_at(code, 4, &multiply_constant) ||
          !integer_constant_loaded_at(code, 8, &limit) || limit <= 0) {
        return FastCallStatus::NotHandled;
      }
      std::int64_t mixed = first + second + add_constant;
      mixed *= multiply_constant;
      while (mixed > limit) {
        mixed -= limit;
      }
      *int_out = mixed;
      *int_result = true;
      return FastCallStatus::Matched;
    }

    if (kind == DirectClosureKind::ScoreRow) {
      if (arg_count != 3U || args[0].int_valid || args[1].int_valid ||
          captures.size() != 2U ||
          !captured_closure_has_kind(frame, captures[0],
                                     DirectClosureKind::MixLinearWrap) ||
          !captured_closure_has_kind(frame, captures[1],
                                     DirectClosureKind::SequenceIndex)) {
        if (fault_.has_value()) {
          return FastCallStatus::Faulted;
        }
        return FastCallStatus::NotHandled;
      }
      std::int64_t bias = 0;
      if (!fast_call_arg_integer(args[2], &bias)) {
        return FastCallStatus::NotHandled;
      }

      const std::vector<Value> *row_items =
          sequence_items_view(frame, args[0].value);
      if (fault_.has_value()) {
        return FastCallStatus::Faulted;
      }
      const std::vector<Value> *weight_items =
          sequence_items_view(frame, args[1].value);
      if (fault_.has_value()) {
        return FastCallStatus::Faulted;
      }
      if (row_items == nullptr || weight_items == nullptr) {
        return FastCallStatus::NotHandled;
      }

      std::int64_t x = 0;
      std::int64_t y = 0;
      std::int64_t z = 0;
      std::int64_t w0 = 0;
      std::int64_t w1 = 0;
      std::int64_t w2 = 0;
      if (!integer_from_sequence_item(*row_items, 0, &x) ||
          !integer_from_sequence_item(*row_items, 1, &y) ||
          !integer_from_sequence_item(*row_items, 2, &z) ||
          !integer_from_sequence_item(*weight_items, 0, &w0) ||
          !integer_from_sequence_item(*weight_items, 1, &w1) ||
          !integer_from_sequence_item(*weight_items, 2, &w2)) {
        return FastCallStatus::NotHandled;
      }

      const BcCode *mix_code = nullptr;
      const Value mix_capture = unwrap_watch_value_for_read(captures[0]);
      if (mix_capture.is_closure()) {
        const std::shared_ptr<ClosureValue> mix_closure =
            mix_capture.as_closure();
        if (mix_closure != nullptr) {
          mix_code = find_code(module_, mix_closure->code_id);
        }
      }
      std::int64_t add_constant = 17;
      std::int64_t multiply_constant = 13;
      std::int64_t limit = 2147483647;
      if (mix_code != nullptr) {
        (void)integer_constant_loaded_at(*mix_code, 1, &add_constant);
        (void)integer_constant_loaded_at(*mix_code, 4, &multiply_constant);
        (void)integer_constant_loaded_at(*mix_code, 8, &limit);
      }
      if (limit <= 0) {
        return FastCallStatus::NotHandled;
      }

      std::int64_t mixed = x + w0 + y + bias + add_constant;
      mixed *= multiply_constant;
      while (mixed > limit) {
        mixed -= limit;
      }
      mixed = mixed > z ? mixed - z : z - mixed;
      *int_out = mixed + w1 * y + w2;
      *int_result = true;
      return FastCallStatus::Matched;
    }

    return FastCallStatus::NotHandled;
  }

  void initialize_frame_register_file(Frame &frame, const BcCode &code) {
    frame.regs.assign(code.reg_count, Value::null());
    frame.initialized.assign(code.reg_count, 0U);
    frame.int64_regs.assign(code.reg_count, 0);
    frame.int_valid.assign(code.reg_count, 0U);
  }

  void ensure_integer_sidecar_size(Frame &frame) {
    if (frame.int64_regs.size() < frame.regs.size()) {
      frame.int64_regs.resize(frame.regs.size(), 0);
    }
    if (frame.int_valid.size() < frame.regs.size()) {
      frame.int_valid.resize(frame.regs.size(), 0U);
    }
  }

  void invalidate_integer_reg(Frame &frame, std::uint32_t reg) {
    if (reg < frame.int_valid.size()) {
      frame.int_valid[reg] = 0U;
    }
  }

  void sync_integer_reg_from_value(Frame &frame, std::uint32_t reg,
                                   const Value &value) {
    if (reg >= frame.regs.size()) {
      return;
    }
    ensure_integer_sidecar_size(frame);
    if (value.is_integer()) {
      frame.int64_regs[reg] = value.as_integer();
      frame.int_valid[reg] = 1U;
    } else {
      frame.int_valid[reg] = 0U;
    }
  }

  bool materialize_integer_reg_if_needed(Frame &frame, std::uint32_t reg) {
    if (reg >= frame.regs.size()) {
      set_fault(frame, "VMError", "register out of range");
      return false;
    }
    if (reg >= frame.int_valid.size() || frame.int_valid[reg] == 0U) {
      return true;
    }
    if (frame.initialized.size() < frame.regs.size()) {
      frame.initialized.resize(frame.regs.size(), 0U);
    }
    if (frame.regs[reg].is_watch_cell()) {
      invalidate_integer_reg(frame, reg);
      return true;
    }
    frame.regs[reg] = Value::integer(frame.int64_regs[reg]);
    frame.initialized[reg] = 1U;
    return true;
  }

  void materialize_integer_regs(Frame &frame) {
    const std::size_t count =
        std::min(frame.regs.size(), frame.int_valid.size());
    for (std::size_t i = 0; i < count; ++i) {
      if (frame.int_valid[i] == 0U) {
        continue;
      }
      if (frame.regs[i].is_watch_cell()) {
        frame.int_valid[i] = 0U;
        continue;
      }
      frame.regs[i] = Value::integer(frame.int64_regs[i]);
      if (frame.initialized.size() < frame.regs.size()) {
        frame.initialized.resize(frame.regs.size(), 0U);
      }
      frame.initialized[i] = 1U;
    }
  }

  void push_frame_from_args(const BcCode &code, const Value *args,
                            std::size_t arg_count,
                            const std::vector<Value> &captures, Value self,
                            Value block,
                            std::optional<std::uint32_t> caller_result_reg) {
    Frame frame = acquire_frame(code);
    for (std::size_t i = 0; i < arg_count && i < frame.regs.size(); ++i) {
      frame.regs[i] = args[i];
      frame.initialized[i] = 1U;
      sync_integer_reg_from_value(frame, static_cast<std::uint32_t>(i),
                                  frame.regs[i]);
    }
    frame.captures = captures;
    frame.self = std::move(self);
    frame.block = std::move(block);
    frame.caller_result_reg = caller_result_reg;
    frames_.push_back(std::move(frame));
  }

  void push_frame_from_fast_args(
      const BcCode &code, const FastCallArg *args, std::size_t arg_count,
      const std::vector<Value> &captures, Value self, Value block,
      std::optional<std::uint32_t> caller_result_reg) {
    Frame frame = acquire_frame(code);
    for (std::size_t i = 0; i < arg_count && i < frame.regs.size(); ++i) {
      frame.initialized[i] = 1U;
      if (args[i].int_valid) {
        frame.regs[i] = Value::null();
        ensure_integer_sidecar_size(frame);
        frame.int64_regs[i] = args[i].int_value;
        frame.int_valid[i] = 1U;
      } else {
        frame.regs[i] = args[i].value;
        sync_integer_reg_from_value(frame, static_cast<std::uint32_t>(i),
                                    frame.regs[i]);
      }
    }
    frame.captures = captures;
    frame.self = std::move(self);
    frame.block = std::move(block);
    frame.caller_result_reg = caller_result_reg;
    frames_.push_back(std::move(frame));
  }

  void push_frame(const BcCode &code, const std::vector<Value> &args,
                  std::vector<Value> captures, Value self, Value block,
                  std::optional<std::uint32_t> caller_result_reg) {
    push_frame_from_args(code, args.data(), args.size(), captures,
                         std::move(self), std::move(block), caller_result_reg);
  }

  Frame acquire_frame(const BcCode &code) {
    Frame frame;
    auto found = frame_pool_.find(code.code_id);
    if (found != frame_pool_.end() && !found->second.empty()) {
      frame = std::move(found->second.back());
      found->second.pop_back();
    }

    frame.code = &code;
    frame.quick_code = &quick_code_for(code);
    frame.pc = 0;
    if (frame.regs.size() != code.reg_count) {
      frame.regs.assign(code.reg_count, Value::null());
    }
    frame.initialized.assign(code.reg_count, 0U);
    if (frame.int64_regs.size() != code.reg_count) {
      frame.int64_regs.assign(code.reg_count, 0);
    }
    frame.int_valid.assign(code.reg_count, 0U);
    frame.captures.clear();
    frame.self = Value::null();
    frame.block = Value::null();
    frame.last_result = Value::null();
    frame.caller_result_reg.reset();
    frame.active_call_pc.reset();
    frame.return_override.reset();
    frame.prepared_seq_regs.clear();
    frame.prepared_map_regs.clear();
    frame.pending_pattern_bindings.clear();
    return frame;
  }

  void recycle_frame(Frame frame) {
    if (frame.code == nullptr) {
      return;
    }
    constexpr std::size_t kMaxPooledFramesPerCode = 16;
    const std::uint32_t code_id = frame.code->code_id;
    frame.code = nullptr;
    frame.quick_code = nullptr;
    frame.pc = 0;
    frame.captures.clear();
    frame.self = Value::null();
    frame.block = Value::null();
    frame.last_result = Value::null();
    frame.caller_result_reg.reset();
    frame.active_call_pc.reset();
    frame.return_override.reset();
    frame.prepared_seq_regs.clear();
    frame.prepared_map_regs.clear();
    frame.pending_pattern_bindings.clear();

    std::vector<Frame> &bucket = frame_pool_[code_id];
    if (bucket.size() < kMaxPooledFramesPerCode) {
      bucket.push_back(std::move(frame));
    }
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

  FastCallStatus step_fast_closure_call(Frame &frame, const Instruction &insn) {
    constexpr std::uint32_t kMaxFastCallArgs = 8;

    std::uint32_t dst = 0;
    std::uint32_t callee_reg = 0;
    std::uint32_t pos_count = 0;
    if (!operand_u32(frame, insn, 0, &dst) ||
        !operand_u32(frame, insn, 1, &callee_reg) ||
        !operand_u32(frame, insn, 2, &pos_count)) {
      return FastCallStatus::Faulted;
    }
    if (pos_count > kMaxFastCallArgs) {
      return FastCallStatus::NotHandled;
    }

    std::uint32_t arg_regs[kMaxFastCallArgs] = {};
    std::size_t operand_index = 3;
    for (std::uint32_t i = 0; i < pos_count; ++i) {
      if (!operand_u32(frame, insn, operand_index++, &arg_regs[i])) {
        return FastCallStatus::Faulted;
      }
    }

    std::uint32_t kw_count = 0;
    if (!operand_u32(frame, insn, operand_index++, &kw_count)) {
      return FastCallStatus::Faulted;
    }
    if (kw_count != 0U) {
      return FastCallStatus::NotHandled;
    }

    std::int64_t block_reg = -1;
    if (!operand_i64(frame, insn, operand_index++, &block_reg)) {
      return FastCallStatus::Faulted;
    }
    if (has_optional_reg(block_reg)) {
      return FastCallStatus::NotHandled;
    }

    const std::optional<std::uint32_t> site_id =
        optional_operand_u32(frame, insn, operand_index++);
    if (fault_.has_value()) {
      return FastCallStatus::Faulted;
    }
    (void)site_id;

    if (callee_reg >= frame.regs.size() ||
        callee_reg >= frame.initialized.size() ||
        frame.initialized[callee_reg] == 0U ||
        (callee_reg < frame.int_valid.size() &&
         frame.int_valid[callee_reg] != 0U) ||
        !frame.regs[callee_reg].is_closure()) {
      return FastCallStatus::NotHandled;
    }

    FastCallArg args[kMaxFastCallArgs];
    for (std::uint32_t i = 0; i < pos_count; ++i) {
      std::int64_t int_arg = 0;
      const bool int_fast =
          read_integer_reg_unboxed(frame, arg_regs[i], &int_arg);
      if (fault_.has_value()) {
        return FastCallStatus::Faulted;
      }
      if (int_fast) {
        args[i].int_value = int_arg;
        args[i].int_valid = true;
        continue;
      }
      args[i].value = read_reg(frame, arg_regs[i]);
      if (fault_.has_value()) {
        return FastCallStatus::Faulted;
      }
    }

    const Value callee = read_reg(frame, callee_reg);
    if (fault_.has_value()) {
      return FastCallStatus::Faulted;
    }
    if (!callee.is_closure()) {
      return FastCallStatus::NotHandled;
    }
    if (!ensure_lifecycle_access(frame, callee)) {
      return FastCallStatus::Faulted;
    }
    const std::shared_ptr<ClosureValue> closure = callee.as_closure();
    if (closure == nullptr) {
      set_fault(frame, "TypeError", "closure value is null");
      return FastCallStatus::Faulted;
    }
    const BcCode *code = find_code(module_, closure->code_id);
    if (code == nullptr) {
      set_fault(frame, "VMError", "closure code id is unknown");
      return FastCallStatus::Faulted;
    }

    Value direct_value = Value::null();
    std::int64_t direct_int = 0;
    bool direct_int_result = false;
    const FastCallStatus direct_status = try_evaluate_direct_closure(
        frame, *code, args, pos_count, closure->captures, &direct_value,
        &direct_int, &direct_int_result);
    if (direct_status == FastCallStatus::Faulted) {
      return FastCallStatus::Faulted;
    }
    if (direct_status == FastCallStatus::Matched) {
      const bool wrote =
          direct_int_result
              ? write_integer_reg_unboxed(frame, dst, direct_int)
              : write_reg_fast_plain(frame, dst, std::move(direct_value));
      if (!wrote) {
        return FastCallStatus::Faulted;
      }
      ++frame.pc;
      return FastCallStatus::Matched;
    }

    const std::uint32_t call_pc = static_cast<std::uint32_t>(frame.pc);
    ++frame.pc;
    frame.active_call_pc = call_pc;
    push_frame_from_fast_args(*code, args, pos_count, closure->captures,
                              closure->self, Value::null(), dst);
    return FastCallStatus::Matched;
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

  bool load_integer_constant(const Frame &frame, std::uint32_t const_id,
                             std::int64_t *out) {
    if (const_id >= module_.const_pool.size()) {
      set_fault(frame, "VMError", "integer constant ref out of range");
      return false;
    }
    const Constant &constant = module_.const_pool[const_id];
    if (constant.kind != ConstantKind::Integer) {
      set_fault(frame, "VMError", "integer opcode expects integer constant");
      return false;
    }
    *out = constant.int_value;
    return true;
  }

  bool read_integer_reg_unboxed(Frame &frame, std::uint32_t reg,
                                std::int64_t *out) {
    if (reg >= frame.regs.size()) {
      set_fault(frame, "VMError", "register out of range");
      return false;
    }
    if (reg >= frame.initialized.size() || frame.initialized[reg] == 0U) {
      set_fault(frame, "NameError", "read of uninitialized local/module cell");
      return false;
    }
    if (reg < frame.int_valid.size() && frame.int_valid[reg] != 0U) {
      *out = frame.int64_regs[reg];
      return true;
    }
    const std::int64_t *value =
        std::get_if<std::int64_t>(&frame.regs[reg].payload);
    if (value == nullptr) {
      return false;
    }
    *out = *value;
    ensure_integer_sidecar_size(frame);
    frame.int64_regs[reg] = *value;
    frame.int_valid[reg] = 1U;
    return true;
  }

  bool write_integer_reg_unboxed(Frame &frame, std::uint32_t reg,
                                 std::int64_t value) {
    if (reg >= frame.regs.size()) {
      set_fault(frame, "VMError", "register out of range");
      return false;
    }
    if (frame.initialized.size() < frame.regs.size()) {
      frame.initialized.resize(frame.regs.size(), 0U);
    }
    if ((frame.initialized[reg] != 0U && frame.regs[reg].is_watch_cell()) ||
        !frame.prepared_seq_regs.empty() || !frame.prepared_map_regs.empty() ||
        !frame.pending_pattern_bindings.empty()) {
      return write_reg(frame, reg, Value::integer(value));
    }
    ensure_integer_sidecar_size(frame);
    frame.int64_regs[reg] = value;
    frame.int_valid[reg] = 1U;
    frame.initialized[reg] = 1U;
    return true;
  }

  bool write_reg_fast_plain(Frame &frame, std::uint32_t reg, Value value) {
    if (reg >= frame.regs.size()) {
      set_fault(frame, "VMError", "register out of range");
      return false;
    }
    if (frame.initialized.size() < frame.regs.size()) {
      frame.initialized.resize(frame.regs.size(), 0U);
    }
    if ((frame.initialized[reg] != 0U && frame.regs[reg].is_watch_cell()) ||
        !frame.prepared_seq_regs.empty() || !frame.prepared_map_regs.empty() ||
        !frame.pending_pattern_bindings.empty()) {
      return write_reg(frame, reg, std::move(value));
    }
    frame.regs[reg] = std::move(value);
    frame.initialized[reg] = 1U;
    sync_integer_reg_from_value(frame, reg, frame.regs[reg]);
    return true;
  }

  bool step_integer_binary_decoded(Frame &frame, Opcode opcode,
                                   std::uint32_t dst, std::uint32_t lhs_reg,
                                   std::uint32_t rhs_operand,
                                   bool rhs_is_constant) {
    std::int64_t fast_lhs = 0;
    std::int64_t fast_rhs = 0;
    const bool lhs_fast = read_integer_reg_unboxed(frame, lhs_reg, &fast_lhs);
    if (fault_.has_value()) {
      return false;
    }
    bool rhs_fast = false;
    if (rhs_is_constant) {
      rhs_fast = load_integer_constant(frame, rhs_operand, &fast_rhs);
      if (!rhs_fast) {
        return false;
      }
    } else {
      rhs_fast = read_integer_reg_unboxed(frame, rhs_operand, &fast_rhs);
      if (fault_.has_value()) {
        return false;
      }
    }
    if (lhs_fast && rhs_fast) {
      switch (opcode) {
      case Opcode::IAdd:
      case Opcode::IAddK:
        if (!write_integer_reg_unboxed(frame, dst, fast_lhs + fast_rhs)) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::ISub:
      case Opcode::ISubK:
        if (!write_integer_reg_unboxed(frame, dst, fast_lhs - fast_rhs)) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::IMul:
      case Opcode::IMulK:
        if (!write_integer_reg_unboxed(frame, dst, fast_lhs * fast_rhs)) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::IDiv:
      case Opcode::IDivK:
        if (fast_rhs == 0) {
          set_fault(frame, "TypeError", "division by zero");
          return false;
        }
        if (!write_integer_reg_unboxed(frame, dst, fast_lhs / fast_rhs)) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::IMod:
      case Opcode::IModK:
        if (fast_rhs == 0) {
          set_fault(frame, "TypeError", "modulo by zero");
          return false;
        }
        if (!write_integer_reg_unboxed(frame, dst,
                                       floor_mod_int64(fast_lhs, fast_rhs))) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::IFloorDiv:
      case Opcode::IFloorDivK:
        if (fast_rhs == 0) {
          set_fault(frame, "TypeError", "division by zero");
          return false;
        }
        if (!write_integer_reg_unboxed(frame, dst,
                                       floor_div_int64(fast_lhs, fast_rhs))) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::ILt:
      case Opcode::ILtK:
        if (!write_reg_fast_plain(frame, dst,
                                  Value::boolean(fast_lhs < fast_rhs))) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::IGt:
      case Opcode::IGtK:
        if (!write_reg_fast_plain(frame, dst,
                                  Value::boolean(fast_lhs > fast_rhs))) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::ILe:
      case Opcode::ILeK:
        if (!write_reg_fast_plain(frame, dst,
                                  Value::boolean(fast_lhs <= fast_rhs))) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::IGe:
      case Opcode::IGeK:
        if (!write_reg_fast_plain(frame, dst,
                                  Value::boolean(fast_lhs >= fast_rhs))) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::IEq:
      case Opcode::IEqK:
        if (!write_reg_fast_plain(frame, dst,
                                  Value::boolean(fast_lhs == fast_rhs))) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::INe:
      case Opcode::INeK:
        if (!write_reg_fast_plain(frame, dst,
                                  Value::boolean(fast_lhs != fast_rhs))) {
          return false;
        }
        ++frame.pc;
        return true;
      case Opcode::ICmp:
      case Opcode::ICmpK:
        if (!write_integer_reg_unboxed(frame, dst,
                                       compare_int64(fast_lhs, fast_rhs))) {
          return false;
        }
        ++frame.pc;
        return true;
      default:
        set_fault(frame, "VMError", "unsupported integer opcode");
        return false;
      }
    }

    const Value lhs_value = read_reg(frame, lhs_reg);
    if (fault_.has_value()) {
      return false;
    }
    Value rhs_value = Value::null();
    if (rhs_is_constant) {
      std::int64_t rhs = 0;
      if (!load_integer_constant(frame, rhs_operand, &rhs)) {
        return false;
      }
      rhs_value = Value::integer(rhs);
    } else {
      rhs_value = read_reg(frame, rhs_operand);
      if (fault_.has_value()) {
        return false;
      }
    }

    auto selector = [&]() -> std::string {
      switch (opcode) {
      case Opcode::IAdd:
      case Opcode::IAddK:
        return "+";
      case Opcode::ISub:
      case Opcode::ISubK:
        return "-";
      case Opcode::IMul:
      case Opcode::IMulK:
        return "*";
      case Opcode::IDiv:
      case Opcode::IDivK:
        return "/";
      case Opcode::IMod:
      case Opcode::IModK:
        return "%";
      case Opcode::IFloorDiv:
      case Opcode::IFloorDivK:
        return "//";
      case Opcode::ILt:
      case Opcode::ILtK:
        return "<";
      case Opcode::IGt:
      case Opcode::IGtK:
        return ">";
      case Opcode::ILe:
      case Opcode::ILeK:
        return "<=";
      case Opcode::IGe:
      case Opcode::IGeK:
        return ">=";
      case Opcode::IEq:
      case Opcode::IEqK:
        return "==";
      case Opcode::INe:
      case Opcode::INeK:
        return "!=";
      case Opcode::ICmp:
      case Opcode::ICmpK:
        return "<=>";
      default:
        return "";
      }
    };

    if (!lhs_value.is_integer() || !rhs_value.is_integer()) {
      const std::string selector_text = selector();
      if (!rhs_is_constant) {
        Instruction fallback;
        fallback.opcode = Opcode::Send;
        fallback.operands = {
            {dst, false},
            {lhs_reg, false},
            {intern_runtime_symbol(selector_text), false},
            {1, false},
            {rhs_operand, false},
            {0, false},
            {-1, true},
        };
        return step_send(frame, fallback, false);
      }

      Value result = Value::null();
      const SendStatus status =
          try_apply_scalar_send(frame, lhs_value, selector_text, {rhs_value},
                                Value::null(), {}, &result);
      if (status == SendStatus::Faulted) {
        return false;
      }
      if (status == SendStatus::Matched) {
        if (!write_reg(frame, dst, std::move(result))) {
          return false;
        }
        ++frame.pc;
        return true;
      }
      set_fault(frame, "NoMethodError",
                "selector is not implemented in current runtime baseline");
      return false;
    }

    const std::int64_t lhs = lhs_value.as_integer();
    const std::int64_t rhs = rhs_value.as_integer();
    switch (opcode) {
    case Opcode::IAdd:
    case Opcode::IAddK:
      if (!write_integer_reg_unboxed(frame, dst, lhs + rhs)) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::ISub:
    case Opcode::ISubK:
      if (!write_integer_reg_unboxed(frame, dst, lhs - rhs)) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::IMul:
    case Opcode::IMulK:
      if (!write_integer_reg_unboxed(frame, dst, lhs * rhs)) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::IDiv:
    case Opcode::IDivK:
      if (rhs == 0) {
        set_fault(frame, "TypeError", "division by zero");
        return false;
      }
      if (!write_integer_reg_unboxed(frame, dst, lhs / rhs)) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::IMod:
    case Opcode::IModK:
      if (rhs == 0) {
        set_fault(frame, "TypeError", "modulo by zero");
        return false;
      }
      if (!write_integer_reg_unboxed(frame, dst, floor_mod_int64(lhs, rhs))) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::IFloorDiv:
    case Opcode::IFloorDivK:
      if (rhs == 0) {
        set_fault(frame, "TypeError", "division by zero");
        return false;
      }
      if (!write_integer_reg_unboxed(frame, dst, floor_div_int64(lhs, rhs))) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::ILt:
    case Opcode::ILtK:
      if (!write_reg_fast_plain(frame, dst, Value::boolean(lhs < rhs))) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::IGt:
    case Opcode::IGtK:
      if (!write_reg_fast_plain(frame, dst, Value::boolean(lhs > rhs))) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::ILe:
    case Opcode::ILeK:
      if (!write_reg_fast_plain(frame, dst, Value::boolean(lhs <= rhs))) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::IGe:
    case Opcode::IGeK:
      if (!write_reg_fast_plain(frame, dst, Value::boolean(lhs >= rhs))) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::IEq:
    case Opcode::IEqK:
      if (!write_reg_fast_plain(frame, dst, Value::boolean(lhs == rhs))) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::INe:
    case Opcode::INeK:
      if (!write_reg_fast_plain(frame, dst, Value::boolean(lhs != rhs))) {
        return false;
      }
      ++frame.pc;
      return true;
    case Opcode::ICmp:
    case Opcode::ICmpK:
      if (!write_integer_reg_unboxed(frame, dst, compare_int64(lhs, rhs))) {
        return false;
      }
      ++frame.pc;
      return true;
    default:
      set_fault(frame, "VMError", "unsupported integer opcode");
      return false;
    }
  }

  bool step_integer_binary(Frame &frame, const Instruction &insn,
                           bool rhs_is_constant) {
    std::uint32_t dst = 0;
    std::uint32_t lhs_reg = 0;
    std::uint32_t rhs_operand = 0;
    if (!operand_u32(frame, insn, 0, &dst) ||
        !operand_u32(frame, insn, 1, &lhs_reg) ||
        !operand_u32(frame, insn, 2, &rhs_operand)) {
      return false;
    }
    return step_integer_binary_decoded(frame, insn.opcode, dst, lhs_reg,
                                       rhs_operand, rhs_is_constant);
  }

  bool step_integer_binary(Frame &frame, const QuickInsn &insn,
                           bool rhs_is_constant) {
    return step_integer_binary_decoded(frame, insn.opcode, insn.a, insn.b,
                                       insn.c, rhs_is_constant);
  }

  bool step_compare_jump_if_false(Frame &frame, const QuickInsn &insn,
                                  bool rhs_is_constant) {
    std::int64_t fast_lhs = 0;
    std::int64_t fast_rhs = 0;
    const bool lhs_fast = read_integer_reg_unboxed(frame, insn.a, &fast_lhs);
    if (fault_.has_value()) {
      return false;
    }
    bool rhs_fast = false;
    if (rhs_is_constant) {
      rhs_fast = load_integer_constant(frame, insn.b, &fast_rhs);
      if (!rhs_fast) {
        return false;
      }
    } else {
      rhs_fast = read_integer_reg_unboxed(frame, insn.b, &fast_rhs);
      if (fault_.has_value()) {
        return false;
      }
    }

    if (!lhs_fast || !rhs_fast) {
      const Instruction &source = frame.code->instructions[frame.pc];
      return step_integer_binary(frame, source, rhs_is_constant);
    }
    if (insn.c >= frame.code->instructions.size()) {
      set_fault(frame, "VMError", "jump target out of range");
      return false;
    }

    const bool compare =
        insn.quick_opcode == QuickOpcode::ILtJumpIfFalse ||
                insn.quick_opcode == QuickOpcode::ILtKJumpIfFalse
            ? fast_lhs < fast_rhs
            : fast_lhs > fast_rhs;
    frame.pc = !compare ? insn.c : frame.pc + 2U;
    return true;
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

  std::optional<Value>
  lookup_native_prelude_constant(const std::vector<std::string> &segments) {
    const std::string path = join_path_segments(segments);
    if (path == "print") {
      return Value::native_function(RuntimeNativeFunctionKind::Print);
    }
    if (path == "p") {
      return Value::native_function(RuntimeNativeFunctionKind::P);
    }
    if (path == "pp") {
      return Value::native_function(RuntimeNativeFunctionKind::Pp);
    }
    if (path == "Kernel") {
      return Value::native_type(RuntimeNativeTypeKind::Kernel);
    }
    if (path == "io") {
      return Value::native_type(RuntimeNativeTypeKind::Io);
    }
    if (path == "io.Buffer") {
      return Value::native_type(RuntimeNativeTypeKind::TextBuffer);
    }
    if (path == "io.Logger") {
      return Value::native_type(RuntimeNativeTypeKind::Logger);
    }
    if (path == "Amber") {
      return Value::native_type(RuntimeNativeTypeKind::Amber);
    }
    if (path == "Str") {
      return Value::native_type(RuntimeNativeTypeKind::Str);
    }
    if (path == "Int") {
      return Value::native_type(RuntimeNativeTypeKind::Int);
    }
    if (path == "Float") {
      return Value::native_type(RuntimeNativeTypeKind::Float);
    }
    if (path == "Bool") {
      return Value::native_type(RuntimeNativeTypeKind::Bool);
    }
    if (path == "Symbol") {
      return Value::native_type(RuntimeNativeTypeKind::Symbol);
    }
    if (path == "Array") {
      return Value::native_type(RuntimeNativeTypeKind::Array);
    }
    if (path == "Tuple") {
      return Value::native_type(RuntimeNativeTypeKind::Tuple);
    }
    if (path == "Set") {
      return Value::native_type(RuntimeNativeTypeKind::Set);
    }
    if (path == "Map") {
      return Value::native_type(RuntimeNativeTypeKind::Map);
    }
    if (path == "Null") {
      return Value::native_type(RuntimeNativeTypeKind::Null);
    }
    if (path == "Object") {
      return Value::native_type(RuntimeNativeTypeKind::Object);
    }
    if (path == "task") {
      return Value::task_module(std::make_shared<RuntimeTaskModule>());
    }
    if (path == "task.flow") {
      return Value::flow_module(std::make_shared<RuntimeFlowModule>());
    }
    if (path == "Flow" || path == "task.flow.Flow") {
      return Value::native_type(RuntimeNativeTypeKind::Flow);
    }
    if (path == "Channel" || path == "sync.Channel") {
      return Value::native_type(RuntimeNativeTypeKind::Channel);
    }
    if (path == "Mutex" || path == "sync.Mutex") {
      return Value::native_type(RuntimeNativeTypeKind::Mutex);
    }
    if (path == "Atomic" || path == "sync.Atomic") {
      return Value::native_type(RuntimeNativeTypeKind::Atomic);
    }
    if (path == "Barrier" || path == "sync.Barrier") {
      return Value::native_type(RuntimeNativeTypeKind::Barrier);
    }
    if (path == "ThreadedCollection" ||
        path == "task.flow.ThreadedCollection") {
      return Value::native_type(RuntimeNativeTypeKind::ThreadedCollection);
    }
    return std::nullopt;
  }

  std::optional<std::uint32_t> lookup_class_by_path_segments_no_fault(
      const std::vector<std::string> &segments, bool *ambiguous) {
    if (ambiguous != nullptr) {
      *ambiguous = false;
    }
    if (segments.empty()) {
      return std::nullopt;
    }

    const std::string full_path = join_path_segments(segments);
    for (std::uint32_t index = 0; index < module_.classes.size(); ++index) {
      const std::uint32_t symbol_id = module_.classes[index].class_name_sym_id;
      if (symbol_id < module_.symbols.size() &&
          module_.symbols[symbol_id] == full_path) {
        return index;
      }
    }

    const std::string &leaf = segments.back();
    std::optional<std::uint32_t> match;
    for (std::uint32_t index = 0; index < module_.classes.size(); ++index) {
      const std::uint32_t symbol_id = module_.classes[index].class_name_sym_id;
      if (symbol_id >= module_.symbols.size() ||
          module_.symbols[symbol_id] != leaf) {
        continue;
      }
      if (match.has_value()) {
        if (ambiguous != nullptr) {
          *ambiguous = true;
        }
        return std::nullopt;
      }
      match = index;
    }
    return match;
  }

  bool find_class_by_path_segments(const Frame &frame,
                                   const std::vector<std::string> &segments,
                                   std::uint32_t *out_class_index) {
    if (segments.empty()) {
      set_fault(frame, "VMError", "class path is empty");
      return false;
    }

    bool ambiguous = false;
    const std::optional<std::uint32_t> match =
        lookup_class_by_path_segments_no_fault(segments, &ambiguous);
    if (match.has_value()) {
      *out_class_index = *match;
      return true;
    }
    if (ambiguous) {
      set_fault(frame, "VMError", "class path ref is ambiguous");
      return false;
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
    bool ambiguous = false;
    const std::optional<std::uint32_t> class_match =
        lookup_class_by_path_segments_no_fault(segments, &ambiguous);
    if (class_match.has_value()) {
      return Value::class_object(*class_match);
    }
    if (ambiguous) {
      set_fault(frame, "VMError", "class path ref is ambiguous");
      return Value::null();
    }
    if (std::optional<Value> native_value =
            lookup_native_prelude_constant(segments)) {
      return *native_value;
    }
    if (find_class_by_path_segments(frame, segments, &class_index)) {
      return Value::class_object(class_index);
    }
    return Value::null();
  }

  Value read_reg(Frame &frame, std::uint32_t reg) {
    if (reg >= frame.regs.size()) {
      set_fault(frame, "VMError", "register out of range");
      return Value::null();
    }
    if (reg >= frame.initialized.size() || frame.initialized[reg] == 0U) {
      set_fault(frame, "NameError", "read of uninitialized local/module cell");
      return Value::null();
    }
    if (!materialize_integer_reg_if_needed(frame, reg)) {
      return Value::null();
    }
    return unwrap_watch_value_for_read(frame.regs[reg]);
  }

  void record_watch_cell_dependency(const RuntimeWatchCellSnapshot &snapshot) {
    if (!snapshot.watched || snapshot.cell_id == 0) {
      return;
    }
    RuntimeDependency dependency;
    dependency.kind = RuntimeDependencyKind::Binding;
    dependency.cell_id = snapshot.cell_id;
    dependency.target_name = snapshot.target_name;
    dependency.revision = snapshot.revision;
    state_->record_dependency(std::move(dependency));
  }

  Value read_watch_cell_value(const std::shared_ptr<RuntimeWatchCell> &cell) {
    if (cell == nullptr) {
      return Value::null();
    }
    const RuntimeWatchCellSnapshot snapshot = cell->snapshot();
    record_watch_cell_dependency(snapshot);
    return snapshot.value;
  }

  Value unwrap_watch_value_for_read(const Value &value) {
    if (!value.is_watch_cell()) {
      return value;
    }
    return read_watch_cell_value(value.as_watch_cell());
  }

  void record_watch_ivar_dependency(const RuntimeWatchIvarSnapshot &snapshot) {
    if (!snapshot.watched || snapshot.object_id == 0 ||
        snapshot.field_name.empty()) {
      return;
    }
    RuntimeDependency dependency;
    dependency.kind = RuntimeDependencyKind::Ivar;
    dependency.object_id = snapshot.object_id;
    dependency.target_name = "@" + snapshot.field_name;
    dependency.field_name = snapshot.field_name;
    dependency.revision = snapshot.field_revision;
    dependency.object_revision = snapshot.object_revision;
    state_->record_dependency(std::move(dependency));
  }

  Value record_watch_ivar_read(const std::shared_ptr<InstanceValue> &instance,
                               const std::string &field_name,
                               Value current_value) {
    if (instance == nullptr || instance->watch_state == nullptr ||
        !instance->watch_state->field_watched(field_name)) {
      return current_value;
    }
    const RuntimeWatchIvarSnapshot snapshot =
        instance->watch_state->snapshot_field(field_name, current_value);
    record_watch_ivar_dependency(snapshot);
    return current_value;
  }

  RuntimeWatchEvent make_watch_event(const std::string &kind,
                                     const RuntimeWatchCellSnapshot &snapshot,
                                     const RuntimeWatchWriteResult *write,
                                     std::uint64_t handle_id = 0) {
    RuntimeWatchEvent event;
    event.kind = kind;
    event.cell_id = snapshot.cell_id;
    event.handle_id = handle_id;
    event.target_name = snapshot.target_name;
    event.old_revision = snapshot.revision;
    event.new_revision = snapshot.revision;
    event.old_value = snapshot.value;
    event.new_value = snapshot.value;
    if (write != nullptr) {
      event.old_revision = write->old_revision;
      event.new_revision = write->new_revision;
      event.old_value = write->old_value;
      event.new_value = write->new_value;
    }
    return event;
  }

  RuntimeWatchEvent make_watch_ivar_event(
      const std::string &kind, const RuntimeWatchIvarSnapshot &snapshot,
      const RuntimeWatchIvarWriteResult *write, std::uint64_t handle_id = 0) {
    RuntimeWatchEvent event;
    event.kind = kind;
    event.handle_id = handle_id;
    event.object_id = snapshot.object_id;
    event.old_object_revision = snapshot.object_revision;
    event.new_object_revision = snapshot.object_revision;
    event.target_name = "@" + snapshot.field_name;
    event.field_name = snapshot.field_name;
    event.old_revision = snapshot.field_revision;
    event.new_revision = snapshot.field_revision;
    event.old_value = snapshot.value;
    event.new_value = snapshot.value;
    if (write != nullptr) {
      event.old_object_revision = write->old_object_revision;
      event.new_object_revision = write->new_object_revision;
      event.old_revision = write->old_revision;
      event.new_revision = write->new_revision;
      event.old_value = write->old_value;
      event.new_value = write->new_value;
    }
    return event;
  }

  void record_watch_write(const std::shared_ptr<RuntimeWatchCell> &cell,
                          const RuntimeWatchWriteResult &write) {
    if (cell == nullptr || !write.changed) {
      return;
    }
    state_->record_watch_event(
        make_watch_event("watch.write", cell->snapshot(), &write));
  }

  std::uint64_t
  object_watch_id(const std::shared_ptr<InstanceValue> &instance) const {
    if (instance == nullptr) {
      return 0;
    }
    if (instance->header.allocation_id != 0) {
      return instance->header.allocation_id;
    }
    return static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(instance.get()));
  }

  std::shared_ptr<RuntimeWatchObjectState>
  ensure_watch_object_state(const std::shared_ptr<InstanceValue> &instance) {
    if (instance == nullptr) {
      return nullptr;
    }
    if (instance->watch_state == nullptr) {
      instance->watch_state =
          std::make_shared<RuntimeWatchObjectState>(object_watch_id(instance));
    }
    return instance->watch_state;
  }

  Value
  instance_ivar_value_or_null(const std::shared_ptr<InstanceValue> &instance,
                              const std::string &name) const {
    if (instance == nullptr) {
      return Value::null();
    }
    const std::shared_ptr<const ShapeDescriptor> shape = instance->header.shape;
    if (shape != nullptr && !shape->dead) {
      const auto slot = shape->ivar_slots.find(name);
      if (slot != shape->ivar_slots.end() &&
          slot->second < instance->ivar_storage.size()) {
        return instance->ivar_storage[slot->second];
      }
    }
    const auto legacy = instance->ivars.find(name);
    return legacy == instance->ivars.end() ? Value::null() : legacy->second;
  }

  void record_watch_ivar_write(const std::shared_ptr<InstanceValue> &instance,
                               const std::string &field_name, Value old_value,
                               Value new_value) {
    if (instance == nullptr || instance->watch_state == nullptr ||
        !instance->watch_state->field_watched(field_name)) {
      return;
    }
    const RuntimeWatchIvarWriteResult write =
        instance->watch_state->write_field(field_name, std::move(old_value),
                                           std::move(new_value));
    if (!write.changed) {
      return;
    }
    const RuntimeWatchIvarSnapshot snapshot =
        instance->watch_state->snapshot_field(field_name, write.new_value);
    state_->record_watch_event(
        make_watch_ivar_event("watch.ivar.write", snapshot, &write));
  }

  bool write_reg(Frame &frame, std::uint32_t reg, Value value) {
    if (reg >= frame.regs.size()) {
      set_fault(frame, "VMError", "register out of range");
      return false;
    }
    if (frame.initialized.size() < frame.regs.size()) {
      frame.initialized.resize(frame.regs.size(), 0U);
    }
    if (frame.initialized[reg] != 0U && frame.regs[reg].is_watch_cell()) {
      invalidate_integer_reg(frame, reg);
      const std::shared_ptr<RuntimeWatchCell> cell =
          frame.regs[reg].as_watch_cell();
      if (cell != nullptr) {
        const RuntimeWatchWriteResult write = cell->write(std::move(value));
        record_watch_write(cell, write);
        frame.initialized[reg] = 1U;
        if (!frame.prepared_seq_regs.empty() ||
            !frame.prepared_map_regs.empty() ||
            !frame.pending_pattern_bindings.empty()) {
          frame.prepared_seq_regs.erase(reg);
          frame.prepared_map_regs.erase(reg);
          frame.pending_pattern_bindings.erase(reg);
        }
        return true;
      }
    }
    invalidate_integer_reg(frame, reg);
    frame.regs[reg] = std::move(value);
    frame.initialized[reg] = 1U;
    if (!frame.prepared_seq_regs.empty() || !frame.prepared_map_regs.empty() ||
        !frame.pending_pattern_bindings.empty()) {
      frame.prepared_seq_regs.erase(reg);
      frame.prepared_map_regs.erase(reg);
      frame.pending_pattern_bindings.erase(reg);
    }
    return true;
  }

  std::shared_ptr<RuntimeWatchCell>
  ensure_local_storage_cell(Frame &frame, std::uint32_t slot,
                            std::optional<Value> seed = {}) {
    if (slot >= frame.regs.size()) {
      set_fault(frame, "VMError", "register out of range");
      return nullptr;
    }
    if (frame.initialized.size() < frame.regs.size()) {
      frame.initialized.resize(frame.regs.size(), 0U);
    }
    if (frame.initialized[slot] == 0U && !seed.has_value()) {
      set_fault(frame, "NameError", "read of uninitialized local/module cell");
      return nullptr;
    }
    if (!materialize_integer_reg_if_needed(frame, slot)) {
      return nullptr;
    }
    if (frame.initialized[slot] != 0U && frame.regs[slot].is_watch_cell()) {
      return frame.regs[slot].as_watch_cell();
    }
    const std::string target_name =
        frame.code == nullptr ? "l" + std::to_string(slot)
                              : local_name_for_slot(*frame.code, slot);
    std::shared_ptr<RuntimeWatchCell> cell = state_->make_watch_cell(
        seed.has_value() ? std::move(*seed) : frame.regs[slot], target_name);
    frame.regs[slot] = Value::watch_cell(cell);
    frame.initialized[slot] = 1U;
    invalidate_integer_reg(frame, slot);
    return cell;
  }

  std::shared_ptr<RuntimeWatchCell>
  ensure_capture_storage_cell(Frame &frame, std::uint32_t slot) {
    if (slot >= frame.captures.size()) {
      set_fault(frame, "VMError", "capture slot out of range");
      return nullptr;
    }
    if (frame.captures[slot].is_watch_cell()) {
      return frame.captures[slot].as_watch_cell();
    }
    const std::string target_name =
        frame.code == nullptr ? "u" + std::to_string(slot)
                              : capture_name_for_slot(*frame.code, slot);
    std::shared_ptr<RuntimeWatchCell> cell =
        state_->make_watch_cell(frame.captures[slot], target_name);
    frame.captures[slot] = Value::watch_cell(cell);
    return cell;
  }

  std::shared_ptr<RuntimeWatchHandle>
  watch_cell(const std::shared_ptr<RuntimeWatchCell> &cell,
             std::string target_name) {
    if (cell == nullptr) {
      return nullptr;
    }
    std::shared_ptr<RuntimeWatchHandle> handle =
        state_->make_watch_handle(cell, std::move(target_name));
    state_->record_watch_event(make_watch_event(
        "watch.binding", cell->snapshot(), nullptr, handle->handle_id()));
    return handle;
  }

  std::shared_ptr<RuntimeWatchHandle>
  watch_ivar(Frame &frame, const std::shared_ptr<InstanceValue> &instance,
             const std::string &field_name) {
    if (instance == nullptr) {
      set_fault(frame, "TypeError", "instance receiver is null");
      return nullptr;
    }
    if (!ensure_instance_layout(frame, instance)) {
      return nullptr;
    }
    const Value current_value =
        instance_ivar_value_or_null(instance, field_name);
    std::shared_ptr<RuntimeWatchObjectState> object_state =
        ensure_watch_object_state(instance);
    if (object_state == nullptr) {
      set_fault(frame, "VMError", "watch object state is missing");
      return nullptr;
    }
    const std::string target_name = "@" + field_name;
    std::shared_ptr<RuntimeWatchHandle> handle =
        state_->make_watch_handle(object_state, target_name, field_name);
    if (handle == nullptr) {
      set_fault(frame, "VMError", "watch handle allocation failed");
      return nullptr;
    }
    const RuntimeWatchIvarSnapshot snapshot =
        object_state->snapshot_field(field_name, current_value);
    state_->record_watch_event(make_watch_ivar_event(
        "watch.ivar", snapshot, nullptr, handle->handle_id()));
    return handle;
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
    if (value.is_set()) {
      const std::shared_ptr<SetValue> set = value.as_set();
      if (set == nullptr) {
        set_fault(frame, "TypeError", "set value is null");
        return std::nullopt;
      }
      if (!ensure_lifecycle_access(frame, value)) {
        return std::nullopt;
      }
      *out_source_was_tuple = false;
      return set->items;
    }
    if (value_is_range_instance(value)) {
      *out_source_was_tuple = false;
      return extract_range_items(frame, value);
    }
    return std::nullopt;
  }

  const std::vector<Value> *sequence_items_view(const Frame &frame,
                                                const Value &value) {
    if (value.is_list()) {
      const std::shared_ptr<ListValue> list = value.as_list();
      if (list == nullptr) {
        set_fault(frame, "TypeError", "list value is null");
        return nullptr;
      }
      if (!ensure_lifecycle_access(frame, value)) {
        return nullptr;
      }
      return &list->items;
    }
    if (value.is_tuple()) {
      const std::shared_ptr<TupleValue> tuple = value.as_tuple();
      if (tuple == nullptr) {
        set_fault(frame, "TypeError", "tuple value is null");
        return nullptr;
      }
      if (!ensure_lifecycle_access(frame, value)) {
        return nullptr;
      }
      return &tuple->items;
    }
    if (value.is_set()) {
      const std::shared_ptr<SetValue> set = value.as_set();
      if (set == nullptr) {
        set_fault(frame, "TypeError", "set value is null");
        return nullptr;
      }
      if (!ensure_lifecycle_access(frame, value)) {
        return nullptr;
      }
      return &set->items;
    }
    return nullptr;
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

  std::optional<std::uint32_t>
  map_key_symbol_id_from_value(const Frame &frame, const Value &key,
                               const std::string &context) {
    if (key.is_symbol()) {
      const std::uint32_t symbol_id = key.as_symbol().symbol_id;
      if (symbol_id >= module_.symbols.size()) {
        set_fault(frame, "VMError", context + " symbol key ref is invalid");
        return std::nullopt;
      }
      return symbol_id;
    }
    if (key.is_string()) {
      const std::optional<std::string> text =
          string_text_from_id(key.as_string().string_id);
      if (!text.has_value()) {
        set_fault(frame, "VMError", context + " string key ref is invalid");
        return std::nullopt;
      }
      const std::optional<std::uint32_t> symbol_id = symbol_id_for_text(*text);
      if (!symbol_id.has_value()) {
        set_fault(frame, "TypeError",
                  context + " string key must name an interned Symbol");
        return std::nullopt;
      }
      return *symbol_id;
    }
    set_fault(frame, "TypeError", context + " key must be Symbol or String");
    return std::nullopt;
  }

  std::optional<MapEntry> map_entry_from_transform_result(const Frame &frame,
                                                          const Value &result) {
    if (!ensure_lifecycle_access(frame, result)) {
      return std::nullopt;
    }

    std::vector<Value> items;
    if (result.is_tuple()) {
      const std::shared_ptr<TupleValue> tuple = result.as_tuple();
      if (tuple == nullptr) {
        set_fault(frame, "TypeError",
                  "Map#transform block returned null tuple");
        return std::nullopt;
      }
      items = tuple->items;
    } else if (result.is_list()) {
      const std::shared_ptr<ListValue> list = result.as_list();
      if (list == nullptr) {
        set_fault(frame, "TypeError", "Map#transform block returned null list");
        return std::nullopt;
      }
      items = list->items;
    } else {
      set_fault(frame, "TypeError",
                "Map#transform block must return key/value tuple or list");
      return std::nullopt;
    }

    if (items.size() != 2U) {
      set_fault(frame, "TypeError",
                "Map#transform block must return exactly two values");
      return std::nullopt;
    }
    const std::optional<std::uint32_t> symbol_id =
        map_key_symbol_id_from_value(frame, items[0], "Map#transform");
    if (!symbol_id.has_value()) {
      return std::nullopt;
    }
    return MapEntry{*symbol_id, items[1]};
  }

  void upsert_map_entry(std::vector<MapEntry> *entries, MapEntry entry) {
    auto existing = std::find_if(
        entries->begin(), entries->end(), [&](const MapEntry &candidate) {
          return candidate.symbol_id == entry.symbol_id;
        });
    if (existing == entries->end()) {
      entries->push_back(std::move(entry));
      return;
    }
    existing->value = std::move(entry.value);
  }

  bool sequence_contains_value(const std::vector<Value> &items,
                               const Value &needle) const {
    return std::find_if(items.begin(), items.end(), [&](const Value &item) {
             return value_equals(item, needle);
           }) != items.end();
  }

  static std::string
  canonical_collection_selector(const std::string &selector) {
    if (selector == "collect") {
      return "map";
    }
    if (selector == "collect_concat") {
      return "flat_map";
    }
    if (selector == "filter" || selector == "find_all") {
      return "select";
    }
    if (selector == "detect") {
      return "find";
    }
    if (selector == "inject") {
      return "reduce";
    }
    if (selector == "member?" || selector == "includes?" ||
        selector == "has_key?" || selector == "key?") {
      return "include?";
    }
    if (selector == "each_slice") {
      return "each";
    }
    if (selector == "entries") {
      return "to_a";
    }
    if (selector == "length" || selector == "size") {
      return "count";
    }
    return selector;
  }

  static bool collection_size_selector(const std::string &selector) {
    const std::string canonical = canonical_collection_selector(selector);
    return canonical == "count";
  }

  void append_unique_value(std::vector<Value> *items, const Value &value) {
    if (!sequence_contains_value(*items, value)) {
      items->push_back(value);
    }
  }

  std::vector<Value> unique_sequence_items(const std::vector<Value> &items) {
    std::vector<Value> unique;
    unique.reserve(items.size());
    for (const Value &item : items) {
      append_unique_value(&unique, item);
    }
    return unique;
  }

  Value materialize_set_like_result(const Value &receiver,
                                    std::vector<Value> items) {
    if (receiver.is_set()) {
      return make_set_value(std::move(items));
    }
    return make_list_value(std::move(items));
  }

  std::optional<std::vector<Value>>
  sequence_argument_items(const Frame &frame, const Value &value,
                          const std::string &context) {
    bool source_was_tuple = false;
    const std::optional<std::vector<Value>> items =
        extract_sequence_items(frame, value, &source_was_tuple);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (!items.has_value()) {
      set_fault(frame, "TypeError", context + " expects sequence argument");
      return std::nullopt;
    }
    return items;
  }

  std::vector<Value> sequence_union_items(const std::vector<Value> &left,
                                          const std::vector<Value> &right) {
    std::vector<Value> merged;
    merged.reserve(left.size() + right.size());
    for (const Value &item : left) {
      append_unique_value(&merged, item);
    }
    for (const Value &item : right) {
      append_unique_value(&merged, item);
    }
    return merged;
  }

  std::vector<Value>
  sequence_intersection_items(const std::vector<Value> &left,
                              const std::vector<Value> &right) {
    std::vector<Value> intersection;
    for (const Value &item : left) {
      if (sequence_contains_value(right, item)) {
        append_unique_value(&intersection, item);
      }
    }
    return intersection;
  }

  std::vector<Value>
  sequence_difference_items(const std::vector<Value> &left,
                            const std::vector<Value> &right) {
    std::vector<Value> difference;
    for (const Value &item : left) {
      if (!sequence_contains_value(right, item)) {
        append_unique_value(&difference, item);
      }
    }
    return difference;
  }

  std::vector<Value>
  sequence_symmetric_difference_items(const std::vector<Value> &left,
                                      const std::vector<Value> &right) {
    std::vector<Value> difference = sequence_difference_items(left, right);
    for (const Value &item : sequence_difference_items(right, left)) {
      append_unique_value(&difference, item);
    }
    return difference;
  }

  bool sequence_is_subset(const std::vector<Value> &left,
                          const std::vector<Value> &right) {
    for (const Value &item : unique_sequence_items(left)) {
      if (!sequence_contains_value(right, item)) {
        return false;
      }
    }
    return true;
  }

  std::optional<std::int64_t> parse_step_keyword(
      const Frame &frame,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      std::int64_t default_step) {
    std::optional<std::int64_t> step;
    for (const auto &[symbol_id, value] : kw_args) {
      if (symbol_id >= module_.symbols.size()) {
        set_fault(frame, "VMError", "keyword symbol ref is out of range");
        return std::nullopt;
      }
      if (module_.symbols[symbol_id] != "step") {
        set_fault(frame, "TypeError", "unknown keyword argument");
        return std::nullopt;
      }
      if (step.has_value()) {
        set_fault(frame, "TypeError", "duplicate keyword argument");
        return std::nullopt;
      }
      if (!value.is_integer()) {
        set_fault(frame, "TypeError", "each step keyword must be Integer");
        return std::nullopt;
      }
      step = value.as_integer();
    }
    return step.value_or(default_step);
  }

  std::optional<std::vector<Value>>
  sequence_windows(const Frame &frame, const std::vector<Value> &items,
                   std::int64_t raw_size, std::int64_t raw_step,
                   const std::string &context) {
    if (raw_size <= 0) {
      set_fault(frame, "ArgumentError", context + " window size must be > 0");
      return std::nullopt;
    }
    if (raw_step <= 0) {
      set_fault(frame, "ArgumentError", context + " step must be > 0");
      return std::nullopt;
    }
    const std::size_t size = static_cast<std::size_t>(raw_size);
    const std::size_t step = static_cast<std::size_t>(raw_step);
    std::vector<Value> windows;
    if (size > items.size()) {
      return windows;
    }
    for (std::size_t start = 0; start + size <= items.size(); start += step) {
      windows.push_back(make_list_value(std::vector<Value>(
          items.begin() + static_cast<std::ptrdiff_t>(start),
          items.begin() + static_cast<std::ptrdiff_t>(start + size))));
      if (items.size() - start <= step) {
        break;
      }
    }
    return windows;
  }

  std::vector<Value> permutation_values(const std::vector<Value> &items,
                                        std::size_t count) {
    std::vector<Value> permutations;
    std::vector<Value> current;
    std::vector<bool> used(items.size(), false);
    current.reserve(count);
    std::function<void()> visit = [&]() {
      if (current.size() == count) {
        permutations.push_back(make_list_value(current));
        return;
      }
      for (std::size_t i = 0; i < items.size(); ++i) {
        if (used[i]) {
          continue;
        }
        used[i] = true;
        current.push_back(items[i]);
        visit();
        current.pop_back();
        used[i] = false;
      }
    };
    visit();
    return permutations;
  }

  std::vector<Value> combination_values(const std::vector<Value> &items,
                                        std::size_t count) {
    std::vector<Value> combinations;
    std::vector<Value> current;
    current.reserve(count);
    std::function<void(std::size_t)> visit = [&](std::size_t start) {
      if (current.size() == count) {
        combinations.push_back(make_list_value(current));
        return;
      }
      const std::size_t remaining = count - current.size();
      for (std::size_t i = start; i + remaining <= items.size(); ++i) {
        current.push_back(items[i]);
        visit(i + 1U);
        current.pop_back();
      }
    };
    visit(0U);
    return combinations;
  }

  std::optional<int> compare_values_for_sort(const Frame &frame,
                                             const Value &left,
                                             const Value &right) {
    if (left.is_integer() && right.is_integer()) {
      const std::int64_t lhs = left.as_integer();
      const std::int64_t rhs = right.as_integer();
      return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
    }
    if ((left.is_integer() || left.is_float()) &&
        (right.is_integer() || right.is_float())) {
      const double lhs = left.is_integer()
                             ? static_cast<double>(left.as_integer())
                             : left.as_float();
      const double rhs = right.is_integer()
                             ? static_cast<double>(right.as_integer())
                             : right.as_float();
      return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
    }
    if (left.is_bool() && right.is_bool()) {
      return left.as_bool() == right.as_bool() ? 0 : (left.as_bool() ? 1 : -1);
    }
    if (left.is_string() && right.is_string()) {
      const std::optional<std::string> lhs =
          string_text_from_id(left.as_string().string_id);
      const std::optional<std::string> rhs =
          string_text_from_id(right.as_string().string_id);
      if (!lhs.has_value() || !rhs.has_value()) {
        set_fault(frame, "VMError", "sort string ref is invalid");
        return std::nullopt;
      }
      return *lhs < *rhs ? -1 : (*lhs > *rhs ? 1 : 0);
    }
    if (left.is_symbol() && right.is_symbol()) {
      const std::uint32_t lhs_id = left.as_symbol().symbol_id;
      const std::uint32_t rhs_id = right.as_symbol().symbol_id;
      if (lhs_id >= module_.symbols.size() ||
          rhs_id >= module_.symbols.size()) {
        set_fault(frame, "VMError", "sort symbol ref is invalid");
        return std::nullopt;
      }
      const std::string &lhs = module_.symbols[lhs_id];
      const std::string &rhs = module_.symbols[rhs_id];
      return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
    }
    set_fault(frame, "TypeError", "sort values are not comparable");
    return std::nullopt;
  }

  std::optional<int> compare_values_for_sort(const Frame &frame,
                                             const Value &receiver,
                                             const Value &block,
                                             const Value &left,
                                             const Value &right) {
    if (block.is_null()) {
      return compare_values_for_sort(frame, left, right);
    }
    const std::optional<Value> value =
        call_block_to_value(frame, block, {left, right});
    if (!value.has_value()) {
      return std::nullopt;
    }
    if (!ensure_lifecycle_access(frame, receiver)) {
      return std::nullopt;
    }
    if (!value->is_integer()) {
      set_fault(frame, "TypeError", "sort block must return Integer");
      return std::nullopt;
    }
    const std::int64_t result = value->as_integer();
    return result < 0 ? -1 : (result > 0 ? 1 : 0);
  }

  std::optional<std::vector<Value>>
  sorted_sequence_items(const Frame &frame, const Value &receiver,
                        const std::vector<Value> &items, const Value &block) {
    std::vector<Value> sorted = items;
    for (std::size_t i = 1; i < sorted.size(); ++i) {
      std::size_t j = i;
      while (j > 0) {
        const std::optional<int> order = compare_values_for_sort(
            frame, receiver, block, sorted[j], sorted[j - 1U]);
        if (!order.has_value()) {
          return std::nullopt;
        }
        if (*order >= 0) {
          break;
        }
        std::swap(sorted[j], sorted[j - 1U]);
        --j;
      }
    }
    return sorted;
  }

  std::optional<Value> apply_sequence_set_operation(
      const Frame &frame, const Value &receiver, const std::string &selector,
      const std::vector<Value> &items, const std::vector<Value> &args,
      const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args) {
    const bool block_allowed = selector == "take_while" || selector == "sort" ||
                               selector == "uniq" || selector == "each_pair" ||
                               selector == "each_cons" || selector == "each";
    if (!block.is_null() && !block_allowed) {
      set_fault(frame, "TypeError",
                "collection operation does not accept block arguments");
      return std::nullopt;
    }
    const bool keywords_allowed = selector == "each";
    if (!kw_args.empty() && !keywords_allowed) {
      set_fault(frame, "TypeError",
                "collection operation does not accept keyword arguments");
      return std::nullopt;
    }

    if (selector == "contains?" || selector == "include?") {
      if (args.size() != 1U) {
        set_fault(frame, "TypeError", "wrong builtin SEND arity");
        return std::nullopt;
      }
      return Value::boolean(sequence_contains_value(items, args[0]));
    }

    if (selector == "+" || selector == "concat") {
      if (args.size() != 1U) {
        set_fault(frame, "TypeError", "wrong builtin SEND arity");
        return std::nullopt;
      }
      const std::optional<std::vector<Value>> right =
          sequence_argument_items(frame, args[0], selector);
      if (!right.has_value()) {
        return std::nullopt;
      }
      std::vector<Value> concatenated = items;
      concatenated.insert(concatenated.end(), right->begin(), right->end());
      return make_list_value(std::move(concatenated));
    }

    if (selector == "*") {
      if (args.size() != 1U || !args[0].is_integer()) {
        set_fault(frame, "TypeError", "repeat expects integer count");
        return std::nullopt;
      }
      const std::int64_t count = args[0].as_integer();
      std::vector<Value> repeated;
      if (count > 0) {
        repeated.reserve(items.size() * static_cast<std::size_t>(count));
        for (std::int64_t i = 0; i < count; ++i) {
          repeated.insert(repeated.end(), items.begin(), items.end());
        }
      }
      return make_list_value(std::move(repeated));
    }

    if (selector == "reverse") {
      if (!args.empty()) {
        set_fault(frame, "TypeError", "wrong builtin SEND arity");
        return std::nullopt;
      }
      std::vector<Value> reversed(items.rbegin(), items.rend());
      return make_list_value(std::move(reversed));
    }

    if (selector == "take_while") {
      if (!args.empty() || block.is_null()) {
        set_fault(frame, "TypeError", "take_while requires block");
        return std::nullopt;
      }
      std::vector<Value> taken;
      for (const Value &item : items) {
        const std::optional<Value> predicate =
            call_block_to_value(frame, block, {item});
        if (!predicate.has_value()) {
          return std::nullopt;
        }
        if (!ensure_lifecycle_access(frame, receiver)) {
          return std::nullopt;
        }
        if (!is_truthy(*predicate)) {
          break;
        }
        taken.push_back(item);
      }
      return make_list_value(std::move(taken));
    }

    if (selector == "sort") {
      if (!args.empty()) {
        set_fault(frame, "TypeError", "wrong builtin SEND arity");
        return std::nullopt;
      }
      const std::optional<std::vector<Value>> sorted =
          sorted_sequence_items(frame, receiver, items, block);
      if (!sorted.has_value()) {
        return std::nullopt;
      }
      return make_list_value(*sorted);
    }

    if (selector == "uniq") {
      if (!args.empty()) {
        set_fault(frame, "TypeError", "wrong builtin SEND arity");
        return std::nullopt;
      }
      std::vector<Value> unique;
      std::vector<Value> keys;
      unique.reserve(items.size());
      keys.reserve(items.size());
      for (const Value &item : items) {
        Value key = item;
        if (!block.is_null()) {
          const std::optional<Value> value =
              call_block_to_value(frame, block, {item});
          if (!value.has_value()) {
            return std::nullopt;
          }
          if (!ensure_lifecycle_access(frame, receiver)) {
            return std::nullopt;
          }
          key = *value;
        }
        if (!sequence_contains_value(keys, key)) {
          keys.push_back(key);
          unique.push_back(item);
        }
      }
      return materialize_set_like_result(receiver, std::move(unique));
    }

    if (selector == "each" || selector == "each_pair" ||
        selector == "each_cons") {
      std::int64_t raw_size = 0;
      std::int64_t raw_step = 1;
      if (selector == "each_pair") {
        if (!args.empty()) {
          set_fault(frame, "TypeError", "wrong builtin SEND arity");
          return std::nullopt;
        }
        raw_size = 2;
      } else {
        if (args.size() != 1U || !args[0].is_integer()) {
          set_fault(frame, "TypeError", selector + " expects integer size");
          return std::nullopt;
        }
        raw_size = args[0].as_integer();
        raw_step = selector == "each" ? raw_size : 1;
      }
      const std::optional<std::int64_t> step =
          parse_step_keyword(frame, kw_args, raw_step);
      if (!step.has_value()) {
        return std::nullopt;
      }
      const std::optional<std::vector<Value>> windows =
          sequence_windows(frame, items, raw_size, *step, selector);
      if (!windows.has_value()) {
        return std::nullopt;
      }
      if (block.is_null()) {
        return make_list_value(*windows);
      }
      for (const Value &window : *windows) {
        if (!call_block_to_value(frame, block, {window}).has_value()) {
          return std::nullopt;
        }
        if (!ensure_lifecycle_access(frame, receiver)) {
          return std::nullopt;
        }
      }
      return receiver;
    }

    if (selector == "permutation") {
      std::int64_t raw_count = static_cast<std::int64_t>(items.size());
      if (args.size() > 1U) {
        set_fault(frame, "TypeError", "wrong builtin SEND arity");
        return std::nullopt;
      }
      if (!args.empty()) {
        if (!args[0].is_integer()) {
          set_fault(frame, "TypeError", "permutation expects integer count");
          return std::nullopt;
        }
        raw_count = args[0].as_integer();
      }
      if (raw_count < 0 ||
          static_cast<std::uint64_t>(raw_count) > items.size()) {
        return make_list_value({});
      }
      return make_list_value(
          permutation_values(items, static_cast<std::size_t>(raw_count)));
    }

    if (selector == "combination") {
      if (args.size() != 1U || !args[0].is_integer()) {
        set_fault(frame, "TypeError", "combination expects integer count");
        return std::nullopt;
      }
      const std::int64_t raw_count = args[0].as_integer();
      if (raw_count < 0 ||
          static_cast<std::uint64_t>(raw_count) > items.size()) {
        return make_list_value({});
      }
      return make_list_value(
          combination_values(items, static_cast<std::size_t>(raw_count)));
    }

    if (args.size() != 1U) {
      set_fault(frame, "TypeError", "wrong builtin SEND arity");
      return std::nullopt;
    }
    const std::optional<std::vector<Value>> right =
        sequence_argument_items(frame, args[0], selector);
    if (!right.has_value()) {
      return std::nullopt;
    }

    if (selector == "union") {
      return materialize_set_like_result(receiver,
                                         sequence_union_items(items, *right));
    }
    if (selector == "|") {
      return materialize_set_like_result(receiver,
                                         sequence_union_items(items, *right));
    }
    if (selector == "intersection") {
      return materialize_set_like_result(
          receiver, sequence_intersection_items(items, *right));
    }
    if (selector == "&") {
      return materialize_set_like_result(
          receiver, sequence_intersection_items(items, *right));
    }
    if (selector == "difference" || selector == "left_difference" ||
        selector == "-") {
      return materialize_set_like_result(
          receiver, sequence_difference_items(items, *right));
    }
    if (selector == "symmetric_difference" || selector == "^") {
      return materialize_set_like_result(
          receiver, sequence_symmetric_difference_items(items, *right));
    }
    if (selector == "subset?" || selector == "proper_subset?" ||
        selector == "<=" || selector == "<") {
      const bool subset = sequence_is_subset(items, *right);
      const bool proper = unique_sequence_items(items).size() <
                          unique_sequence_items(*right).size();
      return Value::boolean((selector == "subset?" || selector == "<=")
                                ? subset
                                : subset && proper);
    }
    if (selector == "superset?" || selector == "proper_superset?" ||
        selector == ">=" || selector == ">") {
      const bool superset = sequence_is_subset(*right, items);
      const bool proper = unique_sequence_items(items).size() >
                          unique_sequence_items(*right).size();
      return Value::boolean((selector == "superset?" || selector == ">=")
                                ? superset
                                : superset && proper);
    }
    if (selector == "disjoint?") {
      return Value::boolean(sequence_intersection_items(items, *right).empty());
    }

    set_fault(frame, "VMError", "unknown collection operation selector");
    return std::nullopt;
  }

  std::optional<Value> merge_map_entries(const Frame &frame,
                                         const Value &receiver,
                                         const std::vector<MapEntry> &left,
                                         const Value &right_value,
                                         const Value &block) {
    const std::optional<std::vector<MapEntry>> right =
        extract_map_entries(frame, right_value);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (!right.has_value()) {
      set_fault(frame, "TypeError", "Map#merge expects Map argument");
      return std::nullopt;
    }

    std::vector<MapEntry> merged = left;
    for (const MapEntry &entry : *right) {
      auto existing = std::find_if(
          merged.begin(), merged.end(), [&](const MapEntry &candidate) {
            return candidate.symbol_id == entry.symbol_id;
          });
      Value value = entry.value;
      if (existing != merged.end() && !block.is_null()) {
        const std::optional<Value> merged_value = call_block_to_value(
            frame, block,
            {Value::symbol(entry.symbol_id), existing->value, entry.value});
        if (!merged_value.has_value()) {
          return std::nullopt;
        }
        if (!ensure_lifecycle_access(frame, receiver) ||
            !ensure_lifecycle_access(frame, right_value)) {
          return std::nullopt;
        }
        value = *merged_value;
      }
      if (existing == merged.end()) {
        merged.push_back({entry.symbol_id, value});
      } else {
        existing->value = value;
      }
    }
    return make_symbol_map_value(std::move(merged));
  }

  bool value_is_range_instance(const Value &value) {
    if (!value.is_instance_object()) {
      return false;
    }
    const std::shared_ptr<InstanceValue> instance = value.as_instance_object();
    if (instance == nullptr) {
      return false;
    }
    const std::optional<std::string> class_name =
        class_name_for_index(instance->class_index);
    return class_name.has_value() && *class_name == "Range";
  }

  std::optional<Value>
  load_instance_ivar_or_null(const Frame &frame,
                             const std::shared_ptr<InstanceValue> &instance,
                             const std::string &name) {
    const std::optional<std::uint32_t> slot =
        ensure_instance_ivar_slot(frame, instance, name);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    const Value value =
        !slot.has_value() || *slot >= instance->ivar_storage.size()
            ? Value::null()
            : instance->ivar_storage[*slot];
    return record_watch_ivar_read(instance, name, value);
  }

  struct RangeBounds {
    std::optional<std::int64_t> start;
    std::optional<std::int64_t> finish;
    bool inclusive_end = true;
  };

  std::optional<RangeBounds> extract_range_bounds(const Frame &frame,
                                                  const Value &value) {
    if (!value.is_instance_object()) {
      return std::nullopt;
    }
    const std::shared_ptr<InstanceValue> instance = value.as_instance_object();
    if (instance == nullptr) {
      set_fault(frame, "TypeError", "Range value is null");
      return std::nullopt;
    }
    if (!ensure_instance_layout(frame, instance)) {
      return std::nullopt;
    }

    const std::optional<Value> start =
        load_instance_ivar_or_null(frame, instance, "start");
    const std::optional<Value> finish =
        load_instance_ivar_or_null(frame, instance, "finish");
    const std::optional<Value> inclusive_end =
        load_instance_ivar_or_null(frame, instance, "inclusive_end");
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (!start->is_null() && !start->is_integer()) {
      set_fault(frame, "TypeError", "Range start must be Integer or null");
      return std::nullopt;
    }
    if (!finish->is_null() && !finish->is_integer()) {
      set_fault(frame, "TypeError", "Range finish must be Integer or null");
      return std::nullopt;
    }
    if (!inclusive_end->is_null() && !inclusive_end->is_bool()) {
      set_fault(frame, "TypeError", "Range inclusive_end must be Bool");
      return std::nullopt;
    }
    if (start->is_null() && finish->is_null()) {
      set_fault(frame, "TypeError", "Range bounds must be Integer values");
      return std::nullopt;
    }
    RangeBounds bounds;
    if (start->is_integer()) {
      bounds.start = start->as_integer();
    }
    if (finish->is_integer()) {
      bounds.finish = finish->as_integer();
    }
    bounds.inclusive_end =
        inclusive_end->is_null() ? true : inclusive_end->as_bool();
    return bounds;
  }

  std::optional<std::vector<Value>> extract_range_items(const Frame &frame,
                                                        const Value &value) {
    const std::optional<RangeBounds> bounds =
        extract_range_bounds(frame, value);
    if (fault_.has_value() || !bounds.has_value()) {
      return std::nullopt;
    }

    std::vector<Value> items;
    if (!bounds->start.has_value() || !bounds->finish.has_value()) {
      set_fault(frame, "ArgumentError",
                "open-ended Range cannot be materialized eagerly");
      return std::nullopt;
    }
    if (*bounds->start > *bounds->finish) {
      return items;
    }
    for (std::int64_t current = *bounds->start; current < *bounds->finish;
         ++current) {
      items.push_back(Value::integer(current));
    }
    if (bounds->inclusive_end) {
      items.push_back(Value::integer(*bounds->finish));
    }
    return items;
  }

  bool range_contains_value(const Frame &frame, const Value &range,
                            const Value &needle, bool *out) {
    const std::optional<RangeBounds> bounds =
        extract_range_bounds(frame, range);
    if (fault_.has_value() || !bounds.has_value()) {
      return false;
    }
    if (!needle.is_integer()) {
      set_fault(frame, "TypeError", "Range#contains? expects Integer value");
      return false;
    }
    const std::int64_t value = needle.as_integer();
    const bool above_start =
        !bounds->start.has_value() || value >= *bounds->start;
    const bool below_finish = !bounds->finish.has_value() ||
                              (bounds->inclusive_end ? value <= *bounds->finish
                                                     : value < *bounds->finish);
    *out = above_start && below_finish;
    return true;
  }

  bool value_is_lazy_seq_instance(const Value &value) {
    if (!value.is_instance_object()) {
      return false;
    }
    const std::shared_ptr<InstanceValue> instance = value.as_instance_object();
    if (instance == nullptr) {
      return false;
    }
    const auto marker = instance->ivars.find("__amber_lazy_seq");
    return marker != instance->ivars.end() && marker->second.is_bool() &&
           marker->second.as_bool();
  }

  std::optional<LazySeqOpKind>
  lazy_seq_op_kind_for_selector(const std::string &selector) const {
    if (selector == "map") {
      return LazySeqOpKind::Map;
    }
    if (selector == "flat_map") {
      return LazySeqOpKind::FlatMap;
    }
    if (selector == "select") {
      return LazySeqOpKind::Select;
    }
    if (selector == "reject") {
      return LazySeqOpKind::Reject;
    }
    return std::nullopt;
  }

  Value encode_lazy_seq_ops(const std::vector<LazySeqOp> &ops) {
    std::vector<Value> encoded;
    encoded.reserve(ops.size());
    for (const LazySeqOp &op : ops) {
      encoded.push_back(make_tuple_value(
          {Value::integer(static_cast<std::int64_t>(op.kind)), op.block}));
    }
    return make_list_value(std::move(encoded));
  }

  Value make_lazy_seq_value(const Value &source,
                            const std::vector<LazySeqOp> &ops) {
    std::shared_ptr<InstanceValue> instance =
        make_instance_value(std::numeric_limits<std::uint32_t>::max());
    instance->ivars["__amber_lazy_seq"] = Value::boolean(true);
    instance->ivars["__amber_lazy_source"] = source;
    instance->ivars["__amber_lazy_ops"] = encode_lazy_seq_ops(ops);
    return Value::instance(std::move(instance));
  }

  std::optional<LazySeqState> extract_lazy_seq_state(const Frame &frame,
                                                     const Value &value) {
    if (!value_is_lazy_seq_instance(value)) {
      return std::nullopt;
    }
    if (!ensure_lifecycle_access(frame, value)) {
      return std::nullopt;
    }
    const std::shared_ptr<InstanceValue> instance = value.as_instance_object();
    const auto source_it = instance->ivars.find("__amber_lazy_source");
    const auto ops_it = instance->ivars.find("__amber_lazy_ops");
    if (source_it == instance->ivars.end() || ops_it == instance->ivars.end()) {
      set_fault(frame, "VMError", "LazySeq state is missing");
      return std::nullopt;
    }
    if (!ops_it->second.is_list()) {
      set_fault(frame, "VMError", "LazySeq ops must be a list");
      return std::nullopt;
    }
    const std::shared_ptr<ListValue> encoded_ops = ops_it->second.as_list();
    if (encoded_ops == nullptr) {
      set_fault(frame, "TypeError", "LazySeq ops list is null");
      return std::nullopt;
    }
    if (!ensure_lifecycle_access(frame, ops_it->second)) {
      return std::nullopt;
    }

    LazySeqState state;
    state.source = source_it->second;
    state.ops.reserve(encoded_ops->items.size());
    for (const Value &encoded : encoded_ops->items) {
      if (!encoded.is_tuple()) {
        set_fault(frame, "VMError", "LazySeq op must be a tuple");
        return std::nullopt;
      }
      const std::shared_ptr<TupleValue> tuple = encoded.as_tuple();
      if (tuple == nullptr || tuple->items.size() != 2U ||
          !tuple->items[0].is_integer()) {
        set_fault(frame, "VMError", "LazySeq op tuple is invalid");
        return std::nullopt;
      }
      const std::int64_t op_code = tuple->items[0].as_integer();
      if (op_code < static_cast<std::int64_t>(LazySeqOpKind::Map) ||
          op_code > static_cast<std::int64_t>(LazySeqOpKind::Reject)) {
        set_fault(frame, "VMError", "LazySeq op code is invalid");
        return std::nullopt;
      }
      if (!tuple->items[1].is_closure()) {
        set_fault(frame, "TypeError", "LazySeq pipeline block must be closure");
        return std::nullopt;
      }
      state.ops.push_back(
          {static_cast<LazySeqOpKind>(op_code), tuple->items[1]});
    }
    return state;
  }

  std::optional<Value> append_lazy_seq_op(const Frame &frame,
                                          const Value &receiver,
                                          LazySeqOpKind kind,
                                          const Value &block) {
    if (block.is_null()) {
      set_fault(frame, "TypeError", "LazySeq pipeline requires block");
      return std::nullopt;
    }
    if (!block.is_closure()) {
      set_fault(frame, "TypeError", "LazySeq pipeline block must be closure");
      return std::nullopt;
    }
    if (!ensure_lifecycle_access(frame, block)) {
      return std::nullopt;
    }
    std::optional<LazySeqState> state = extract_lazy_seq_state(frame, receiver);
    if (fault_.has_value()) {
      return std::nullopt;
    }
    if (!state.has_value()) {
      state = LazySeqState{receiver, {}};
    }
    state->ops.push_back({kind, block});
    return make_lazy_seq_value(state->source, state->ops);
  }

  LazySeqVisitStatus emit_lazy_seq_value(
      const Frame &frame, const LazySeqState &state, const Value &receiver,
      const Value &item, std::size_t op_index,
      const std::function<LazySeqVisitStatus(const Value &)> &visitor) {
    if (op_index >= state.ops.size()) {
      return visitor(item);
    }

    const LazySeqOp &op = state.ops[op_index];
    const std::optional<Value> value =
        call_block_to_value(frame, op.block, {item});
    if (!value.has_value()) {
      return LazySeqVisitStatus::Faulted;
    }
    if (!ensure_lifecycle_access(frame, receiver) ||
        !ensure_lifecycle_access(frame, state.source)) {
      return LazySeqVisitStatus::Faulted;
    }

    if (op.kind == LazySeqOpKind::Map) {
      return emit_lazy_seq_value(frame, state, receiver, *value, op_index + 1U,
                                 visitor);
    }
    if (op.kind == LazySeqOpKind::Select) {
      if (!is_truthy(*value)) {
        return LazySeqVisitStatus::Continue;
      }
      return emit_lazy_seq_value(frame, state, receiver, item, op_index + 1U,
                                 visitor);
    }
    if (op.kind == LazySeqOpKind::Reject) {
      if (is_truthy(*value)) {
        return LazySeqVisitStatus::Continue;
      }
      return emit_lazy_seq_value(frame, state, receiver, item, op_index + 1U,
                                 visitor);
    }

    bool nested_was_tuple = false;
    const std::optional<std::vector<Value>> nested =
        extract_sequence_items(frame, *value, &nested_was_tuple);
    if (fault_.has_value()) {
      return LazySeqVisitStatus::Faulted;
    }
    if (!nested.has_value()) {
      set_fault(frame, "TypeError", "flat_map block must return sequence");
      return LazySeqVisitStatus::Faulted;
    }
    for (const Value &nested_item : *nested) {
      const LazySeqVisitStatus status = emit_lazy_seq_value(
          frame, state, receiver, nested_item, op_index + 1U, visitor);
      if (status != LazySeqVisitStatus::Continue) {
        return status;
      }
    }
    return LazySeqVisitStatus::Continue;
  }

  LazySeqVisitStatus visit_lazy_seq(
      const Frame &frame, const LazySeqState &state, const Value &receiver,
      const std::function<LazySeqVisitStatus(const Value &)> &visitor) {
    if (value_is_range_instance(state.source)) {
      const std::optional<RangeBounds> bounds =
          extract_range_bounds(frame, state.source);
      if (fault_.has_value() || !bounds.has_value()) {
        return LazySeqVisitStatus::Faulted;
      }
      if (!bounds->start.has_value()) {
        set_fault(frame, "ArgumentError",
                  "beginless Range cannot be iterated lazily");
        return LazySeqVisitStatus::Faulted;
      }
      std::int64_t current = *bounds->start;
      while (true) {
        if (bounds->finish.has_value()) {
          if (bounds->inclusive_end) {
            if (current > *bounds->finish) {
              return LazySeqVisitStatus::Continue;
            }
          } else if (current >= *bounds->finish) {
            return LazySeqVisitStatus::Continue;
          }
        }
        const LazySeqVisitStatus status = emit_lazy_seq_value(
            frame, state, receiver, Value::integer(current), 0U, visitor);
        if (status != LazySeqVisitStatus::Continue) {
          return status;
        }
        if (current == std::numeric_limits<std::int64_t>::max()) {
          if (!bounds->finish.has_value()) {
            set_fault(frame, "ArgumentError",
                      "open-ended Range lazy iteration overflows Integer");
            return LazySeqVisitStatus::Faulted;
          }
          return LazySeqVisitStatus::Continue;
        }
        ++current;
      }
    }

    bool source_was_tuple = false;
    const std::optional<std::vector<Value>> source_items =
        extract_sequence_items(frame, state.source, &source_was_tuple);
    if (fault_.has_value()) {
      return LazySeqVisitStatus::Faulted;
    }
    if (!source_items.has_value()) {
      set_fault(frame, "TypeError", "LazySeq source must be a sequence");
      return LazySeqVisitStatus::Faulted;
    }
    for (const Value &item : *source_items) {
      const LazySeqVisitStatus status =
          emit_lazy_seq_value(frame, state, receiver, item, 0U, visitor);
      if (status != LazySeqVisitStatus::Continue) {
        return status;
      }
    }
    return LazySeqVisitStatus::Continue;
  }

  bool lazy_seq_source_is_open_ended_range(const Frame &frame,
                                           const LazySeqState &state,
                                           bool *out) {
    *out = false;
    if (!value_is_range_instance(state.source)) {
      return true;
    }
    const std::optional<RangeBounds> bounds =
        extract_range_bounds(frame, state.source);
    if (fault_.has_value() || !bounds.has_value()) {
      return false;
    }
    *out = !bounds->start.has_value() || !bounds->finish.has_value();
    return true;
  }

  bool require_lazy_seq_finite_source(const Frame &frame,
                                      const LazySeqState &state,
                                      const std::string &operation) {
    bool open_ended = false;
    if (!lazy_seq_source_is_open_ended_range(frame, state, &open_ended)) {
      return false;
    }
    if (open_ended) {
      set_fault(frame, "ArgumentError",
                "open-ended LazySeq cannot " + operation);
      return false;
    }
    return true;
  }

  std::optional<std::vector<Value>>
  materialize_lazy_seq_items(const Frame &frame, const LazySeqState &state,
                             const Value &receiver,
                             std::optional<std::size_t> limit) {
    if (limit.has_value() && *limit == 0U) {
      return std::vector<Value>{};
    }
    if (!limit.has_value() && !require_lazy_seq_finite_source(
                                  frame, state, "materialize all items")) {
      return std::nullopt;
    }
    std::vector<Value> items;
    const LazySeqVisitStatus status = visit_lazy_seq(
        frame, state, receiver, [&](const Value &item) -> LazySeqVisitStatus {
          items.push_back(item);
          if (limit.has_value() && items.size() >= *limit) {
            return LazySeqVisitStatus::Stop;
          }
          return LazySeqVisitStatus::Continue;
        });
    if (status == LazySeqVisitStatus::Faulted) {
      return std::nullopt;
    }
    return items;
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

  std::uint32_t intern_runtime_string(const std::string &text) {
    for (std::uint32_t i = 0; i < module_.strings.size(); ++i) {
      if (module_.strings[i] == text) {
        return i;
      }
    }
    module_.strings.push_back(text);
    return static_cast<std::uint32_t>(module_.strings.size() - 1U);
  }

  std::optional<std::uint32_t> symbol_id_for_text(const std::string &text) {
    for (std::uint32_t i = 0; i < module_.symbols.size(); ++i) {
      if (module_.symbols[i] == text) {
        return i;
      }
    }
    return std::nullopt;
  }

  std::uint32_t intern_runtime_symbol(const std::string &text) {
    if (const std::optional<std::uint32_t> existing =
            symbol_id_for_text(text)) {
      return *existing;
    }
    module_.symbols.push_back(text);
    return static_cast<std::uint32_t>(module_.symbols.size() - 1U);
  }

  struct ConversionResult {
    bool ok = false;
    Value value = Value::null();
    std::string error_name = "TypeError";
    std::string message;
  };

  static bool is_conversion_type(RuntimeNativeTypeKind kind) {
    switch (kind) {
    case RuntimeNativeTypeKind::Str:
    case RuntimeNativeTypeKind::Int:
    case RuntimeNativeTypeKind::Float:
    case RuntimeNativeTypeKind::Bool:
    case RuntimeNativeTypeKind::Symbol:
    case RuntimeNativeTypeKind::Array:
    case RuntimeNativeTypeKind::Tuple:
    case RuntimeNativeTypeKind::Set:
    case RuntimeNativeTypeKind::Map:
    case RuntimeNativeTypeKind::Null:
    case RuntimeNativeTypeKind::Object:
      return true;
    default:
      return false;
    }
  }

  static std::optional<RuntimeNativeTypeKind>
  conversion_target_for_alias(const std::string &name) {
    if (name == "str") {
      return RuntimeNativeTypeKind::Str;
    }
    if (name == "int") {
      return RuntimeNativeTypeKind::Int;
    }
    if (name == "float") {
      return RuntimeNativeTypeKind::Float;
    }
    if (name == "bool") {
      return RuntimeNativeTypeKind::Bool;
    }
    if (name == "symbol") {
      return RuntimeNativeTypeKind::Symbol;
    }
    if (name == "array") {
      return RuntimeNativeTypeKind::Array;
    }
    if (name == "tuple") {
      return RuntimeNativeTypeKind::Tuple;
    }
    if (name == "set") {
      return RuntimeNativeTypeKind::Set;
    }
    if (name == "map") {
      return RuntimeNativeTypeKind::Map;
    }
    return std::nullopt;
  }

  static std::optional<RuntimeNativeTypeKind>
  conversion_target_for_to_method(const std::string &selector) {
    constexpr std::string_view prefix = "to_";
    if (selector.size() <= prefix.size() ||
        selector.compare(0, prefix.size(), prefix) != 0) {
      return std::nullopt;
    }
    return conversion_target_for_alias(selector.substr(prefix.size()));
  }

  std::optional<RuntimeNativeTypeKind>
  conversion_type_from_value(const Value &target) const {
    if (target.is_native_type()) {
      const RuntimeNativeTypeKind kind = target.as_native_type().kind;
      if (is_conversion_type(kind)) {
        return kind;
      }
    }
    return std::nullopt;
  }

  ConversionResult conversion_ok(Value value) {
    ConversionResult result;
    result.ok = true;
    result.value = std::move(value);
    return result;
  }

  ConversionResult conversion_error(std::string error_name,
                                    std::string message) {
    ConversionResult result;
    result.ok = false;
    result.error_name = std::move(error_name);
    result.message = std::move(message);
    return result;
  }

  bool parse_integer_text(const std::string &text, std::int64_t *out) {
    if (text.empty()) {
      return false;
    }
    const char *begin = text.data();
    const char *end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, *out, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end;
  }

  bool parse_float_text(const std::string &text, double *out) {
    if (text.empty()) {
      return false;
    }
    for (unsigned char c : text) {
      if (std::isspace(c) != 0) {
        return false;
      }
    }
    try {
      std::size_t consumed = 0;
      const double parsed = std::stod(text, &consumed);
      if (consumed != text.size()) {
        return false;
      }
      *out = parsed;
      return true;
    } catch (const std::invalid_argument &) {
      return false;
    } catch (const std::out_of_range &) {
      return false;
    }
  }

  std::string display_float_text(double value) {
    char buffer[128];
    const auto converted =
        std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (converted.ec == std::errc{}) {
      return std::string(buffer, converted.ptr);
    }
    std::ostringstream out;
    out << value;
    return out.str();
  }

  ConversionResult display_string_value(const Value &value) {
    if (value.is_string()) {
      const std::optional<std::string> text =
          string_text_from_id(value.as_string().string_id);
      if (!text.has_value()) {
        return conversion_error("VMError", "string ref is invalid");
      }
      return conversion_ok(value);
    }
    if (value.is_symbol()) {
      const std::optional<std::string> text =
          selector_text_from_symbol(value.as_symbol().symbol_id);
      if (!text.has_value()) {
        return conversion_error("VMError", "symbol ref is invalid");
      }
      return conversion_ok(Value::string(intern_runtime_string(*text)));
    }
    if (value.is_float()) {
      return conversion_ok(Value::string(
          intern_runtime_string(display_float_text(value.as_float()))));
    }
    if (value.is_native_type()) {
      return conversion_ok(Value::string(intern_runtime_string(
          std::string("<type ") +
          native_type_name(value.as_native_type().kind) + ">")));
    }
    return conversion_ok(
        Value::string(intern_runtime_string(runtime_stringify_value(
            value, RuntimeStringifyMode::Display, &module_))));
  }

  ConversionResult convert_value_to_native_type(const Frame &frame,
                                                const Value &value,
                                                RuntimeNativeTypeKind target) {
    (void)frame;
    switch (target) {
    case RuntimeNativeTypeKind::Str:
      return display_string_value(value);
    case RuntimeNativeTypeKind::Int: {
      if (value.is_integer()) {
        return conversion_ok(value);
      }
      if (value.is_float()) {
        const double raw = value.as_float();
        const auto as_int = static_cast<std::int64_t>(raw);
        if (!std::isfinite(raw) || static_cast<double>(as_int) != raw) {
          return conversion_error("ValueError",
                                  "Float cannot be exactly cast to Int");
        }
        return conversion_ok(Value::integer(as_int));
      }
      if (value.is_string()) {
        const std::optional<std::string> text =
            string_text_from_id(value.as_string().string_id);
        std::int64_t parsed = 0;
        if (!text.has_value()) {
          return conversion_error("VMError", "string ref is invalid");
        }
        if (!parse_integer_text(*text, &parsed)) {
          return conversion_error("ValueError", "String content is not an Int");
        }
        return conversion_ok(Value::integer(parsed));
      }
      return conversion_error("TypeError", "cannot cast value to Int");
    }
    case RuntimeNativeTypeKind::Float: {
      if (value.is_float()) {
        return conversion_ok(value);
      }
      if (value.is_integer()) {
        return conversion_ok(
            Value::floating(static_cast<double>(value.as_integer())));
      }
      if (value.is_string()) {
        const std::optional<std::string> text =
            string_text_from_id(value.as_string().string_id);
        double parsed = 0.0;
        if (!text.has_value()) {
          return conversion_error("VMError", "string ref is invalid");
        }
        if (!parse_float_text(*text, &parsed)) {
          return conversion_error("ValueError",
                                  "String content is not a Float");
        }
        return conversion_ok(Value::floating(parsed));
      }
      return conversion_error("TypeError", "cannot cast value to Float");
    }
    case RuntimeNativeTypeKind::Bool: {
      if (value.is_bool()) {
        return conversion_ok(value);
      }
      if (value.is_string()) {
        const std::optional<std::string> text =
            string_text_from_id(value.as_string().string_id);
        if (!text.has_value()) {
          return conversion_error("VMError", "string ref is invalid");
        }
        if (*text == "true") {
          return conversion_ok(Value::boolean(true));
        }
        if (*text == "false") {
          return conversion_ok(Value::boolean(false));
        }
        return conversion_error("ValueError", "String content is not a Bool");
      }
      return conversion_error("TypeError", "cannot cast value to Bool");
    }
    case RuntimeNativeTypeKind::Symbol: {
      if (value.is_symbol()) {
        return conversion_ok(value);
      }
      if (value.is_string()) {
        const std::optional<std::string> text =
            string_text_from_id(value.as_string().string_id);
        if (!text.has_value()) {
          return conversion_error("VMError", "string ref is invalid");
        }
        if (text->empty()) {
          return conversion_error("ValueError",
                                  "String content is not a Symbol");
        }
        return conversion_ok(Value::symbol(intern_runtime_symbol(*text)));
      }
      return conversion_error("TypeError", "cannot cast value to Symbol");
    }
    case RuntimeNativeTypeKind::Array: {
      if (value.is_list()) {
        return conversion_ok(value);
      }
      if (value.is_tuple()) {
        const std::shared_ptr<TupleValue> tuple = value.as_tuple();
        if (tuple == nullptr) {
          return conversion_error("TypeError", "tuple value is null");
        }
        return conversion_ok(make_list_value(tuple->items));
      }
      if (value.is_set()) {
        const std::shared_ptr<SetValue> set = value.as_set();
        if (set == nullptr) {
          return conversion_error("TypeError", "set value is null");
        }
        return conversion_ok(make_list_value(set->items));
      }
      return conversion_error("TypeError", "cannot cast value to Array");
    }
    case RuntimeNativeTypeKind::Tuple: {
      if (value.is_tuple()) {
        return conversion_ok(value);
      }
      if (value.is_list()) {
        const std::shared_ptr<ListValue> list = value.as_list();
        if (list == nullptr) {
          return conversion_error("TypeError", "list value is null");
        }
        return conversion_ok(make_tuple_value(list->items));
      }
      if (value.is_set()) {
        const std::shared_ptr<SetValue> set = value.as_set();
        if (set == nullptr) {
          return conversion_error("TypeError", "set value is null");
        }
        return conversion_ok(make_tuple_value(set->items));
      }
      return conversion_error("TypeError", "cannot cast value to Tuple");
    }
    case RuntimeNativeTypeKind::Set: {
      if (value.is_set()) {
        return conversion_ok(value);
      }
      if (value.is_list()) {
        const std::shared_ptr<ListValue> list = value.as_list();
        if (list == nullptr) {
          return conversion_error("TypeError", "list value is null");
        }
        return conversion_ok(make_set_value(list->items));
      }
      if (value.is_tuple()) {
        const std::shared_ptr<TupleValue> tuple = value.as_tuple();
        if (tuple == nullptr) {
          return conversion_error("TypeError", "tuple value is null");
        }
        return conversion_ok(make_set_value(tuple->items));
      }
      return conversion_error("TypeError", "cannot cast value to Set");
    }
    case RuntimeNativeTypeKind::Map: {
      if (value.is_map()) {
        return conversion_ok(value);
      }
      std::vector<Value> items;
      if (value.is_list()) {
        const std::shared_ptr<ListValue> list = value.as_list();
        if (list == nullptr) {
          return conversion_error("TypeError", "list value is null");
        }
        items = list->items;
      } else if (value.is_tuple()) {
        const std::shared_ptr<TupleValue> tuple = value.as_tuple();
        if (tuple == nullptr) {
          return conversion_error("TypeError", "tuple value is null");
        }
        items = tuple->items;
      } else {
        return conversion_error("TypeError", "cannot cast value to Map");
      }
      std::vector<MapEntry> entries;
      entries.reserve(items.size());
      for (const Value &item : items) {
        std::vector<Value> pair;
        if (item.is_tuple()) {
          const std::shared_ptr<TupleValue> tuple = item.as_tuple();
          if (tuple == nullptr) {
            return conversion_error("TypeError", "map pair tuple is null");
          }
          pair = tuple->items;
        } else if (item.is_list()) {
          const std::shared_ptr<ListValue> list = item.as_list();
          if (list == nullptr) {
            return conversion_error("TypeError", "map pair list is null");
          }
          pair = list->items;
        } else {
          return conversion_error("TypeError",
                                  "Map cast expects key/value pairs");
        }
        if (pair.size() != 2U) {
          return conversion_error("TypeError",
                                  "Map cast pair must have two values");
        }
        std::uint32_t key_symbol_id = 0;
        if (pair[0].is_symbol()) {
          key_symbol_id = pair[0].as_symbol().symbol_id;
        } else if (pair[0].is_string()) {
          const std::optional<std::string> text =
              string_text_from_id(pair[0].as_string().string_id);
          if (!text.has_value()) {
            return conversion_error("VMError", "string key ref is invalid");
          }
          key_symbol_id = intern_runtime_symbol(*text);
        } else {
          return conversion_error("TypeError",
                                  "Map cast key must be Symbol or String");
        }
        entries.push_back({key_symbol_id, pair[1]});
      }
      return conversion_ok(make_symbol_map_value(std::move(entries)));
    }
    case RuntimeNativeTypeKind::Null:
      if (value.is_null()) {
        return conversion_ok(value);
      }
      return conversion_error("TypeError", "cannot cast value to Null");
    case RuntimeNativeTypeKind::Object:
      return conversion_ok(value);
    default:
      return conversion_error("TypeError", "target is not a conversion type");
    }
  }

  bool conversion_error_is_nullable(const ConversionResult &result) const {
    return result.error_name == "TypeError" ||
           result.error_name == "ValueError" ||
           result.error_name == "EncodingError";
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

  std::uint32_t call_site_flags(const Frame &frame,
                                std::uint32_t site_id) const {
    if (frame.code == nullptr ||
        site_id >= frame.code->call_site_table.size()) {
      return 0;
    }
    return frame.code->call_site_table[site_id].flags;
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
    const Value old_value = instance_ivar_value_or_null(instance, name);
    if (!store_instance_ivar_slow(frame, instance, name, std::move(value))) {
      return false;
    }
    const auto slot = instance->header.shape->ivar_slots.find(name);
    if (slot == instance->header.shape->ivar_slots.end()) {
      set_fault(frame, "VMError", "stored ivar slot is missing");
      return false;
    }
    update_ivar_cache(frame, site_id, *instance, symbol_id, slot->second);
    record_watch_ivar_write(instance, name, old_value,
                            instance_ivar_value_or_null(instance, name));
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

  bool pattern_triple_eq(Frame &frame, const Value &matcher, const Value &value,
                         bool *out) {
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
      const Value old_value = instance_ivar_value_or_null(instance, ivar_name);
      if (!store_instance_ivar_slow(frame, instance, ivar_name, value)) {
        return false;
      }
      record_watch_ivar_write(instance, ivar_name, old_value,
                              instance_ivar_value_or_null(instance, ivar_name));
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
        sync_integer_reg_from_value(frame, static_cast<std::uint32_t>(slot),
                                    frame.regs[slot]);
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
        sync_integer_reg_from_value(frame, static_cast<std::uint32_t>(slot),
                                    frame.regs[slot]);
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
      if (nested_frame.initialized[i] != 0U) {
        sync_integer_reg_from_value(nested_frame, static_cast<std::uint32_t>(i),
                                    nested_frame.regs[i]);
      } else {
        invalidate_integer_reg(nested_frame, static_cast<std::uint32_t>(i));
      }
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
    initialize_frame_register_file(callee, *code);
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
    const std::vector<std::pair<std::uint32_t, Value>> no_keywords;
    const SendStatus scalar_status = try_apply_scalar_send(
        frame, receiver, selector, args, Value::null(), no_keywords, out);
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

  bool invoke_callable_value(
      Frame &frame, const Value &callee, const std::vector<Value> &pos_args,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      const Value &block, std::uint32_t dst) {
    if (callee.is_closure()) {
      if (!kw_args.empty()) {
        set_fault(frame, "TypeError",
                  "closure CALL does not accept keyword arguments");
        return false;
      }
      if (!ensure_lifecycle_access(frame, callee)) {
        return false;
      }
      const std::shared_ptr<ClosureValue> closure = callee.as_closure();
      if (closure == nullptr) {
        set_fault(frame, "TypeError", "closure value is null");
        return false;
      }
      const BcCode *code = find_code(module_, closure->code_id);
      if (code == nullptr) {
        set_fault(frame, "VMError", "closure code id is unknown");
        return false;
      }
      const std::uint32_t call_pc = static_cast<std::uint32_t>(frame.pc);
      ++frame.pc;
      frame.active_call_pc = call_pc;
      push_frame(*code, pos_args, closure->captures, closure->self, block, dst);
      return true;
    }

    if (callee.is_native_function()) {
      Value result = Value::null();
      const SendStatus status =
          apply_kernel_output_helper(frame, callee.as_native_function().kind,
                                     pos_args, block, kw_args, &result);
      if (status == SendStatus::Faulted) {
        return false;
      }
      if (status == SendStatus::Matched) {
        if (!write_reg(frame, dst, std::move(result))) {
          return false;
        }
        ++frame.pc;
        return true;
      }
    }

    if (callee.is_class_object()) {
      const std::uint32_t class_index = callee.as_class_object().class_index;
      auto instance = make_instance_value(class_index);
      if (!ensure_instance_layout(frame, instance)) {
        return false;
      }
      const Value instance_value = Value::instance(instance);
      const bytecode::BcMethod *init = find_method_for_dispatch(
          frame, class_index, "init", kMethodFlagInstance);
      if (fault_.has_value()) {
        return false;
      }
      if (init == nullptr) {
        if (!pos_args.empty()) {
          set_fault(frame, "TypeError",
                    "class call without init accepts no positional arguments");
          return false;
        }
        if (!kw_args.empty()) {
          set_fault(
              frame, "TypeError",
              "class call without init does not accept keyword arguments");
          return false;
        }
        if (!block.is_null()) {
          set_fault(frame, "TypeError",
                    "class call without init does not accept block");
          return false;
        }
        if (!write_reg(frame, dst, instance_value)) {
          return false;
        }
        ++frame.pc;
        return true;
      }
      return invoke_method(frame, *init, pos_args, kw_args, instance_value,
                           block, dst, instance_value);
    }

    if (callee.is_instance_object()) {
      const std::shared_ptr<InstanceValue> instance =
          callee.as_instance_object();
      if (instance == nullptr) {
        set_fault(frame, "TypeError", "instance callee is null");
        return false;
      }
      const bytecode::BcMethod *method = find_method_for_dispatch(
          frame, instance->class_index, "call", kMethodFlagInstance);
      if (fault_.has_value()) {
        return false;
      }
      if (method == nullptr) {
        set_fault(frame, "TypeError",
                  "CALL expects closure, class, or object with call method");
        return false;
      }
      return invoke_method(frame, *method, pos_args, kw_args, callee, block,
                           dst);
    }

    set_fault(frame, "TypeError",
              "CALL expects closure, class, or object with call method");
    return false;
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
    initialize_frame_register_file(callee, entry_code);
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
        (method_flags & kMethodFlagClass) != 0U
            ? runtime_owner.class_method_table
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
        frame, class_index, selector, (expected_flags & kMethodFlagClass) != 0U,
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

  using NativeBlockInvoker =
      std::function<Value(const std::vector<Value> &args)>;

  std::optional<NativeBlockInvoker>
  make_native_block_invoker(const Frame &frame, const Value &block,
                            const std::string &context) {
    if (block.is_null()) {
      set_fault(frame, "TypeError", context + " requires block");
      return std::nullopt;
    }
    if (!block.is_closure()) {
      set_fault(frame, "TypeError", context + " block must be closure");
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

    BcModule module_copy = module_;
    std::shared_ptr<RuntimeState> runtime_state = state_;
    std::string module_id = module_id_;
    const std::uint32_t code_id = closure->code_id;
    std::vector<Value> captures = closure->captures;
    Value self = closure->self;
    return [module_copy = std::move(module_copy),
            runtime_state = std::move(runtime_state),
            module_id = std::move(module_id), code_id,
            captures = std::move(captures),
            self = std::move(self)](const std::vector<Value> &args) mutable {
      const BcCode *code = find_code(module_copy, code_id);
      if (code == nullptr) {
        throw RuntimeTaskFailure("VMError", "closure code id is unknown");
      }
      Vm nested(module_copy, runtime_state, module_id);
      nested.push_frame(*code, args, captures, self, Value::null(),
                        std::nullopt);
      while (nested.fault_ == std::nullopt && !nested.frames_.empty()) {
        nested.step();
      }
      if (nested.fault_.has_value()) {
        throw RuntimeTaskFailure(nested.fault_->error_name,
                                 nested.fault_->message);
      }
      return nested.final_value_;
    };
  }

  std::optional<Value>
  keyword_arg_value(const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
                    const std::string &name) {
    for (const auto &[symbol_id, value] : kw_args) {
      const std::optional<std::string> key =
          selector_text_from_symbol(symbol_id);
      if (key.has_value() && *key == name) {
        return value;
      }
    }
    return std::nullopt;
  }

  bool reject_unknown_keywords(
      const Frame &frame,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      std::initializer_list<const char *> allowed) {
    for (const auto &[symbol_id, value] : kw_args) {
      (void)value;
      const std::optional<std::string> key =
          selector_text_from_symbol(symbol_id);
      bool matched = false;
      if (key.has_value()) {
        for (const char *candidate : allowed) {
          if (*key == candidate) {
            matched = true;
            break;
          }
        }
      }
      if (!matched) {
        set_fault(frame, "TypeError", "unknown keyword argument");
        return false;
      }
    }
    return true;
  }

  bool duration_from_value(const Frame &frame, const Value &value,
                           std::chrono::milliseconds *out) {
    if (value.is_integer()) {
      *out = std::chrono::milliseconds(value.as_integer());
      return true;
    }
    if (value.is_float()) {
      *out = std::chrono::milliseconds(
          static_cast<std::int64_t>(value.as_float() * 1000.0));
      return true;
    }
    set_fault(frame, "TypeError",
              "duration must be Integer milliseconds or Float seconds");
    return false;
  }

  bool timeout_from_keywords(
      const Frame &frame,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      std::chrono::milliseconds *out) {
    *out = std::chrono::milliseconds::max();
    std::optional<Value> timeout = keyword_arg_value(kw_args, "timeout");
    if (!timeout.has_value()) {
      return true;
    }
    return duration_from_value(frame, *timeout, out);
  }

  bool capacity_from_args(
      const Frame &frame, const std::vector<Value> &args,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      std::size_t *out) {
    *out = 0;
    if (!args.empty()) {
      if (args.size() != 1 || !args[0].is_integer() ||
          args[0].as_integer() < 0) {
        set_fault(frame, "TypeError",
                  "capacity must be a non-negative Integer");
        return false;
      }
      *out = static_cast<std::size_t>(args[0].as_integer());
    }
    std::optional<Value> capacity = keyword_arg_value(kw_args, "capacity");
    if (capacity.has_value()) {
      if (!capacity->is_integer() || capacity->as_integer() < 0) {
        set_fault(frame, "TypeError",
                  "capacity must be a non-negative Integer");
        return false;
      }
      *out = static_cast<std::size_t>(capacity->as_integer());
    }
    return true;
  }

  bool workers_from_optional_arg(const Frame &frame,
                                 const std::vector<Value> &args,
                                 std::size_t *out) {
    *out = 0;
    if (args.empty()) {
      return true;
    }
    if (args.size() != 1 || !args[0].is_integer() || args[0].as_integer() < 0) {
      set_fault(frame, "TypeError",
                "worker count must be a non-negative Integer");
      return false;
    }
    *out = static_cast<std::size_t>(args[0].as_integer());
    return true;
  }

  static std::vector<Value>
  native_sequence_items_or_throw(const Value &value,
                                 const std::string &context) {
    if (value.is_list()) {
      const std::shared_ptr<ListValue> list = value.as_list();
      if (list == nullptr) {
        throw RuntimeTaskFailure("TypeError", context + " list value is null");
      }
      return list->items;
    }
    if (value.is_tuple()) {
      const std::shared_ptr<TupleValue> tuple = value.as_tuple();
      if (tuple == nullptr) {
        throw RuntimeTaskFailure("TypeError", context + " tuple value is null");
      }
      return tuple->items;
    }
    if (value.is_set()) {
      const std::shared_ptr<SetValue> set = value.as_set();
      if (set == nullptr) {
        throw RuntimeTaskFailure("TypeError", context + " set value is null");
      }
      return set->items;
    }
    throw RuntimeTaskFailure("TypeError", context + " must return sequence");
  }

  bool set_fault_from_task_result(const Frame &frame,
                                  const RuntimeTaskPublicResult &result) {
    set_fault(
        frame, result.error_name.empty() ? "TaskError" : result.error_name,
        result.message.empty() ? "task operation failed" : result.message);
    return false;
  }

  bool set_fault_from_channel_result(const Frame &frame,
                                     const RuntimeChannelResult &result) {
    set_fault(
        frame, result.error_name.empty() ? "ChannelError" : result.error_name,
        result.message.empty() ? "channel operation failed" : result.message);
    return false;
  }

  bool set_fault_from_mutex_result(const Frame &frame,
                                   const RuntimeMutexResult &result) {
    set_fault(
        frame, result.error_name.empty() ? "RuntimeError" : result.error_name,
        result.message.empty() ? "mutex operation failed" : result.message);
    return false;
  }

  bool set_fault_from_atomic_result(const Frame &frame,
                                    const RuntimeAtomic::Result &result) {
    set_fault(
        frame, result.error_name.empty() ? "AtomicError" : result.error_name,
        result.message.empty() ? "atomic operation failed" : result.message);
    return false;
  }

  bool set_fault_from_barrier_result(const Frame &frame,
                                     const RuntimeBarrierResult &result) {
    set_fault(
        frame, result.error_name.empty() ? "RuntimeError" : result.error_name,
        result.message.empty() ? "barrier operation failed" : result.message);
    return false;
  }

  bool set_fault_from_flow_result(const Frame &frame,
                                  const RuntimeFlowGatherResult &result) {
    std::string error_name = result.error_name;
    std::string message = result.message;
    if (error_name.empty() && !result.failures.empty()) {
      error_name = result.failures.front().error_name;
      message = result.failures.front().message;
    }
    set_fault(frame, error_name.empty() ? "FlowError" : error_name,
              message.empty() ? "flow operation failed" : message);
    return false;
  }

  bool set_fault_from_text_write_result(const Frame &frame,
                                        const RuntimeTextWriteResult &result) {
    if (result.ok) {
      return true;
    }
    set_fault(frame, result.error_name.empty() ? "IOError" : result.error_name,
              result.message.empty() ? "text writer failed" : result.message);
    return false;
  }

  std::optional<std::shared_ptr<RuntimeTextWriter>>
  text_writer_from_value(const Frame &frame, const Value &value,
                         const std::string &context) {
    if (!value.is_text_writer()) {
      set_fault(frame, "TypeError", context + " expects text writer");
      return std::nullopt;
    }
    std::shared_ptr<RuntimeTextWriter> writer = value.as_text_writer();
    if (writer == nullptr) {
      set_fault(frame, "TypeError", context + " text writer is null");
      return std::nullopt;
    }
    return writer;
  }

  std::optional<std::string> text_from_symbol_or_string(const Value &value) {
    if (value.is_symbol()) {
      return selector_text_from_symbol(value.as_symbol().symbol_id);
    }
    if (value.is_string()) {
      return string_text_from_id(value.as_string().string_id);
    }
    return std::nullopt;
  }

  std::optional<RuntimeFlowPartitionPolicy>
  threaded_scatter_policy_from_value(const Frame &frame, const Value &value) {
    const std::optional<std::string> text = text_from_symbol_or_string(value);
    if (!text.has_value()) {
      set_fault(frame, "TypeError",
                "threaded scatter must be :atomic, :dynamic, :chunks, "
                ":fixed, :stride, or :items");
      return std::nullopt;
    }
    if (*text == "atomic" || *text == "shared" || *text == "dynamic") {
      return RuntimeFlowPartitionPolicy::Atomic;
    }
    if (*text == "chunks" || *text == "chunk" || *text == "fixed" ||
        *text == "fixed_chunks") {
      return RuntimeFlowPartitionPolicy::Chunks;
    }
    if (*text == "stride" || *text == "strided") {
      return RuntimeFlowPartitionPolicy::Stride;
    }
    if (*text == "items" || *text == "item" || *text == "per_item") {
      return RuntimeFlowPartitionPolicy::Items;
    }
    set_fault(frame, "TypeError",
              "threaded scatter must be :atomic, :dynamic, :chunks, "
              ":fixed, :stride, or :items");
    return std::nullopt;
  }

  bool threaded_scatter_policy_from_keywords(
      const Frame &frame,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      RuntimeFlowPartitionPolicy *out) {
    *out = RuntimeFlowPartitionPolicy::Atomic;
    if (!reject_unknown_keywords(frame, kw_args, {"scatter"})) {
      return false;
    }
    const std::optional<Value> scatter = keyword_arg_value(kw_args, "scatter");
    if (!scatter.has_value()) {
      return true;
    }
    const std::optional<RuntimeFlowPartitionPolicy> policy =
        threaded_scatter_policy_from_value(frame, *scatter);
    if (!policy.has_value()) {
      return false;
    }
    *out = *policy;
    return true;
  }

  std::optional<RuntimeLogLevel>
  log_level_from_value(const Frame &frame, const Value &value,
                       const std::string &context) {
    const std::optional<std::string> text = text_from_symbol_or_string(value);
    if (!text.has_value()) {
      set_fault(frame, "TypeError",
                context + " must be :fatal, :error, :warn, :info, or :debug");
      return std::nullopt;
    }
    if (*text == "fatal") {
      return RuntimeLogLevel::Fatal;
    }
    if (*text == "error") {
      return RuntimeLogLevel::Error;
    }
    if (*text == "warn" || *text == "warning") {
      return RuntimeLogLevel::Warn;
    }
    if (*text == "info") {
      return RuntimeLogLevel::Info;
    }
    if (*text == "debug") {
      return RuntimeLogLevel::Debug;
    }
    set_fault(frame, "TypeError",
              context + " must be :fatal, :error, :warn, :info, or :debug");
    return std::nullopt;
  }

  std::optional<RuntimeLogColorMode>
  log_color_mode_from_value(const Frame &frame, const Value &value,
                            const std::string &context) {
    if (value.is_bool()) {
      return value.as_bool() ? RuntimeLogColorMode::Always
                             : RuntimeLogColorMode::Never;
    }
    const std::optional<std::string> text = text_from_symbol_or_string(value);
    if (!text.has_value()) {
      set_fault(frame, "TypeError",
                context + " must be Bool, :auto, :always, or :never");
      return std::nullopt;
    }
    if (*text == "auto") {
      return RuntimeLogColorMode::Auto;
    }
    if (*text == "always") {
      return RuntimeLogColorMode::Always;
    }
    if (*text == "never") {
      return RuntimeLogColorMode::Never;
    }
    set_fault(frame, "TypeError",
              context + " must be Bool, :auto, :always, or :never");
    return std::nullopt;
  }

  bool size_keyword_value(const Frame &frame, const Value &value,
                          const std::string &name, std::size_t *out) {
    if (!value.is_integer() || value.as_integer() < 0) {
      set_fault(frame, "TypeError", name + " must be non-negative Integer");
      return false;
    }
    *out = static_cast<std::size_t>(value.as_integer());
    return true;
  }

  bool apply_pretty_options(
      const Frame &frame,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      RuntimePrettyPrintOptions *options) {
    if (const std::optional<Value> max_width =
            keyword_arg_value(kw_args, "max_width")) {
      if (!size_keyword_value(frame, *max_width, "max_width",
                              &options->max_width)) {
        return false;
      }
    }
    if (const std::optional<Value> max_depth =
            keyword_arg_value(kw_args, "max_depth")) {
      if (!size_keyword_value(frame, *max_depth, "max_depth",
                              &options->max_depth)) {
        return false;
      }
    }
    if (const std::optional<Value> max_items =
            keyword_arg_value(kw_args, "max_items")) {
      if (!size_keyword_value(frame, *max_items, "max_items",
                              &options->max_items)) {
        return false;
      }
    }
    return true;
  }

  std::optional<RuntimeStringifyMode>
  stringify_mode_from_value(const Frame &frame, const Value &mode) {
    std::optional<std::string> mode_text;
    if (mode.is_symbol()) {
      mode_text = selector_text_from_symbol(mode.as_symbol().symbol_id);
    } else if (mode.is_string()) {
      mode_text = string_text_from_id(mode.as_string().string_id);
    }
    if (!mode_text.has_value()) {
      set_fault(frame, "TypeError",
                "Amber.stringify mode must be :display, :inspect, or :pretty");
      return std::nullopt;
    }
    if (*mode_text == "display") {
      return RuntimeStringifyMode::Display;
    }
    if (*mode_text == "inspect") {
      return RuntimeStringifyMode::Inspect;
    }
    if (*mode_text == "pretty") {
      return RuntimeStringifyMode::Pretty;
    }
    set_fault(frame, "TypeError",
              "Amber.stringify mode must be :display, :inspect, or :pretty");
    return std::nullopt;
  }

  Value string_value_from_text(std::string text) {
    return Value::string(intern_runtime_string(text));
  }

  SendStatus apply_kernel_output_helper(
      const Frame &frame, RuntimeNativeFunctionKind kind,
      const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args, Value *out) {
    if (!block.is_null()) {
      set_fault(frame, "TypeError", "output helper does not accept block");
      return SendStatus::Faulted;
    }
    const bool pretty = kind == RuntimeNativeFunctionKind::Pp;
    if (!reject_unknown_keywords(
            frame, kw_args,
            pretty
                ? std::initializer_list<const char *>{"to", "max_width",
                                                      "max_depth", "max_items"}
                : std::initializer_list<const char *>{"to"})) {
      return SendStatus::Faulted;
    }

    std::shared_ptr<RuntimeTextWriter> writer = current_runtime_stdout();
    if (const std::optional<Value> to = keyword_arg_value(kw_args, "to")) {
      const std::optional<std::shared_ptr<RuntimeTextWriter>> explicit_writer =
          text_writer_from_value(frame, *to, "to:");
      if (!explicit_writer.has_value()) {
        return SendStatus::Faulted;
      }
      writer = *explicit_writer;
    }

    RuntimePrettyPrintOptions options;
    if (pretty && !apply_pretty_options(frame, kw_args, &options)) {
      return SendStatus::Faulted;
    }

    RuntimeStringifyMode mode = RuntimeStringifyMode::Display;
    if (kind == RuntimeNativeFunctionKind::P) {
      mode = RuntimeStringifyMode::Inspect;
    } else if (kind == RuntimeNativeFunctionKind::Pp) {
      mode = RuntimeStringifyMode::Pretty;
    }

    if (kind == RuntimeNativeFunctionKind::Print && args.empty()) {
      if (!set_fault_from_text_write_result(frame, writer->write_str("\n"))) {
        return SendStatus::Faulted;
      }
    } else {
      for (const Value &arg : args) {
        const std::string text = runtime_stringify_value(
            arg, mode, &module_, nullptr, nullptr, options);
        if (!set_fault_from_text_write_result(frame, writer->write_str(text)) ||
            !set_fault_from_text_write_result(frame, writer->write_str("\n"))) {
          return SendStatus::Faulted;
        }
      }
    }

    if (kind == RuntimeNativeFunctionKind::Print || args.empty()) {
      *out = Value::null();
    } else if (args.size() == 1U) {
      *out = args.front();
    } else {
      *out = make_tuple_value(args);
    }
    return SendStatus::Matched;
  }

  SendStatus try_apply_native_stdlib_send(
      const Frame &frame, const Value &receiver, const std::string &selector,
      const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args, Value *out) {
    auto require_arity = [&](std::size_t expected) -> bool {
      if (args.size() != expected) {
        set_fault(frame, "TypeError", "wrong native stdlib SEND arity");
        return false;
      }
      return true;
    };
    auto require_no_block = [&]() -> bool {
      if (!block.is_null()) {
        set_fault(frame, "TypeError",
                  "native stdlib selector does not accept block arguments");
        return false;
      }
      return true;
    };

    if (receiver.is_native_type()) {
      const RuntimeNativeTypeKind kind = receiver.as_native_type().kind;
      if (kind == RuntimeNativeTypeKind::Amber) {
        if (selector != "stringify") {
          return SendStatus::NotHandled;
        }
        if (!require_arity(1) ||
            !reject_unknown_keywords(
                frame, kw_args,
                {"mode", "max_width", "max_depth", "max_items"}) ||
            !require_no_block()) {
          return SendStatus::Faulted;
        }
        RuntimeStringifyMode stringify_mode = RuntimeStringifyMode::Display;
        const std::optional<Value> mode_arg =
            keyword_arg_value(kw_args, "mode");
        if (mode_arg.has_value()) {
          const std::optional<RuntimeStringifyMode> parsed =
              stringify_mode_from_value(frame, *mode_arg);
          if (!parsed.has_value()) {
            return SendStatus::Faulted;
          }
          stringify_mode = *parsed;
        }
        RuntimePrettyPrintOptions options;
        if (!apply_pretty_options(frame, kw_args, &options)) {
          return SendStatus::Faulted;
        }
        *out = string_value_from_text(runtime_stringify_value(
            args[0], stringify_mode, &module_, nullptr, nullptr, options));
        return SendStatus::Matched;
      }
      if (kind == RuntimeNativeTypeKind::Kernel) {
        if (selector == "print") {
          return apply_kernel_output_helper(frame,
                                            RuntimeNativeFunctionKind::Print,
                                            args, block, kw_args, out);
        }
        if (selector == "p") {
          return apply_kernel_output_helper(frame, RuntimeNativeFunctionKind::P,
                                            args, block, kw_args, out);
        }
        if (selector == "pp") {
          return apply_kernel_output_helper(
              frame, RuntimeNativeFunctionKind::Pp, args, block, kw_args, out);
        }
        return SendStatus::NotHandled;
      }
      if (kind == RuntimeNativeTypeKind::Io) {
        if (selector == "Buffer") {
          if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
            if (!kw_args.empty()) {
              set_fault(frame, "TypeError",
                        "io.Buffer does not accept keywords");
            }
            return SendStatus::Faulted;
          }
          *out = Value::native_type(RuntimeNativeTypeKind::TextBuffer);
          return SendStatus::Matched;
        }
        if (selector == "Logger") {
          if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
            if (!kw_args.empty()) {
              set_fault(frame, "TypeError",
                        "io.Logger does not accept keywords");
            }
            return SendStatus::Faulted;
          }
          *out = Value::native_type(RuntimeNativeTypeKind::Logger);
          return SendStatus::Matched;
        }
        if (selector == "current_stdout" || selector == "stdout") {
          if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
            if (!kw_args.empty()) {
              set_fault(frame, "TypeError",
                        "io.stdout does not accept keywords");
            }
            return SendStatus::Faulted;
          }
          *out = Value::text_writer(current_runtime_stdout());
          return SendStatus::Matched;
        }
        if (selector == "current_stderr" || selector == "stderr") {
          if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
            if (!kw_args.empty()) {
              set_fault(frame, "TypeError",
                        "io.stderr does not accept keywords");
            }
            return SendStatus::Faulted;
          }
          *out = Value::text_writer(current_runtime_stderr());
          return SendStatus::Matched;
        }
        if (selector == "with_output") {
          if (!require_arity(0) ||
              !reject_unknown_keywords(frame, kw_args, {"stdout", "stderr"})) {
            return SendStatus::Faulted;
          }
          std::shared_ptr<RuntimeTextWriter> stdout_writer;
          std::shared_ptr<RuntimeTextWriter> stderr_writer;
          if (const std::optional<Value> stdout_kw =
                  keyword_arg_value(kw_args, "stdout")) {
            const std::optional<std::shared_ptr<RuntimeTextWriter>> writer =
                text_writer_from_value(frame, *stdout_kw, "stdout:");
            if (!writer.has_value()) {
              return SendStatus::Faulted;
            }
            stdout_writer = *writer;
          }
          if (const std::optional<Value> stderr_kw =
                  keyword_arg_value(kw_args, "stderr")) {
            const std::optional<std::shared_ptr<RuntimeTextWriter>> writer =
                text_writer_from_value(frame, *stderr_kw, "stderr:");
            if (!writer.has_value()) {
              return SendStatus::Faulted;
            }
            stderr_writer = *writer;
          }
          std::optional<NativeBlockInvoker> invoker =
              make_native_block_invoker(frame, block, "io.with_output");
          if (!invoker.has_value()) {
            return SendStatus::Faulted;
          }
          try {
            RuntimeOutputScope scope(std::move(stdout_writer),
                                     std::move(stderr_writer));
            *out = (*invoker)(std::vector<Value>{});
          } catch (const RuntimeTaskFailure &failure) {
            set_fault(frame, failure.error_name(), failure.message());
            return SendStatus::Faulted;
          }
          return SendStatus::Matched;
        }
        return SendStatus::NotHandled;
      }
      if (kind == RuntimeNativeTypeKind::TextBuffer) {
        if (selector != "new") {
          return SendStatus::NotHandled;
        }
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "io.Buffer.new does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::text_writer(RuntimeTextWriter::buffer());
        return SendStatus::Matched;
      }
      if (kind == RuntimeNativeTypeKind::Logger) {
        if (selector != "new") {
          return SendStatus::NotHandled;
        }
        if (!require_arity(0) ||
            !reject_unknown_keywords(frame, kw_args,
                                     {"to", "level", "color"}) ||
            !require_no_block()) {
          return SendStatus::Faulted;
        }
        std::shared_ptr<RuntimeTextWriter> writer = current_runtime_stderr();
        RuntimeLogLevel level = RuntimeLogLevel::Info;
        RuntimeLogColorMode color_mode = RuntimeLogColorMode::Auto;
        if (const std::optional<Value> to = keyword_arg_value(kw_args, "to")) {
          const std::optional<std::shared_ptr<RuntimeTextWriter>>
              explicit_writer = text_writer_from_value(frame, *to, "to:");
          if (!explicit_writer.has_value()) {
            return SendStatus::Faulted;
          }
          writer = *explicit_writer;
        }
        if (const std::optional<Value> level_value =
                keyword_arg_value(kw_args, "level")) {
          const std::optional<RuntimeLogLevel> parsed =
              log_level_from_value(frame, *level_value, "level");
          if (!parsed.has_value()) {
            return SendStatus::Faulted;
          }
          level = *parsed;
        }
        if (const std::optional<Value> color_value =
                keyword_arg_value(kw_args, "color")) {
          const std::optional<RuntimeLogColorMode> parsed =
              log_color_mode_from_value(frame, *color_value, "color");
          if (!parsed.has_value()) {
            return SendStatus::Faulted;
          }
          color_mode = *parsed;
        }
        *out = Value::logger(
            std::make_shared<RuntimeLogger>(writer, level, color_mode));
        return SendStatus::Matched;
      }
      if (is_conversion_type(kind)) {
        if (selector != "cast" && selector != "new" && selector != "parse") {
          return SendStatus::NotHandled;
        }
        if (!require_arity(1) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "conversion type call does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        if (selector == "parse" && !args[0].is_string()) {
          set_fault(frame, "TypeError", "parse expects Str input");
          return SendStatus::Faulted;
        }
        ConversionResult converted =
            convert_value_to_native_type(frame, args[0], kind);
        if (!converted.ok) {
          set_fault(frame, converted.error_name, converted.message);
          return SendStatus::Faulted;
        }
        *out = converted.value;
        return SendStatus::Matched;
      }
      if (selector != "new") {
        return SendStatus::NotHandled;
      }
      if (kind == RuntimeNativeTypeKind::Channel) {
        if (!reject_unknown_keywords(frame, kw_args, {"capacity"}) ||
            !require_no_block()) {
          return SendStatus::Faulted;
        }
        std::size_t capacity = 0;
        if (!capacity_from_args(frame, args, kw_args, &capacity)) {
          return SendStatus::Faulted;
        }
        *out = Value::channel(std::make_shared<RuntimeChannel>(capacity));
        return SendStatus::Matched;
      }
      if (kind == RuntimeNativeTypeKind::Mutex) {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError", "Mutex.new does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::mutex(std::make_shared<RuntimeMutex>());
        return SendStatus::Matched;
      }
      if (kind == RuntimeNativeTypeKind::Atomic) {
        if (!require_arity(1) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "Atomic.new does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        try {
          *out = Value::atomic(std::make_shared<RuntimeAtomic>(args[0]));
        } catch (const RuntimeTaskFailure &failure) {
          set_fault(frame, failure.error_name(), failure.message());
          return SendStatus::Faulted;
        }
        return SendStatus::Matched;
      }
      if (kind == RuntimeNativeTypeKind::Barrier) {
        if (!require_arity(1) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "Barrier.new does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        if (!args[0].is_integer() || args[0].as_integer() <= 0) {
          set_fault(frame, "TypeError", "barrier parties must be positive");
          return SendStatus::Faulted;
        }
        try {
          *out = Value::barrier(std::make_shared<RuntimeBarrier>(
              static_cast<std::size_t>(args[0].as_integer())));
        } catch (const RuntimeTaskFailure &failure) {
          set_fault(frame, failure.error_name(), failure.message());
          return SendStatus::Faulted;
        }
        return SendStatus::Matched;
      }
      if (kind == RuntimeNativeTypeKind::Flow) {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError", "Flow.new does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::flow_module(std::make_shared<RuntimeFlowModule>());
        return SendStatus::Matched;
      }
      if (kind == RuntimeNativeTypeKind::ThreadedCollection) {
        if ((args.size() < 1 || args.size() > 2) || !require_no_block()) {
          set_fault(frame, "TypeError",
                    "ThreadedCollection.new expects items and workers");
          return SendStatus::Faulted;
        }
        RuntimeFlowPartitionPolicy scatter_policy =
            RuntimeFlowPartitionPolicy::Atomic;
        if (!threaded_scatter_policy_from_keywords(frame, kw_args,
                                                   &scatter_policy)) {
          return SendStatus::Faulted;
        }
        bool source_was_tuple = false;
        std::optional<std::vector<Value>> items =
            extract_sequence_items(frame, args[0], &source_was_tuple);
        if (!items.has_value()) {
          set_fault(frame, "TypeError",
                    "ThreadedCollection.new expects sequence items");
          return SendStatus::Faulted;
        }
        std::size_t workers = 0;
        if (args.size() == 2) {
          if (!args[1].is_integer() || args[1].as_integer() < 0) {
            set_fault(frame, "TypeError",
                      "ThreadedCollection workers must be non-negative");
            return SendStatus::Faulted;
          }
          workers = static_cast<std::size_t>(args[1].as_integer());
        }
        *out = Value::threaded_collection(
            std::make_shared<RuntimeThreadedCollection>(
                *items, workers, RuntimeFlowOptions{}, scatter_policy));
        return SendStatus::Matched;
      }
    }

    if (receiver.is_text_writer()) {
      std::shared_ptr<RuntimeTextWriter> writer = receiver.as_text_writer();
      if (writer == nullptr) {
        set_fault(frame, "TypeError", "text writer is null");
        return SendStatus::Faulted;
      }
      auto require_string_arg = [&](std::size_t index,
                                    std::string *text) -> bool {
        if (index >= args.size() || !args[index].is_string()) {
          set_fault(frame, "TypeError", "text writer expects Str argument");
          return false;
        }
        const std::optional<std::string> raw =
            string_text_from_id(args[index].as_string().string_id);
        if (!raw.has_value()) {
          set_fault(frame, "VMError", "string ref is invalid");
          return false;
        }
        *text = *raw;
        return true;
      };

      if (selector == "write_str") {
        if (!require_arity(1) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError", "write_str does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        std::string text;
        if (!require_string_arg(0, &text)) {
          return SendStatus::Faulted;
        }
        if (!set_fault_from_text_write_result(frame, writer->write_str(text))) {
          return SendStatus::Faulted;
        }
        *out = Value::null();
        return SendStatus::Matched;
      }
      if (selector == "write_line") {
        if ((args.size() > 1U) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "write_line does not accept keywords");
          } else if (args.size() > 1U) {
            set_fault(frame, "TypeError",
                      "write_line accepts zero or one argument");
          }
          return SendStatus::Faulted;
        }
        std::string text;
        if (!args.empty() && !require_string_arg(0, &text)) {
          return SendStatus::Faulted;
        }
        if (!set_fault_from_text_write_result(frame,
                                              writer->write_line(text))) {
          return SendStatus::Faulted;
        }
        *out = Value::null();
        return SendStatus::Matched;
      }
      if (selector == "flush") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError", "flush does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        if (!set_fault_from_text_write_result(frame, writer->flush())) {
          return SendStatus::Faulted;
        }
        *out = Value::null();
        return SendStatus::Matched;
      }
      if (selector == "close") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError", "close does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        if (!set_fault_from_text_write_result(frame, writer->close())) {
          return SendStatus::Faulted;
        }
        *out = Value::null();
        return SendStatus::Matched;
      }
      if (selector == "closed?") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError", "closed? does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::boolean(writer->closed());
        return SendStatus::Matched;
      }
      if (selector == "to_str" || selector == "str") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError", "to_str does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        if (!writer->buffered()) {
          set_fault(frame, "TypeError", "to_str requires buffered writer");
          return SendStatus::Faulted;
        }
        *out = string_value_from_text(writer->to_string());
        return SendStatus::Matched;
      }
    }

    if (receiver.is_logger()) {
      std::shared_ptr<RuntimeLogger> logger = receiver.as_logger();
      if (logger == nullptr) {
        set_fault(frame, "TypeError", "logger is null");
        return SendStatus::Faulted;
      }

      auto apply_logger_log = [&](RuntimeLogLevel level) -> SendStatus {
        if (!require_arity(1) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "logger level method does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        const std::string message = runtime_stringify_value(
            args[0], RuntimeStringifyMode::Display, &module_, nullptr, nullptr,
            RuntimePrettyPrintOptions{});
        if (!set_fault_from_text_write_result(frame,
                                              logger->log(level, message))) {
          return SendStatus::Faulted;
        }
        *out = Value::null();
        return SendStatus::Matched;
      };

      if (selector == "fatal") {
        return apply_logger_log(RuntimeLogLevel::Fatal);
      }
      if (selector == "error") {
        return apply_logger_log(RuntimeLogLevel::Error);
      }
      if (selector == "warn" || selector == "warning") {
        return apply_logger_log(RuntimeLogLevel::Warn);
      }
      if (selector == "info") {
        return apply_logger_log(RuntimeLogLevel::Info);
      }
      if (selector == "debug") {
        return apply_logger_log(RuntimeLogLevel::Debug);
      }
      if (selector == "log") {
        if (!require_arity(2) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "logger.log does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        const std::optional<RuntimeLogLevel> level =
            log_level_from_value(frame, args[0], "logger.log level");
        if (!level.has_value()) {
          return SendStatus::Faulted;
        }
        const std::string message = runtime_stringify_value(
            args[1], RuntimeStringifyMode::Display, &module_, nullptr, nullptr,
            RuntimePrettyPrintOptions{});
        if (!set_fault_from_text_write_result(frame,
                                              logger->log(*level, message))) {
          return SendStatus::Faulted;
        }
        *out = Value::null();
        return SendStatus::Matched;
      }
      if (selector == "flush") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "logger.flush does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        if (!set_fault_from_text_write_result(frame, logger->flush())) {
          return SendStatus::Faulted;
        }
        *out = Value::null();
        return SendStatus::Matched;
      }
      if (selector == "close") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "logger.close does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        if (!set_fault_from_text_write_result(frame, logger->close())) {
          return SendStatus::Faulted;
        }
        *out = Value::null();
        return SendStatus::Matched;
      }
      if (selector == "closed?") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "logger.closed? does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::boolean(logger->closed());
        return SendStatus::Matched;
      }
    }

    if (receiver.is_task_module()) {
      const std::shared_ptr<RuntimeTaskModule> task = receiver.as_task_module();
      if (task == nullptr) {
        set_fault(frame, "TypeError", "task module is null");
        return SendStatus::Faulted;
      }
      if (selector == "annotation" || selector == "current_annotation") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "task.annotation does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = string_value_from_text(current_runtime_task_annotation());
        return SendStatus::Matched;
      }
      if (selector == "task_id" || selector == "current_task_id") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "task.task_id does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::integer(
            static_cast<std::int64_t>(current_runtime_task_id()));
        return SendStatus::Matched;
      }
      if (selector == "strand_id") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "task.strand_id does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::integer(
            static_cast<std::int64_t>(current_runtime_strand_id()));
        return SendStatus::Matched;
      }
      if (selector == "worker_id") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "task.worker_id does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::integer(
            static_cast<std::int64_t>(current_runtime_worker_id()));
        return SendStatus::Matched;
      }
      if (selector == "thread_id") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "task.thread_id does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::integer(
            static_cast<std::int64_t>(current_runtime_native_thread_id()));
        return SendStatus::Matched;
      }
      if (selector == "with_annotation" || selector == "annotate") {
        if (!require_arity(1) || !kw_args.empty()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "task.with_annotation does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        if (!args[0].is_string()) {
          set_fault(frame, "TypeError",
                    "task.with_annotation expects Str annotation");
          return SendStatus::Faulted;
        }
        const std::optional<std::string> annotation =
            string_text_from_id(args[0].as_string().string_id);
        if (!annotation.has_value()) {
          set_fault(frame, "VMError", "string ref is invalid");
          return SendStatus::Faulted;
        }
        std::optional<NativeBlockInvoker> invoker =
            make_native_block_invoker(frame, block, "task.with_annotation");
        if (!invoker.has_value()) {
          return SendStatus::Faulted;
        }
        try {
          RuntimeTaskAnnotationScope annotation_scope(*annotation);
          *out = (*invoker)(std::vector<Value>{});
        } catch (const RuntimeTaskFailure &failure) {
          set_fault(frame, failure.error_name(), failure.message());
          return SendStatus::Faulted;
        }
        return SendStatus::Matched;
      }
      if (selector == "flow") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError", "task.flow does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::flow_module(std::make_shared<RuntimeFlowModule>());
        return SendStatus::Matched;
      }
      if (selector == "async" || selector == "spawn") {
        if (!require_arity(0) || !kw_args.empty()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "task async/spawn does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        std::optional<NativeBlockInvoker> invoker =
            make_native_block_invoker(frame, block, "task " + selector);
        if (!invoker.has_value()) {
          return SendStatus::Faulted;
        }
        const std::shared_ptr<RuntimeTextWriter> inherited_stdout =
            current_runtime_stdout();
        const std::shared_ptr<RuntimeTextWriter> inherited_stderr =
            current_runtime_stderr();
        auto task_function = [invoker = *invoker, inherited_stdout,
                              inherited_stderr]() mutable {
          RuntimeOutputScope output_scope(inherited_stdout, inherited_stderr);
          return invoker(std::vector<Value>{});
        };
        RuntimeTaskHandle handle;
        if (selector == "async") {
          handle = task->async(std::move(task_function));
        } else {
          handle = task->spawn(std::move(task_function));
        }
        *out = Value::task_handle(
            std::make_shared<RuntimeTaskHandle>(std::move(handle)));
        return SendStatus::Matched;
      }
      if (selector == "sleep") {
        if (!require_arity(1) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "task.sleep does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        std::chrono::milliseconds duration;
        if (!duration_from_value(frame, args[0], &duration)) {
          return SendStatus::Faulted;
        }
        try {
          task->sleep(duration);
        } catch (const RuntimeTaskCancelled &cancelled) {
          (void)cancelled;
          set_fault(frame, "CancelledError", "task cancelled");
          return SendStatus::Faulted;
        }
        *out = Value::null();
        return SendStatus::Matched;
      }
      if (selector == "yield") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "task.yield does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        try {
          task->yield_current();
        } catch (const RuntimeTaskCancelled &cancelled) {
          (void)cancelled;
          set_fault(frame, "CancelledError", "task cancelled");
          return SendStatus::Faulted;
        }
        *out = Value::null();
        return SendStatus::Matched;
      }
      if (selector == "sync") {
        if (!require_arity(0) || !kw_args.empty()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError", "task.sync does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        std::optional<NativeBlockInvoker> invoker =
            make_native_block_invoker(frame, block, "task.sync");
        if (!invoker.has_value()) {
          return SendStatus::Faulted;
        }
        try {
          *out = task->sync([invoker = *invoker]() mutable {
            return invoker(std::vector<Value>{});
          });
        } catch (const RuntimeTaskFailure &failure) {
          set_fault(frame, failure.error_name(), failure.message());
          return SendStatus::Faulted;
        } catch (const RuntimeTaskCancelled &cancelled) {
          (void)cancelled;
          set_fault(frame, "CancelledError", "task cancelled");
          return SendStatus::Faulted;
        }
        return SendStatus::Matched;
      }
    }

    if (receiver.is_task_handle()) {
      const std::shared_ptr<RuntimeTaskHandle> handle =
          receiver.as_task_handle();
      if (handle == nullptr) {
        set_fault(frame, "TypeError", "task handle is null");
        return SendStatus::Faulted;
      }
      if (selector == "wait") {
        if (!require_arity(0) ||
            !reject_unknown_keywords(frame, kw_args, {"timeout"}) ||
            !require_no_block()) {
          return SendStatus::Faulted;
        }
        std::chrono::milliseconds timeout;
        if (!timeout_from_keywords(frame, kw_args, &timeout)) {
          return SendStatus::Faulted;
        }
        const RuntimeTaskPublicResult waited = handle->wait(timeout);
        if (!waited.ok) {
          set_fault_from_task_result(frame, waited);
          return SendStatus::Faulted;
        }
        *out = waited.value;
        return SendStatus::Matched;
      }
      if (selector == "result") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "TaskHandle.result does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        const RuntimeTaskPublicResult result = handle->result();
        if (!result.ok) {
          set_fault_from_task_result(frame, result);
          return SendStatus::Faulted;
        }
        *out = result.value;
        return SendStatus::Matched;
      }
      if (selector == "cancel") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "TaskHandle.cancel does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::boolean(handle->cancel());
        return SendStatus::Matched;
      }
      if (selector == "resume") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "TaskHandle.resume does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::boolean(handle->resume());
        return SendStatus::Matched;
      }
      if (selector == "cancelled?" || selector == "done?" ||
          selector == "running?" || selector == "failed?") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "TaskHandle predicate does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        if (selector == "cancelled?") {
          *out = Value::boolean(handle->cancelled());
        } else if (selector == "done?") {
          *out = Value::boolean(handle->done());
        } else if (selector == "running?") {
          *out = Value::boolean(handle->running());
        } else {
          *out = Value::boolean(handle->failed());
        }
        return SendStatus::Matched;
      }
    }

    if (receiver.is_channel()) {
      const std::shared_ptr<RuntimeChannel> channel = receiver.as_channel();
      if (channel == nullptr) {
        set_fault(frame, "TypeError", "channel is null");
        return SendStatus::Faulted;
      }
      if (selector == "send") {
        if (!require_arity(1) ||
            !reject_unknown_keywords(frame, kw_args, {"timeout"}) ||
            !require_no_block()) {
          return SendStatus::Faulted;
        }
        std::chrono::milliseconds timeout;
        if (!timeout_from_keywords(frame, kw_args, &timeout)) {
          return SendStatus::Faulted;
        }
        const RuntimeChannelResult sent = channel->send(args[0], timeout);
        if (!sent.ok) {
          set_fault_from_channel_result(frame, sent);
          return SendStatus::Faulted;
        }
        *out = Value::boolean(sent.sent);
        return SendStatus::Matched;
      }
      if (selector == "recv") {
        if (!require_arity(0) ||
            !reject_unknown_keywords(frame, kw_args, {"timeout"}) ||
            !require_no_block()) {
          return SendStatus::Faulted;
        }
        std::chrono::milliseconds timeout;
        if (!timeout_from_keywords(frame, kw_args, &timeout)) {
          return SendStatus::Faulted;
        }
        const RuntimeChannelResult received = channel->recv(timeout);
        if (!received.ok) {
          set_fault_from_channel_result(frame, received);
          return SendStatus::Faulted;
        }
        *out = received.value;
        return SendStatus::Matched;
      }
      if (selector == "close") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "Channel.close does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::boolean(channel->close());
        return SendStatus::Matched;
      }
      if (selector == "closed?") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "Channel.closed? does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::boolean(channel->closed());
        return SendStatus::Matched;
      }
    }

    if (receiver.is_mutex()) {
      const std::shared_ptr<RuntimeMutex> mutex = receiver.as_mutex();
      if (mutex == nullptr) {
        set_fault(frame, "TypeError", "mutex is null");
        return SendStatus::Faulted;
      }
      if (selector == "lock") {
        if (!require_arity(0) ||
            !reject_unknown_keywords(frame, kw_args, {"timeout"}) ||
            !require_no_block()) {
          return SendStatus::Faulted;
        }
        std::chrono::milliseconds timeout;
        if (!timeout_from_keywords(frame, kw_args, &timeout)) {
          return SendStatus::Faulted;
        }
        const RuntimeMutexResult locked = mutex->lock(timeout);
        if (!locked.ok) {
          set_fault_from_mutex_result(frame, locked);
          return SendStatus::Faulted;
        }
        *out = Value::boolean(locked.locked);
        return SendStatus::Matched;
      }
      if (selector == "unlock") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "Mutex.unlock does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        const RuntimeMutexResult unlocked = mutex->unlock();
        if (!unlocked.ok) {
          set_fault_from_mutex_result(frame, unlocked);
          return SendStatus::Faulted;
        }
        *out = Value::boolean(unlocked.unlocked);
        return SendStatus::Matched;
      }
      if (selector == "synchronize") {
        if (!require_arity(0) ||
            !reject_unknown_keywords(frame, kw_args, {"timeout"})) {
          return SendStatus::Faulted;
        }
        std::chrono::milliseconds timeout;
        if (!timeout_from_keywords(frame, kw_args, &timeout)) {
          return SendStatus::Faulted;
        }
        std::optional<NativeBlockInvoker> invoker =
            make_native_block_invoker(frame, block, "Mutex.synchronize");
        if (!invoker.has_value()) {
          return SendStatus::Faulted;
        }
        try {
          const RuntimeMutexResult synchronized = mutex->synchronize(
              [invoker = *invoker]() mutable {
                return invoker(std::vector<Value>{});
              },
              timeout);
          if (!synchronized.ok) {
            set_fault_from_mutex_result(frame, synchronized);
            return SendStatus::Faulted;
          }
          *out = synchronized.value;
        } catch (const RuntimeTaskFailure &failure) {
          set_fault(frame, failure.error_name(), failure.message());
          return SendStatus::Faulted;
        }
        return SendStatus::Matched;
      }
      if (selector == "locked?" || selector == "owned?") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "Mutex predicate does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = Value::boolean(selector == "locked?" ? mutex->locked()
                                                    : mutex->owned());
        return SendStatus::Matched;
      }
    }

    if (receiver.is_atomic()) {
      const std::shared_ptr<RuntimeAtomic> atomic = receiver.as_atomic();
      if (atomic == nullptr) {
        set_fault(frame, "TypeError", "atomic is null");
        return SendStatus::Faulted;
      }
      if (selector == "get") {
        if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "Atomic.get does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        *out = atomic->get_value();
        return SendStatus::Matched;
      }
      if (selector == "set") {
        if (!require_arity(1) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "Atomic.set does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        const RuntimeAtomic::Result result = atomic->set_value(args[0]);
        if (!result.ok) {
          set_fault_from_atomic_result(frame, result);
          return SendStatus::Faulted;
        }
        *out = result.value;
        return SendStatus::Matched;
      }
      if (selector == "compare_and_set") {
        if (!require_arity(2) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "Atomic.compare_and_set does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        const RuntimeAtomic::Result result =
            atomic->compare_and_set_value(args[0], args[1]);
        if (!result.ok) {
          set_fault_from_atomic_result(frame, result);
          return SendStatus::Faulted;
        }
        *out = Value::boolean(result.matched);
        return SendStatus::Matched;
      }
      if (selector == "update") {
        if (!require_arity(0) || !kw_args.empty()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "Atomic.update does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        std::optional<NativeBlockInvoker> invoker =
            make_native_block_invoker(frame, block, "Atomic.update");
        if (!invoker.has_value()) {
          return SendStatus::Faulted;
        }
        const RuntimeAtomic::Result result =
            atomic->update([invoker = *invoker](const Value &current) mutable {
              return invoker(std::vector<Value>{current});
            });
        if (!result.ok) {
          set_fault_from_atomic_result(frame, result);
          return SendStatus::Faulted;
        }
        *out = result.value;
        return SendStatus::Matched;
      }
    }

    if (receiver.is_barrier()) {
      const std::shared_ptr<RuntimeBarrier> barrier = receiver.as_barrier();
      if (barrier == nullptr) {
        set_fault(frame, "TypeError", "barrier is null");
        return SendStatus::Faulted;
      }
      if (selector == "wait") {
        if (!require_arity(0) ||
            !reject_unknown_keywords(frame, kw_args, {"timeout"}) ||
            !require_no_block()) {
          return SendStatus::Faulted;
        }
        std::chrono::milliseconds timeout;
        if (!timeout_from_keywords(frame, kw_args, &timeout)) {
          return SendStatus::Faulted;
        }
        const RuntimeBarrierResult waited = barrier->wait(timeout);
        if (!waited.ok) {
          set_fault_from_barrier_result(frame, waited);
          return SendStatus::Faulted;
        }
        *out = Value::boolean(waited.passed);
        return SendStatus::Matched;
      }
    }

    if (receiver.is_flow_module()) {
      const std::shared_ptr<RuntimeFlowModule> flow = receiver.as_flow_module();
      if (flow == nullptr) {
        set_fault(frame, "TypeError", "flow module is null");
        return SendStatus::Faulted;
      }
      if (selector == "scatter_map" || selector == "scatter") {
        if (!require_arity(1) || !kw_args.empty()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "flow scatter_map does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        bool source_was_tuple = false;
        std::optional<std::vector<Value>> items =
            extract_sequence_items(frame, args[0], &source_was_tuple);
        if (!items.has_value()) {
          set_fault(frame, "TypeError", "flow scatter_map expects sequence");
          return SendStatus::Faulted;
        }
        std::optional<NativeBlockInvoker> invoker =
            make_native_block_invoker(frame, block, "flow scatter_map");
        if (!invoker.has_value()) {
          return SendStatus::Faulted;
        }
        RuntimeFlowGatherResult result = flow->scatter_map(
            *items, [invoker = *invoker](const Value &value,
                                         std::size_t index) mutable {
              return invoker(std::vector<Value>{
                  value, Value::integer(static_cast<std::int64_t>(index))});
            });
        if (!result.ok || result.failed) {
          set_fault_from_flow_result(frame, result);
          return SendStatus::Faulted;
        }
        *out = make_list_value(std::move(result.values));
        return SendStatus::Matched;
      }
      if (selector == "broadcast") {
        if (!require_arity(2) || !kw_args.empty()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "flow broadcast does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        if (!args[1].is_integer() || args[1].as_integer() < 0) {
          set_fault(frame, "TypeError",
                    "flow broadcast workers must be non-negative");
          return SendStatus::Faulted;
        }
        std::optional<NativeBlockInvoker> invoker =
            make_native_block_invoker(frame, block, "flow broadcast");
        if (!invoker.has_value()) {
          return SendStatus::Faulted;
        }
        RuntimeFlowGatherResult result = flow->broadcast(
            args[0], static_cast<std::size_t>(args[1].as_integer()),
            [invoker = *invoker](const Value &value,
                                 std::size_t index) mutable {
              return invoker(std::vector<Value>{
                  value, Value::integer(static_cast<std::int64_t>(index))});
            });
        if (!result.ok || result.failed) {
          set_fault_from_flow_result(frame, result);
          return SendStatus::Faulted;
        }
        *out = make_list_value(std::move(result.values));
        return SendStatus::Matched;
      }
      if (selector == "gather") {
        if (!require_arity(1) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "flow gather does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        bool source_was_tuple = false;
        std::optional<std::vector<Value>> values =
            extract_sequence_items(frame, args[0], &source_was_tuple);
        if (!values.has_value()) {
          set_fault(frame, "TypeError", "flow gather expects sequence");
          return SendStatus::Faulted;
        }
        std::vector<RuntimeTaskHandle> handles;
        handles.reserve(values->size());
        for (const Value &value : *values) {
          if (!value.is_task_handle() || value.as_task_handle() == nullptr) {
            set_fault(frame, "TypeError",
                      "flow gather expects TaskHandle values");
            return SendStatus::Faulted;
          }
          handles.push_back(*value.as_task_handle());
        }
        RuntimeFlowGatherResult result = flow->gather(std::move(handles));
        if (!result.ok || result.failed) {
          set_fault_from_flow_result(frame, result);
          return SendStatus::Faulted;
        }
        *out = make_list_value(std::move(result.values));
        return SendStatus::Matched;
      }
    }

    if (receiver.is_threaded_collection()) {
      const std::shared_ptr<RuntimeThreadedCollection> threaded =
          receiver.as_threaded_collection();
      if (threaded == nullptr) {
        set_fault(frame, "TypeError", "threaded collection is null");
        return SendStatus::Faulted;
      }
      const std::string collection_selector =
          canonical_collection_selector(selector);
      if (collection_selector == "map" || collection_selector == "select" ||
          collection_selector == "reject" ||
          collection_selector == "flat_map" || collection_selector == "each") {
        if (!require_arity(0) || !kw_args.empty()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "threaded collection operation does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        std::optional<NativeBlockInvoker> invoker = make_native_block_invoker(
            frame, block, "threaded collection " + collection_selector);
        if (!invoker.has_value()) {
          return SendStatus::Faulted;
        }
        RuntimeFlowGatherResult result;
        if (collection_selector == "map") {
          result =
              threaded->map([invoker = *invoker](const Value &value,
                                                 std::size_t index) mutable {
                return invoker(std::vector<Value>{
                    value, Value::integer(static_cast<std::int64_t>(index))});
              });
        } else if (collection_selector == "select") {
          result =
              threaded->select([invoker = *invoker](const Value &value,
                                                    std::size_t index) mutable {
                const Value kept = invoker(std::vector<Value>{
                    value, Value::integer(static_cast<std::int64_t>(index))});
                return is_truthy(kept);
              });
        } else if (collection_selector == "reject") {
          result =
              threaded->reject([invoker = *invoker](const Value &value,
                                                    std::size_t index) mutable {
                const Value rejected = invoker(std::vector<Value>{
                    value, Value::integer(static_cast<std::int64_t>(index))});
                return is_truthy(rejected);
              });
        } else if (collection_selector == "flat_map") {
          result = threaded->flat_map(
              [invoker = *invoker](const Value &value,
                                   std::size_t index) mutable {
                const Value mapped = invoker(std::vector<Value>{
                    value, Value::integer(static_cast<std::int64_t>(index))});
                return native_sequence_items_or_throw(
                    mapped, "threaded flat_map block");
              });
        } else {
          result =
              threaded->each([invoker = *invoker](const Value &value,
                                                  std::size_t index) mutable {
                (void)invoker(std::vector<Value>{
                    value, Value::integer(static_cast<std::int64_t>(index))});
              });
        }
        if (!result.ok || result.failed) {
          set_fault_from_flow_result(frame, result);
          return SendStatus::Faulted;
        }
        *out = make_list_value(std::move(result.values));
        return SendStatus::Matched;
      }
      if (collection_selector == "combination" ||
          collection_selector == "permutation") {
        if (!require_arity(1) || !kw_args.empty() || !require_no_block()) {
          if (!kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "threaded generated operation does not accept keywords");
          }
          return SendStatus::Faulted;
        }
        if (!args[0].is_integer() || args[0].as_integer() < 0) {
          set_fault(frame, "TypeError", "generated count must be non-negative");
          return SendStatus::Faulted;
        }
        RuntimeFlowGatherResult result =
            collection_selector == "combination"
                ? threaded->combination(
                      static_cast<std::size_t>(args[0].as_integer()))
                : threaded->permutation(
                      static_cast<std::size_t>(args[0].as_integer()));
        if (!result.ok || result.failed) {
          set_fault_from_flow_result(frame, result);
          return SendStatus::Faulted;
        }
        *out = make_list_value(std::move(result.values));
        return SendStatus::Matched;
      }
    }

    if ((receiver.is_list() || receiver.is_tuple() || receiver.is_set()) &&
        (selector == "threaded" || selector == "parallel")) {
      if (!block.is_null()) {
        set_fault(frame, "TypeError", selector + " does not accept block");
        return SendStatus::Faulted;
      }
      RuntimeFlowPartitionPolicy scatter_policy =
          RuntimeFlowPartitionPolicy::Atomic;
      if (!threaded_scatter_policy_from_keywords(frame, kw_args,
                                                 &scatter_policy)) {
        return SendStatus::Faulted;
      }
      std::size_t workers = 0;
      if (!workers_from_optional_arg(frame, args, &workers)) {
        return SendStatus::Faulted;
      }
      bool source_was_tuple = false;
      std::optional<std::vector<Value>> items =
          extract_sequence_items(frame, receiver, &source_was_tuple);
      if (!items.has_value()) {
        return SendStatus::Faulted;
      }
      *out = Value::threaded_collection(
          std::make_shared<RuntimeThreadedCollection>(
              *items, workers, RuntimeFlowOptions{}, scatter_policy));
      return SendStatus::Matched;
    }

    return SendStatus::NotHandled;
  }

  SendStatus try_apply_scalar_send(
      const Frame &frame, const Value &receiver, const std::string &selector,
      const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args, Value *out) {
    if (!ensure_lifecycle_access(frame, receiver)) {
      return SendStatus::Faulted;
    }

    Value native_result = Value::null();
    const SendStatus native_status = try_apply_native_stdlib_send(
        frame, receiver, selector, args, block, kw_args, &native_result);
    if (native_status != SendStatus::NotHandled) {
      if (native_status == SendStatus::Matched) {
        *out = std::move(native_result);
      }
      return native_status;
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

    auto require_numeric_arg = [&](std::size_t index, double *value) -> bool {
      if (index >= args.size()) {
        set_fault(frame, "TypeError", "builtin SEND expects numeric argument");
        return false;
      }
      if (args[index].is_integer()) {
        *value = static_cast<double>(args[index].as_integer());
        return true;
      }
      if (args[index].is_float()) {
        *value = args[index].as_float();
        return true;
      }
      set_fault(frame, "TypeError", "builtin SEND expects numeric argument");
      return false;
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

    const std::string collection_selector =
        canonical_collection_selector(selector);
    auto collection_selector_in =
        [&](std::initializer_list<const char *> names) -> bool {
      for (const char *name : names) {
        if (collection_selector == name) {
          return true;
        }
      }
      return false;
    };
    const bool count_alias_accepts_block = selector == "count";

    auto require_receiver_live_after_block = [&]() -> bool {
      return ensure_lifecycle_access(frame, receiver);
    };

    auto apply_conversion_result = [&](ConversionResult converted,
                                       bool nullable) -> SendStatus {
      if (converted.ok) {
        *out = converted.value;
        return SendStatus::Matched;
      }
      if (nullable && conversion_error_is_nullable(converted)) {
        *out = Value::null();
        return SendStatus::Matched;
      }
      set_fault(frame, converted.error_name, converted.message);
      return SendStatus::Faulted;
    };

    if (selector == "cast" || selector == "cast?" || selector == "to_type") {
      if (!require_arity(1) || !kw_args.empty() || !require_no_block()) {
        if (!kw_args.empty()) {
          set_fault(frame, "TypeError", "cast does not accept keywords");
        }
        return SendStatus::Faulted;
      }
      const std::optional<RuntimeNativeTypeKind> target =
          conversion_type_from_value(args[0]);
      if (!target.has_value()) {
        set_fault(frame, "TypeError", "cast target must be a type object");
        return SendStatus::Faulted;
      }
      return apply_conversion_result(
          convert_value_to_native_type(frame, receiver, *target),
          selector == "cast?");
    }

    if (const std::optional<RuntimeNativeTypeKind> target =
            conversion_target_for_to_method(selector)) {
      if (!require_arity(0) || !kw_args.empty() || !require_no_block()) {
        if (!kw_args.empty()) {
          set_fault(frame, "TypeError",
                    "to_* conversion does not accept keywords");
        }
        return SendStatus::Faulted;
      }
      return apply_conversion_result(
          convert_value_to_native_type(frame, receiver, *target), false);
    }

    if (receiver.is_string() && (selector == "+" || selector == "concat")) {
      if (!require_arity(1) || !require_no_block()) {
        return SendStatus::Faulted;
      }
      if (!args[0].is_string()) {
        set_fault(frame, "TypeError", "String#+ expects Str argument");
        return SendStatus::Faulted;
      }
      const std::optional<std::string> left =
          string_text_from_id(receiver.as_string().string_id);
      const std::optional<std::string> right =
          string_text_from_id(args[0].as_string().string_id);
      if (!left.has_value() || !right.has_value()) {
        set_fault(frame, "VMError", "string ref is invalid");
        return SendStatus::Faulted;
      }
      *out = Value::string(intern_runtime_string(*left + *right));
      return SendStatus::Matched;
    }

    const bool receiver_is_range = value_is_range_instance(receiver);
    const bool receiver_is_lazy_seq = value_is_lazy_seq_instance(receiver);
    const bool sequence_set_operation_selector =
        collection_selector_in({"contains?",
                                "include?",
                                "union",
                                "intersection",
                                "difference",
                                "left_difference",
                                "symmetric_difference",
                                "subset?",
                                "proper_subset?",
                                "superset?",
                                "proper_superset?",
                                "disjoint?",
                                "permutation",
                                "combination",
                                "&",
                                "|",
                                "-",
                                "^",
                                "<=",
                                "<",
                                ">=",
                                ">"});
    const bool sequence_extra_operation_selector =
        collection_selector_in({"+", "*", "concat", "take_while", "reverse",
                                "sort", "uniq", "each_pair", "each_cons"});
    const bool sequence_collection_selector =
        collection_selector_in({"empty?", "[]", "deconstruct", "first", "count",
                                "to_a", "lazy", "each", "map", "flat_map",
                                "select", "reject", "find", "group_by", "any?",
                                "all?", "none?", "reduce"}) ||
        sequence_set_operation_selector || sequence_extra_operation_selector;
    const bool range_collection_selector =
        sequence_collection_selector || selector == "===";
    const bool lazy_seq_collection_selector = sequence_collection_selector;
    const bool numeric_selector =
        selector_in({"+", "-", "*", "/", "%", "//", ">", "<",
                     ">=", "<=", "==", "!=", "<=>"});
    const bool builtin_selector =
        selector_in({"==", "===", "!="}) ||
        ((receiver.is_list() || receiver.is_tuple() || receiver.is_set()) &&
         sequence_collection_selector) ||
        (receiver_is_range && range_collection_selector) ||
        (receiver_is_lazy_seq && lazy_seq_collection_selector) ||
        (receiver.is_map() && collection_selector_in({"empty?",
                                                      "[]",
                                                      "deconstruct_keys",
                                                      "keys",
                                                      "values",
                                                      "entries",
                                                      "to_a",
                                                      "count",
                                                      "each",
                                                      "map",
                                                      "select",
                                                      "reject",
                                                      "transform",
                                                      "transform_values",
                                                      "merge",
                                                      "each_pair",
                                                      "contains?",
                                                      "include?",
                                                      "value?",
                                                      "has_value?",
                                                      "+",
                                                      "|"})) ||
        ((receiver.is_integer() || receiver.is_float()) && numeric_selector);
    const bool keyword_compatible_builtin_selector =
        collection_selector == "each" &&
        (receiver.is_list() || receiver.is_tuple() || receiver.is_set() ||
         receiver_is_range || receiver_is_lazy_seq);
    if (!kw_args.empty() && builtin_selector &&
        !keyword_compatible_builtin_selector) {
      set_fault(frame, "TypeError",
                "builtin SEND does not accept keyword arguments");
      return SendStatus::Faulted;
    }

    if ((selector == "==" || selector == "===" || selector == "!=") &&
        !receiver.is_instance_object() && !receiver.is_class_object()) {
      if (!require_arity(1) || !require_no_block()) {
        return SendStatus::Faulted;
      }
      if (!ensure_lifecycle_access(frame, args[0])) {
        return SendStatus::Faulted;
      }
      const bool equal = value_equals(receiver, args[0]);
      *out = Value::boolean(selector == "!=" ? !equal : equal);
      return SendStatus::Matched;
    }

    if (receiver_is_lazy_seq && lazy_seq_collection_selector) {
      if (collection_selector == "lazy") {
        if (!require_arity(0) || !require_no_block()) {
          return SendStatus::Faulted;
        }
        *out = receiver;
        return SendStatus::Matched;
      }

      const std::optional<LazySeqOpKind> op_kind =
          lazy_seq_op_kind_for_selector(collection_selector);
      if (op_kind.has_value()) {
        if (!require_arity(0)) {
          return SendStatus::Faulted;
        }
        const std::optional<Value> next =
            append_lazy_seq_op(frame, receiver, *op_kind, block);
        if (!next.has_value()) {
          return SendStatus::Faulted;
        }
        *out = *next;
        return SendStatus::Matched;
      }

      const std::optional<LazySeqState> state =
          extract_lazy_seq_state(frame, receiver);
      if (fault_.has_value() || !state.has_value()) {
        return SendStatus::Faulted;
      }

      if (collection_selector == "to_a" ||
          collection_selector == "deconstruct") {
        if (!require_arity(0) || !require_no_block()) {
          return SendStatus::Faulted;
        }
        const std::optional<std::vector<Value>> items =
            materialize_lazy_seq_items(frame, *state, receiver, std::nullopt);
        if (!items.has_value()) {
          return SendStatus::Faulted;
        }
        *out = make_list_value(*items);
        return SendStatus::Matched;
      }

      if (collection_selector == "empty?") {
        if (!require_arity(0) || !require_no_block()) {
          return SendStatus::Faulted;
        }
        bool saw_item = false;
        const LazySeqVisitStatus status =
            visit_lazy_seq(frame, *state, receiver,
                           [&](const Value &item) -> LazySeqVisitStatus {
                             (void)item;
                             saw_item = true;
                             return LazySeqVisitStatus::Stop;
                           });
        if (status == LazySeqVisitStatus::Faulted) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(!saw_item);
        return SendStatus::Matched;
      }

      if (collection_selector == "[]") {
        std::int64_t index = 0;
        if (!require_arity(1) || !require_no_block() ||
            !require_integer_arg(0, &index)) {
          return SendStatus::Faulted;
        }
        if (index < 0) {
          set_fault(frame, "IndexError", "LazySeq index is out of bounds");
          return SendStatus::Faulted;
        }
        std::int64_t seen = 0;
        bool found_value = false;
        Value found = Value::null();
        const LazySeqVisitStatus status =
            visit_lazy_seq(frame, *state, receiver,
                           [&](const Value &item) -> LazySeqVisitStatus {
                             if (seen == index) {
                               found_value = true;
                               found = item;
                               return LazySeqVisitStatus::Stop;
                             }
                             ++seen;
                             return LazySeqVisitStatus::Continue;
                           });
        if (status == LazySeqVisitStatus::Faulted) {
          return SendStatus::Faulted;
        }
        if (!found_value) {
          set_fault(frame, "IndexError", "LazySeq index is out of bounds");
          return SendStatus::Faulted;
        }
        *out = found;
        return SendStatus::Matched;
      }

      if (collection_selector == "first") {
        if (!require_no_block()) {
          return SendStatus::Faulted;
        }
        if (args.empty()) {
          const std::optional<std::vector<Value>> items =
              materialize_lazy_seq_items(frame, *state, receiver, 1U);
          if (!items.has_value()) {
            return SendStatus::Faulted;
          }
          *out = items->empty() ? Value::null() : items->front();
          return SendStatus::Matched;
        }
        std::int64_t count = 0;
        if (!require_arity(1) || !require_integer_arg(0, &count)) {
          return SendStatus::Faulted;
        }
        const std::size_t take =
            count <= 0 ? 0U : static_cast<std::size_t>(count);
        const std::optional<std::vector<Value>> items =
            materialize_lazy_seq_items(frame, *state, receiver, take);
        if (!items.has_value()) {
          return SendStatus::Faulted;
        }
        *out = make_list_value(*items);
        return SendStatus::Matched;
      }

      if (collection_selector == "each" && args.empty() && kw_args.empty()) {
        if (!require_arity(0) ||
            !require_lazy_seq_finite_source(frame, *state, "run each")) {
          return SendStatus::Faulted;
        }
        const LazySeqVisitStatus status = visit_lazy_seq(
            frame, *state, receiver,
            [&](const Value &item) -> LazySeqVisitStatus {
              if (!call_block_to_value(frame, block, {item}).has_value()) {
                return LazySeqVisitStatus::Faulted;
              }
              if (!ensure_lifecycle_access(frame, receiver) ||
                  !ensure_lifecycle_access(frame, state->source)) {
                return LazySeqVisitStatus::Faulted;
              }
              return LazySeqVisitStatus::Continue;
            });
        if (status == LazySeqVisitStatus::Faulted) {
          return SendStatus::Faulted;
        }
        *out = receiver;
        return SendStatus::Matched;
      }

      if (collection_selector == "count") {
        if (!require_arity(0) ||
            (!count_alias_accepts_block && !require_no_block()) ||
            !require_lazy_seq_finite_source(frame, *state, "count all items")) {
          return SendStatus::Faulted;
        }
        std::int64_t count = 0;
        const LazySeqVisitStatus status = visit_lazy_seq(
            frame, *state, receiver,
            [&](const Value &item) -> LazySeqVisitStatus {
              if (block.is_null()) {
                ++count;
                return LazySeqVisitStatus::Continue;
              }
              const std::optional<Value> predicate =
                  call_block_to_value(frame, block, {item});
              if (!predicate.has_value()) {
                return LazySeqVisitStatus::Faulted;
              }
              if (!ensure_lifecycle_access(frame, receiver) ||
                  !ensure_lifecycle_access(frame, state->source)) {
                return LazySeqVisitStatus::Faulted;
              }
              if (is_truthy(*predicate)) {
                ++count;
              }
              return LazySeqVisitStatus::Continue;
            });
        if (status == LazySeqVisitStatus::Faulted) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(count);
        return SendStatus::Matched;
      }

      if (collection_selector == "find") {
        if (!require_arity(0)) {
          return SendStatus::Faulted;
        }
        Value found = Value::null();
        const LazySeqVisitStatus status = visit_lazy_seq(
            frame, *state, receiver,
            [&](const Value &item) -> LazySeqVisitStatus {
              const std::optional<Value> predicate =
                  call_block_to_value(frame, block, {item});
              if (!predicate.has_value()) {
                return LazySeqVisitStatus::Faulted;
              }
              if (!ensure_lifecycle_access(frame, receiver) ||
                  !ensure_lifecycle_access(frame, state->source)) {
                return LazySeqVisitStatus::Faulted;
              }
              if (is_truthy(*predicate)) {
                found = item;
                return LazySeqVisitStatus::Stop;
              }
              return LazySeqVisitStatus::Continue;
            });
        if (status == LazySeqVisitStatus::Faulted) {
          return SendStatus::Faulted;
        }
        *out = found;
        return SendStatus::Matched;
      }

      if (collection_selector == "group_by") {
        if (!require_arity(0) ||
            !require_lazy_seq_finite_source(frame, *state, "group all items")) {
          return SendStatus::Faulted;
        }
        std::vector<std::pair<std::uint32_t, std::vector<Value>>> groups;
        const LazySeqVisitStatus status = visit_lazy_seq(
            frame, *state, receiver,
            [&](const Value &item) -> LazySeqVisitStatus {
              const std::optional<Value> key =
                  call_block_to_value(frame, block, {item});
              if (!key.has_value()) {
                return LazySeqVisitStatus::Faulted;
              }
              if (!ensure_lifecycle_access(frame, receiver) ||
                  !ensure_lifecycle_access(frame, state->source)) {
                return LazySeqVisitStatus::Faulted;
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
                  return LazySeqVisitStatus::Faulted;
                }
                key_symbol_id = symbol_id_for_text(*text);
              }
              if (!key_symbol_id.has_value()) {
                set_fault(frame, "TypeError",
                          "group_by block must return Symbol key");
                return LazySeqVisitStatus::Faulted;
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
              return LazySeqVisitStatus::Continue;
            });
        if (status == LazySeqVisitStatus::Faulted) {
          return SendStatus::Faulted;
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

      if (collection_selector == "any?" || collection_selector == "all?" ||
          collection_selector == "none?") {
        if (!require_arity(0)) {
          return SendStatus::Faulted;
        }
        bool saw_any = false;
        bool all_match = true;
        bool any_match = false;
        const LazySeqVisitStatus status = visit_lazy_seq(
            frame, *state, receiver,
            [&](const Value &item) -> LazySeqVisitStatus {
              saw_any = true;
              Value predicate = item;
              if (!block.is_null()) {
                const std::optional<Value> value =
                    call_block_to_value(frame, block, {item});
                if (!value.has_value()) {
                  return LazySeqVisitStatus::Faulted;
                }
                if (!ensure_lifecycle_access(frame, receiver) ||
                    !ensure_lifecycle_access(frame, state->source)) {
                  return LazySeqVisitStatus::Faulted;
                }
                predicate = *value;
              }
              const bool truthy = is_truthy(predicate);
              any_match = any_match || truthy;
              all_match = all_match && truthy;
              if ((collection_selector == "any?" && any_match) ||
                  (collection_selector == "all?" && !all_match) ||
                  (collection_selector == "none?" && any_match)) {
                return LazySeqVisitStatus::Stop;
              }
              return LazySeqVisitStatus::Continue;
            });
        if (status == LazySeqVisitStatus::Faulted) {
          return SendStatus::Faulted;
        }
        if (collection_selector == "any?") {
          *out = Value::boolean(any_match);
        } else if (collection_selector == "all?") {
          *out = Value::boolean(!saw_any || all_match);
        } else {
          *out = Value::boolean(!any_match);
        }
        return SendStatus::Matched;
      }

      if (collection_selector == "reduce") {
        if (args.size() > 1U) {
          set_fault(frame, "TypeError", "wrong builtin SEND arity");
          return SendStatus::Faulted;
        }
        if (block.is_null()) {
          set_fault(frame, "TypeError", "reduce requires block");
          return SendStatus::Faulted;
        }
        if (!require_lazy_seq_finite_source(frame, *state,
                                            "reduce all items")) {
          return SendStatus::Faulted;
        }
        bool has_accumulator = !args.empty();
        Value accumulator = has_accumulator ? args[0] : Value::null();
        const LazySeqVisitStatus status = visit_lazy_seq(
            frame, *state, receiver,
            [&](const Value &item) -> LazySeqVisitStatus {
              if (!has_accumulator) {
                accumulator = item;
                has_accumulator = true;
                return LazySeqVisitStatus::Continue;
              }
              const std::optional<Value> value =
                  call_block_to_value(frame, block, {accumulator, item});
              if (!value.has_value()) {
                return LazySeqVisitStatus::Faulted;
              }
              if (!ensure_lifecycle_access(frame, receiver) ||
                  !ensure_lifecycle_access(frame, state->source)) {
                return LazySeqVisitStatus::Faulted;
              }
              accumulator = *value;
              return LazySeqVisitStatus::Continue;
            });
        if (status == LazySeqVisitStatus::Faulted) {
          return SendStatus::Faulted;
        }
        if (!has_accumulator) {
          set_fault(frame, "EmptyCollectionError",
                    "reduce without initial value on empty sequence");
          return SendStatus::Faulted;
        }
        *out = accumulator;
        return SendStatus::Matched;
      }

      if (collection_selector == "take_while") {
        if (!args.empty() || block.is_null()) {
          set_fault(frame, "TypeError", "take_while requires block");
          return SendStatus::Faulted;
        }
        std::vector<Value> taken;
        const LazySeqVisitStatus status = visit_lazy_seq(
            frame, *state, receiver,
            [&](const Value &item) -> LazySeqVisitStatus {
              const std::optional<Value> predicate =
                  call_block_to_value(frame, block, {item});
              if (!predicate.has_value()) {
                return LazySeqVisitStatus::Faulted;
              }
              if (!ensure_lifecycle_access(frame, receiver) ||
                  !ensure_lifecycle_access(frame, state->source)) {
                return LazySeqVisitStatus::Faulted;
              }
              if (!is_truthy(*predicate)) {
                return LazySeqVisitStatus::Stop;
              }
              taken.push_back(item);
              return LazySeqVisitStatus::Continue;
            });
        if (status == LazySeqVisitStatus::Faulted) {
          return SendStatus::Faulted;
        }
        *out = make_list_value(std::move(taken));
        return SendStatus::Matched;
      }

      if (sequence_set_operation_selector ||
          sequence_extra_operation_selector ||
          (collection_selector == "each" &&
           (!args.empty() || !kw_args.empty()))) {
        const std::optional<std::vector<Value>> items =
            materialize_lazy_seq_items(frame, *state, receiver, std::nullopt);
        if (!items.has_value()) {
          return SendStatus::Faulted;
        }
        const std::optional<Value> value = apply_sequence_set_operation(
            frame, receiver, collection_selector, *items, args, block, kw_args);
        if (!value.has_value()) {
          return SendStatus::Faulted;
        }
        *out = *value;
        return SendStatus::Matched;
      }
    }

    if (receiver.is_list() || receiver.is_tuple() || receiver.is_set() ||
        (receiver_is_range && range_collection_selector)) {
      if (receiver_is_range &&
          (collection_selector == "contains?" ||
           collection_selector == "include?" || selector == "===")) {
        if (!require_arity(1) || !require_no_block()) {
          return SendStatus::Faulted;
        }
        bool contains = false;
        if (!range_contains_value(frame, receiver, args[0], &contains)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(contains);
        return SendStatus::Matched;
      }

      if (receiver_is_range) {
        const std::optional<RangeBounds> bounds =
            extract_range_bounds(frame, receiver);
        if (fault_.has_value() || !bounds.has_value()) {
          return SendStatus::Faulted;
        }
        const bool open_ended =
            !bounds->start.has_value() || !bounds->finish.has_value();
        if (open_ended) {
          if (collection_selector == "lazy") {
            if (!require_arity(0) || !require_no_block()) {
              return SendStatus::Faulted;
            }
            *out = make_lazy_seq_value(receiver, {});
            return SendStatus::Matched;
          }
          if (collection_selector == "empty?") {
            if (!require_arity(0) || !require_no_block()) {
              return SendStatus::Faulted;
            }
            *out = Value::boolean(false);
            return SendStatus::Matched;
          }
          if (collection_selector == "first") {
            if (!require_no_block()) {
              return SendStatus::Faulted;
            }
            if (!bounds->start.has_value()) {
              set_fault(frame, "ArgumentError",
                        "beginless Range has no first element");
              return SendStatus::Faulted;
            }
            if (args.empty()) {
              *out = Value::integer(*bounds->start);
              return SendStatus::Matched;
            }
            std::int64_t count = 0;
            if (!require_arity(1) || !require_integer_arg(0, &count)) {
              return SendStatus::Faulted;
            }
            std::vector<Value> taken;
            if (count > 0) {
              taken.reserve(static_cast<std::size_t>(count));
            }
            std::int64_t current = *bounds->start;
            for (std::int64_t index = 0; index < count; ++index) {
              taken.push_back(Value::integer(current));
              if (index + 1 < count &&
                  current == std::numeric_limits<std::int64_t>::max()) {
                set_fault(frame, "ArgumentError",
                          "open-ended Range first(count) overflows Integer");
                return SendStatus::Faulted;
              }
              ++current;
            }
            *out = make_list_value(std::move(taken));
            return SendStatus::Matched;
          }
          if (collection_selector == "[]" && bounds->start.has_value()) {
            std::int64_t index = 0;
            if (!require_arity(1) || !require_no_block() ||
                !require_integer_arg(0, &index)) {
              return SendStatus::Faulted;
            }
            if (index < 0) {
              set_fault(frame, "IndexError", "Range index is out of bounds");
              return SendStatus::Faulted;
            }
            if (*bounds->start >
                std::numeric_limits<std::int64_t>::max() - index) {
              set_fault(frame, "ArgumentError",
                        "open-ended Range index overflows Integer");
              return SendStatus::Faulted;
            }
            *out = Value::integer(*bounds->start + index);
            return SendStatus::Matched;
          }
          if ((collection_selector == "any?" || collection_selector == "all?" ||
               collection_selector == "none?") &&
              block.is_null()) {
            if (!require_arity(0)) {
              return SendStatus::Faulted;
            }
            *out = Value::boolean(collection_selector != "none?");
            return SendStatus::Matched;
          }
          set_fault(frame, "ArgumentError",
                    "open-ended Range cannot run eager collection method");
          return SendStatus::Faulted;
        }
      }

      if (!receiver_is_range) {
        const std::vector<Value> *items_view =
            sequence_items_view(frame, receiver);
        if (fault_.has_value()) {
          return SendStatus::Faulted;
        }
        if (items_view != nullptr) {
          const std::vector<Value> &items = *items_view;
          if (collection_selector == "empty?") {
            if (!require_arity(0) || !require_no_block()) {
              return SendStatus::Faulted;
            }
            *out = Value::boolean(items.empty());
            return SendStatus::Matched;
          }
          if (collection_selector == "[]") {
            std::int64_t index = 0;
            if (!require_arity(1) || !require_no_block() ||
                !require_integer_arg(0, &index)) {
              return SendStatus::Faulted;
            }
            if (index < 0 || static_cast<std::size_t>(index) >= items.size()) {
              set_fault(frame, "IndexError",
                        "collection index is out of bounds");
              return SendStatus::Faulted;
            }
            *out = items[static_cast<std::size_t>(index)];
            return SendStatus::Matched;
          }
          if (collection_selector == "deconstruct") {
            if (!require_arity(0) || !require_no_block()) {
              return SendStatus::Faulted;
            }
            *out = receiver;
            return SendStatus::Matched;
          }
          if (collection_selector == "first") {
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
          if (collection_selector == "count" && block.is_null()) {
            if (!require_arity(0)) {
              return SendStatus::Faulted;
            }
            *out = Value::integer(static_cast<std::int64_t>(items.size()));
            return SendStatus::Matched;
          }
          if (collection_selector == "to_a") {
            if (!require_arity(0) || !require_no_block()) {
              return SendStatus::Faulted;
            }
            *out = make_list_value(items);
            return SendStatus::Matched;
          }
        }
      }

      std::vector<Value> items;
      bool source_was_tuple = false;
      const std::optional<std::vector<Value>> extracted =
          extract_sequence_items(frame, receiver, &source_was_tuple);
      if (fault_.has_value()) {
        return SendStatus::Faulted;
      }
      if (extracted.has_value()) {
        items = *extracted;
        if (collection_selector == "empty?") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          *out = Value::boolean(items.empty());
          return SendStatus::Matched;
        }
        if (collection_selector == "[]") {
          std::int64_t index = 0;
          if (!require_arity(1) || !require_no_block() ||
              !require_integer_arg(0, &index)) {
            return SendStatus::Faulted;
          }
          if (index < 0 || static_cast<std::size_t>(index) >= items.size()) {
            set_fault(frame, "IndexError", "collection index is out of bounds");
            return SendStatus::Faulted;
          }
          *out = items[static_cast<std::size_t>(index)];
          return SendStatus::Matched;
        }
        if (collection_selector == "deconstruct") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          *out = receiver;
          return SendStatus::Matched;
        }
        if (sequence_set_operation_selector ||
            sequence_extra_operation_selector ||
            (collection_selector == "each" &&
             (!args.empty() || !kw_args.empty()))) {
          const std::optional<Value> value =
              apply_sequence_set_operation(frame, receiver, collection_selector,
                                           items, args, block, kw_args);
          if (!value.has_value()) {
            return SendStatus::Faulted;
          }
          *out = *value;
          return SendStatus::Matched;
        }
        if (collection_selector == "first") {
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
        if (collection_selector == "count") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          if (!count_alias_accepts_block && !require_no_block()) {
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
        if (collection_selector == "to_a") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          *out = make_list_value(items);
          return SendStatus::Matched;
        }
        if (collection_selector == "lazy") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          *out = make_lazy_seq_value(receiver, {});
          return SendStatus::Matched;
        }
        if (collection_selector == "each") {
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
        if (collection_selector == "map" || collection_selector == "select" ||
            collection_selector == "reject" ||
            collection_selector == "flat_map" ||
            collection_selector == "find" ||
            collection_selector == "group_by") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          if (collection_selector == "map") {
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
          if (collection_selector == "flat_map") {
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
          if (collection_selector == "find") {
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
          if (collection_selector == "group_by") {
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
            if ((collection_selector == "select" && keep) ||
                (collection_selector == "reject" && !keep)) {
              filtered.push_back(item);
            }
          }
          *out = make_list_value(std::move(filtered));
          return SendStatus::Matched;
        }
        if (collection_selector == "any?" || collection_selector == "all?" ||
            collection_selector == "none?") {
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
          if (collection_selector == "any?") {
            *out = Value::boolean(any_match);
          } else if (collection_selector == "all?") {
            *out = Value::boolean(!saw_any || all_match);
          } else {
            *out = Value::boolean(!any_match);
          }
          return SendStatus::Matched;
        }
        if (collection_selector == "reduce") {
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
        if (collection_selector == "empty?") {
          if (!require_arity(0) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          *out = Value::boolean(extracted->empty());
          return SendStatus::Matched;
        }
        if (collection_selector == "count") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          if (!count_alias_accepts_block && !require_no_block()) {
            return SendStatus::Faulted;
          }
          if (block.is_null()) {
            *out = Value::integer(static_cast<std::int64_t>(extracted->size()));
            return SendStatus::Matched;
          }
          std::int64_t count = 0;
          for (const MapEntry &entry : *extracted) {
            const std::optional<Value> predicate = call_block_to_value(
                frame, block, {Value::symbol(entry.symbol_id), entry.value});
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
        if (collection_selector == "contains?" ||
            collection_selector == "include?") {
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
            *out = Value::boolean(false);
            return SendStatus::Matched;
          }
          const bool found =
              std::find_if(extracted->begin(), extracted->end(),
                           [&](const MapEntry &entry) {
                             return entry.symbol_id == *key_symbol_id;
                           }) != extracted->end();
          *out = Value::boolean(found);
          return SendStatus::Matched;
        }
        if (collection_selector == "value?" ||
            collection_selector == "has_value?") {
          if (!require_arity(1) || !require_no_block()) {
            return SendStatus::Faulted;
          }
          const bool found =
              std::find_if(extracted->begin(), extracted->end(),
                           [&](const MapEntry &entry) {
                             return value_equals(entry.value, args[0]);
                           }) != extracted->end();
          *out = Value::boolean(found);
          return SendStatus::Matched;
        }
        if (collection_selector == "[]") {
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
            set_fault(frame, "KeyError", "map key is absent");
            return SendStatus::Faulted;
          }
          Value found = Value::null();
          bool found_key = false;
          for (const MapEntry &entry : *extracted) {
            if (entry.symbol_id == *key_symbol_id) {
              found = entry.value;
              found_key = true;
              break;
            }
          }
          if (!found_key) {
            set_fault(frame, "KeyError", "map key is absent");
            return SendStatus::Faulted;
          }
          *out = found;
          return SendStatus::Matched;
        }
        if (collection_selector == "deconstruct_keys") {
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
        if (collection_selector == "keys") {
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
        if (collection_selector == "values") {
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
        if (collection_selector == "entries" || collection_selector == "to_a") {
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
        if (collection_selector == "each" ||
            collection_selector == "each_pair") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          if (block.is_null() && collection_selector == "each_pair") {
            std::vector<Value> entries;
            entries.reserve(extracted->size());
            for (const MapEntry &entry : *extracted) {
              entries.push_back(make_tuple_value(
                  {Value::symbol(entry.symbol_id), entry.value}));
            }
            *out = make_list_value(std::move(entries));
            return SendStatus::Matched;
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
        if (collection_selector == "map") {
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
        if (collection_selector == "select" ||
            collection_selector == "reject") {
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
            if ((collection_selector == "select" && keep) ||
                (collection_selector == "reject" && !keep)) {
              filtered.push_back(entry);
            }
          }
          *out = make_symbol_map_value(std::move(filtered));
          return SendStatus::Matched;
        }
        if (collection_selector == "transform") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          std::vector<MapEntry> transformed;
          transformed.reserve(extracted->size());
          for (const MapEntry &entry : *extracted) {
            const std::optional<Value> value = call_block_to_value(
                frame, block, {Value::symbol(entry.symbol_id), entry.value});
            if (!value.has_value()) {
              return SendStatus::Faulted;
            }
            if (!require_receiver_live_after_block()) {
              return SendStatus::Faulted;
            }
            const std::optional<MapEntry> transformed_entry =
                map_entry_from_transform_result(frame, *value);
            if (!transformed_entry.has_value()) {
              return SendStatus::Faulted;
            }
            upsert_map_entry(&transformed, *transformed_entry);
          }
          *out = make_symbol_map_value(std::move(transformed));
          return SendStatus::Matched;
        }
        if (collection_selector == "transform_values") {
          if (!require_arity(0)) {
            return SendStatus::Faulted;
          }
          std::vector<MapEntry> transformed;
          transformed.reserve(extracted->size());
          for (const MapEntry &entry : *extracted) {
            const std::optional<Value> value = call_block_to_value(
                frame, block, {entry.value, Value::symbol(entry.symbol_id)});
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
        if (collection_selector == "merge" || collection_selector == "+" ||
            collection_selector == "|") {
          if (!require_arity(1)) {
            return SendStatus::Faulted;
          }
          const std::optional<Value> merged =
              merge_map_entries(frame, receiver, *extracted, args[0], block);
          if (!merged.has_value()) {
            return SendStatus::Faulted;
          }
          *out = *merged;
          return SendStatus::Matched;
        }
      }
    }

    if (receiver.is_integer()) {
      const std::int64_t lhs = receiver.as_integer();
      std::int64_t rhs = 0;
      double numeric_rhs = 0.0;
      if (selector == "+") {
        if (!require_arity(1) || !require_no_block()) {
          return SendStatus::Faulted;
        }
        if (args[0].is_float()) {
          *out = Value::floating(static_cast<double>(lhs) + args[0].as_float());
          return SendStatus::Matched;
        }
        if (!require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs + rhs);
        return SendStatus::Matched;
      }
      if (selector == "-") {
        if (!require_arity(1) || !require_no_block()) {
          return SendStatus::Faulted;
        }
        if (args[0].is_float()) {
          *out = Value::floating(static_cast<double>(lhs) - args[0].as_float());
          return SendStatus::Matched;
        }
        if (!require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs - rhs);
        return SendStatus::Matched;
      }
      if (selector == "*") {
        if (!require_arity(1) || !require_no_block()) {
          return SendStatus::Faulted;
        }
        if (args[0].is_float()) {
          *out = Value::floating(static_cast<double>(lhs) * args[0].as_float());
          return SendStatus::Matched;
        }
        if (!require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs * rhs);
        return SendStatus::Matched;
      }
      if (selector == "/") {
        if (!require_arity(1) || !require_no_block()) {
          return SendStatus::Faulted;
        }
        if (args[0].is_float()) {
          const double float_rhs = args[0].as_float();
          if (float_rhs == 0.0) {
            set_fault(frame, "TypeError", "division by zero");
            return SendStatus::Faulted;
          }
          *out = Value::floating(static_cast<double>(lhs) / float_rhs);
          return SendStatus::Matched;
        }
        if (!require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        if (rhs == 0) {
          set_fault(frame, "TypeError", "division by zero");
          return SendStatus::Faulted;
        }
        *out = Value::integer(lhs / rhs);
        return SendStatus::Matched;
      }
      if (selector == "%") {
        if (!require_arity(1) || !require_no_block()) {
          return SendStatus::Faulted;
        }
        if (args[0].is_float()) {
          const double float_rhs = args[0].as_float();
          if (float_rhs == 0.0) {
            set_fault(frame, "TypeError", "modulo by zero");
            return SendStatus::Faulted;
          }
          *out = Value::floating(
              floor_mod_double(static_cast<double>(lhs), float_rhs));
          return SendStatus::Matched;
        }
        if (!require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        if (rhs == 0) {
          set_fault(frame, "TypeError", "modulo by zero");
          return SendStatus::Faulted;
        }
        *out = Value::integer(floor_mod_int64(lhs, rhs));
        return SendStatus::Matched;
      }
      if (selector == "//") {
        if (!require_arity(1) || !require_no_block()) {
          return SendStatus::Faulted;
        }
        if (args[0].is_float()) {
          const double float_rhs = args[0].as_float();
          if (float_rhs == 0.0) {
            set_fault(frame, "TypeError", "division by zero");
            return SendStatus::Faulted;
          }
          *out =
              Value::floating(std::floor(static_cast<double>(lhs) / float_rhs));
          return SendStatus::Matched;
        }
        if (!require_integer_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        if (rhs == 0) {
          set_fault(frame, "TypeError", "division by zero");
          return SendStatus::Faulted;
        }
        *out = Value::integer(floor_div_int64(lhs, rhs));
        return SendStatus::Matched;
      }
      if (selector == ">") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &numeric_rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(static_cast<double>(lhs) > numeric_rhs);
        return SendStatus::Matched;
      }
      if (selector == "<") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &numeric_rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(static_cast<double>(lhs) < numeric_rhs);
        return SendStatus::Matched;
      }
      if (selector == ">=") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &numeric_rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(static_cast<double>(lhs) >= numeric_rhs);
        return SendStatus::Matched;
      }
      if (selector == "<=") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &numeric_rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(static_cast<double>(lhs) <= numeric_rhs);
        return SendStatus::Matched;
      }
      if (selector == "<=>") {
        if (!require_arity(1) || !require_no_block()) {
          return SendStatus::Faulted;
        }
        if (args[0].is_integer()) {
          *out = Value::integer(compare_int64(lhs, args[0].as_integer()));
          return SendStatus::Matched;
        }
        if (!require_numeric_arg(0, &numeric_rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(
            compare_double(static_cast<double>(lhs), numeric_rhs));
        return SendStatus::Matched;
      }
    }

    if (receiver.is_float()) {
      const double lhs = receiver.as_float();
      double rhs = 0.0;
      if (selector == "+") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::floating(lhs + rhs);
        return SendStatus::Matched;
      }
      if (selector == "-") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::floating(lhs - rhs);
        return SendStatus::Matched;
      }
      if (selector == "*") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::floating(lhs * rhs);
        return SendStatus::Matched;
      }
      if (selector == "/") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        if (rhs == 0.0) {
          set_fault(frame, "TypeError", "division by zero");
          return SendStatus::Faulted;
        }
        *out = Value::floating(lhs / rhs);
        return SendStatus::Matched;
      }
      if (selector == "%") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        if (rhs == 0.0) {
          set_fault(frame, "TypeError", "modulo by zero");
          return SendStatus::Faulted;
        }
        *out = Value::floating(floor_mod_double(lhs, rhs));
        return SendStatus::Matched;
      }
      if (selector == "//") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        if (rhs == 0.0) {
          set_fault(frame, "TypeError", "division by zero");
          return SendStatus::Faulted;
        }
        *out = Value::floating(std::floor(lhs / rhs));
        return SendStatus::Matched;
      }
      if (selector == ">") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs > rhs);
        return SendStatus::Matched;
      }
      if (selector == "<") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs < rhs);
        return SendStatus::Matched;
      }
      if (selector == ">=") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs >= rhs);
        return SendStatus::Matched;
      }
      if (selector == "<=") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::boolean(lhs <= rhs);
        return SendStatus::Matched;
      }
      if (selector == "<=>") {
        if (!require_arity(1) || !require_no_block() ||
            !require_numeric_arg(0, &rhs)) {
          return SendStatus::Faulted;
        }
        *out = Value::integer(compare_double(lhs, rhs));
        return SendStatus::Matched;
      }
    }

    return SendStatus::NotHandled;
  }

  FastSendStatus step_fast_static_send(Frame &frame, const Instruction &insn,
                                       std::uint32_t dst,
                                       std::uint32_t recv_reg,
                                       const std::string &selector) {
    const std::string collection_selector =
        canonical_collection_selector(selector);
    const bool collection_fast_selector =
        collection_selector == "[]" || collection_selector == "count" ||
        collection_selector == "first" || collection_selector == "empty?" ||
        collection_selector == "deconstruct" || collection_selector == "to_a";
    const bool integer_selector =
        selector == "+" || selector == "-" || selector == "*" ||
        selector == "/" || selector == "%" || selector == "//" ||
        selector == ">" || selector == "<" || selector == ">=" ||
        selector == "<=" || selector == "==" || selector == "!=" ||
        selector == "<=>";
    if (!collection_fast_selector && !integer_selector) {
      return FastSendStatus::NotHandled;
    }

    std::uint32_t pos_count = 0;
    if (!operand_u32(frame, insn, 3, &pos_count)) {
      return FastSendStatus::Faulted;
    }
    if (pos_count > 1U) {
      return FastSendStatus::NotHandled;
    }

    std::uint32_t arg_reg = 0;
    std::size_t operand_index = 4;
    if (pos_count == 1U &&
        !operand_u32(frame, insn, operand_index++, &arg_reg)) {
      return FastSendStatus::Faulted;
    }

    std::uint32_t kw_count = 0;
    if (!operand_u32(frame, insn, operand_index++, &kw_count)) {
      return FastSendStatus::Faulted;
    }
    if (kw_count != 0U) {
      return FastSendStatus::NotHandled;
    }

    std::int64_t block_reg = -1;
    if (!operand_i64(frame, insn, operand_index++, &block_reg)) {
      return FastSendStatus::Faulted;
    }
    if (has_optional_reg(block_reg)) {
      return FastSendStatus::NotHandled;
    }

    const std::optional<std::uint32_t> site_id =
        optional_operand_u32(frame, insn, operand_index++);
    if (fault_.has_value()) {
      return FastSendStatus::Faulted;
    }
    const std::uint32_t site_flags =
        site_id.has_value() ? call_site_flags(frame, *site_id) : 0U;
    if ((site_flags & (bytecode::kCallSiteFlagPropertyAccess |
                       bytecode::kCallSiteFlagPropertyAssignment)) != 0U) {
      return FastSendStatus::NotHandled;
    }

    if (integer_selector) {
      if (pos_count != 1U) {
        return FastSendStatus::NotHandled;
      }
      std::int64_t lhs = 0;
      std::int64_t rhs = 0;
      const bool rhs_fast = read_integer_reg_unboxed(frame, arg_reg, &rhs);
      if (fault_.has_value()) {
        return FastSendStatus::Faulted;
      }
      if (!rhs_fast) {
        return FastSendStatus::NotHandled;
      }
      const bool lhs_fast = read_integer_reg_unboxed(frame, recv_reg, &lhs);
      if (fault_.has_value()) {
        return FastSendStatus::Faulted;
      }
      if (!lhs_fast || !rhs_fast) {
        return FastSendStatus::NotHandled;
      }
      if (selector == "+") {
        return write_integer_reg_unboxed(frame, dst, lhs + rhs)
                   ? FastSendStatus::Matched
                   : FastSendStatus::Faulted;
      }
      if (selector == "-") {
        return write_integer_reg_unboxed(frame, dst, lhs - rhs)
                   ? FastSendStatus::Matched
                   : FastSendStatus::Faulted;
      }
      if (selector == "*") {
        return write_integer_reg_unboxed(frame, dst, lhs * rhs)
                   ? FastSendStatus::Matched
                   : FastSendStatus::Faulted;
      }
      if (selector == "/") {
        if (rhs == 0) {
          set_fault(frame, "TypeError", "division by zero");
          return FastSendStatus::Faulted;
        }
        return write_integer_reg_unboxed(frame, dst, lhs / rhs)
                   ? FastSendStatus::Matched
                   : FastSendStatus::Faulted;
      }
      if (selector == "%") {
        if (rhs == 0) {
          set_fault(frame, "TypeError", "modulo by zero");
          return FastSendStatus::Faulted;
        }
        return write_integer_reg_unboxed(frame, dst, floor_mod_int64(lhs, rhs))
                   ? FastSendStatus::Matched
                   : FastSendStatus::Faulted;
      }
      if (selector == "//") {
        if (rhs == 0) {
          set_fault(frame, "TypeError", "division by zero");
          return FastSendStatus::Faulted;
        }
        return write_integer_reg_unboxed(frame, dst, floor_div_int64(lhs, rhs))
                   ? FastSendStatus::Matched
                   : FastSendStatus::Faulted;
      }
      if (selector == "<=>") {
        return write_integer_reg_unboxed(frame, dst, compare_int64(lhs, rhs))
                   ? FastSendStatus::Matched
                   : FastSendStatus::Faulted;
      }

      bool result = false;
      if (selector == ">") {
        result = lhs > rhs;
      } else if (selector == "<") {
        result = lhs < rhs;
      } else if (selector == ">=") {
        result = lhs >= rhs;
      } else if (selector == "<=") {
        result = lhs <= rhs;
      } else if (selector == "==") {
        result = lhs == rhs;
      } else {
        result = lhs != rhs;
      }
      return write_reg_fast_plain(frame, dst, Value::boolean(result))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    }

    std::int64_t single_integer_arg = 0;
    if (collection_selector == "empty?" || collection_selector == "count" ||
        collection_selector == "deconstruct" || collection_selector == "to_a") {
      if (pos_count != 0U) {
        return FastSendStatus::NotHandled;
      }
    } else if (collection_selector == "[]") {
      if (pos_count != 1U) {
        return FastSendStatus::NotHandled;
      }
      const bool index_fast =
          read_integer_reg_unboxed(frame, arg_reg, &single_integer_arg);
      if (fault_.has_value()) {
        return FastSendStatus::Faulted;
      }
      if (!index_fast) {
        return FastSendStatus::NotHandled;
      }
    } else if (collection_selector == "first" && pos_count == 1U) {
      const bool count_fast =
          read_integer_reg_unboxed(frame, arg_reg, &single_integer_arg);
      if (fault_.has_value()) {
        return FastSendStatus::Faulted;
      }
      if (!count_fast) {
        return FastSendStatus::NotHandled;
      }
    }

    const Value receiver = read_reg(frame, recv_reg);
    if (fault_.has_value()) {
      return FastSendStatus::Faulted;
    }
    const std::vector<Value> *items = sequence_items_view(frame, receiver);
    if (fault_.has_value()) {
      return FastSendStatus::Faulted;
    }
    if (items == nullptr) {
      return FastSendStatus::NotHandled;
    }

    if (collection_selector == "empty?") {
      return write_reg_fast_plain(frame, dst, Value::boolean(items->empty()))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    }
    if (collection_selector == "count") {
      return write_integer_reg_unboxed(frame, dst,
                                       static_cast<std::int64_t>(items->size()))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    }
    if (collection_selector == "deconstruct") {
      return write_reg_fast_plain(frame, dst, receiver)
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    }
    if (collection_selector == "to_a") {
      return write_reg_fast_plain(frame, dst, make_list_value(*items))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    }
    if (collection_selector == "[]") {
      if (single_integer_arg < 0 ||
          static_cast<std::size_t>(single_integer_arg) >= items->size()) {
        set_fault(frame, "IndexError", "collection index is out of bounds");
        return FastSendStatus::Faulted;
      }
      return write_reg_fast_plain(
                 frame, dst,
                 (*items)[static_cast<std::size_t>(single_integer_arg)])
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    }
    if (collection_selector == "first") {
      if (pos_count == 0U) {
        return write_reg_fast_plain(
                   frame, dst, items->empty() ? Value::null() : items->front())
                   ? FastSendStatus::Matched
                   : FastSendStatus::Faulted;
      }
      const std::size_t take =
          single_integer_arg <= 0
              ? 0U
              : std::min<std::size_t>(
                    static_cast<std::size_t>(single_integer_arg),
                    items->size());
      return write_reg_fast_plain(frame, dst,
                                  make_list_value(std::vector<Value>(
                                      items->begin(), items->begin() + take)))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    }

    return FastSendStatus::NotHandled;
  }

  FastSendStatus step_quick_integer_send(Frame &frame, QuickOpcode opcode,
                                         std::uint32_t dst,
                                         std::uint32_t recv_reg,
                                         std::uint32_t arg_reg) {
    std::int64_t lhs = 0;
    std::int64_t rhs = 0;
    const bool rhs_fast = read_integer_reg_unboxed(frame, arg_reg, &rhs);
    if (fault_.has_value()) {
      return FastSendStatus::Faulted;
    }
    if (!rhs_fast) {
      return FastSendStatus::NotHandled;
    }
    const bool lhs_fast = read_integer_reg_unboxed(frame, recv_reg, &lhs);
    if (fault_.has_value()) {
      return FastSendStatus::Faulted;
    }
    if (!lhs_fast) {
      return FastSendStatus::NotHandled;
    }

    switch (opcode) {
    case QuickOpcode::SendIAdd:
      return write_integer_reg_unboxed(frame, dst, lhs + rhs)
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendISub:
      return write_integer_reg_unboxed(frame, dst, lhs - rhs)
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendIMul:
      return write_integer_reg_unboxed(frame, dst, lhs * rhs)
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendIDiv:
      if (rhs == 0) {
        set_fault(frame, "TypeError", "division by zero");
        return FastSendStatus::Faulted;
      }
      return write_integer_reg_unboxed(frame, dst, lhs / rhs)
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendIMod:
      if (rhs == 0) {
        set_fault(frame, "TypeError", "modulo by zero");
        return FastSendStatus::Faulted;
      }
      return write_integer_reg_unboxed(frame, dst, floor_mod_int64(lhs, rhs))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendIFloorDiv:
      if (rhs == 0) {
        set_fault(frame, "TypeError", "division by zero");
        return FastSendStatus::Faulted;
      }
      return write_integer_reg_unboxed(frame, dst, floor_div_int64(lhs, rhs))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendILt:
      return write_reg_fast_plain(frame, dst, Value::boolean(lhs < rhs))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendIGt:
      return write_reg_fast_plain(frame, dst, Value::boolean(lhs > rhs))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendILe:
      return write_reg_fast_plain(frame, dst, Value::boolean(lhs <= rhs))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendIGe:
      return write_reg_fast_plain(frame, dst, Value::boolean(lhs >= rhs))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendIEq:
      return write_reg_fast_plain(frame, dst, Value::boolean(lhs == rhs))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendINe:
      return write_reg_fast_plain(frame, dst, Value::boolean(lhs != rhs))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendICmp:
      return write_integer_reg_unboxed(frame, dst, compare_int64(lhs, rhs))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    default:
      return FastSendStatus::NotHandled;
    }
  }

  FastSendStatus step_quick_sequence_send(Frame &frame, QuickOpcode opcode,
                                          std::uint32_t dst,
                                          std::uint32_t recv_reg,
                                          std::uint32_t arg_reg,
                                          std::int64_t pos_count) {
    std::int64_t integer_arg = 0;
    if (opcode == QuickOpcode::SendSeqIndex ||
        (opcode == QuickOpcode::SendSeqFirst && pos_count == 1)) {
      const bool arg_fast =
          read_integer_reg_unboxed(frame, arg_reg, &integer_arg);
      if (fault_.has_value()) {
        return FastSendStatus::Faulted;
      }
      if (!arg_fast) {
        return FastSendStatus::NotHandled;
      }
    }

    const Value receiver = read_reg(frame, recv_reg);
    if (fault_.has_value()) {
      return FastSendStatus::Faulted;
    }
    const std::vector<Value> *items = sequence_items_view(frame, receiver);
    if (fault_.has_value()) {
      return FastSendStatus::Faulted;
    }
    if (items == nullptr) {
      return FastSendStatus::NotHandled;
    }

    switch (opcode) {
    case QuickOpcode::SendSeqIndex:
      if (integer_arg < 0 ||
          static_cast<std::size_t>(integer_arg) >= items->size()) {
        set_fault(frame, "IndexError", "collection index is out of bounds");
        return FastSendStatus::Faulted;
      }
      return write_reg_fast_plain(
                 frame, dst, (*items)[static_cast<std::size_t>(integer_arg)])
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendSeqCount:
      return write_integer_reg_unboxed(frame, dst,
                                       static_cast<std::int64_t>(items->size()))
                 ? FastSendStatus::Matched
                 : FastSendStatus::Faulted;
    case QuickOpcode::SendSeqFirst:
      if (pos_count == 0) {
        return write_reg_fast_plain(
                   frame, dst, items->empty() ? Value::null() : items->front())
                   ? FastSendStatus::Matched
                   : FastSendStatus::Faulted;
      }
      if (pos_count == 1) {
        const std::size_t take =
            integer_arg <= 0
                ? 0U
                : std::min<std::size_t>(static_cast<std::size_t>(integer_arg),
                                        items->size());
        return write_reg_fast_plain(frame, dst,
                                    make_list_value(std::vector<Value>(
                                        items->begin(), items->begin() + take)))
                   ? FastSendStatus::Matched
                   : FastSendStatus::Faulted;
      }
      return FastSendStatus::NotHandled;
    default:
      return FastSendStatus::NotHandled;
    }
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
      if (selector_id >= module_.symbols.size()) {
        set_fault(frame, "VMError", "selector symbol ref is out of range");
        return false;
      }
      const std::string &static_selector = module_.symbols[selector_id];
      const FastSendStatus fast_status =
          step_fast_static_send(frame, insn, dst, recv_reg, static_selector);
      if (fast_status == FastSendStatus::Faulted) {
        return false;
      }
      if (fast_status == FastSendStatus::Matched) {
        ++frame.pc;
        return true;
      }
      selector_value = Value::symbol(selector_id);
      selector_symbol_id_for_cache = selector_id;
      selector = static_selector;
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
    const std::uint32_t site_flags =
        site_id.has_value() ? call_site_flags(frame, *site_id) : 0U;
    const bool property_access =
        (site_flags & bytecode::kCallSiteFlagPropertyAccess) != 0U;
    const bool property_assignment =
        (site_flags & bytecode::kCallSiteFlagPropertyAssignment) != 0U;
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
    if (property_access && property_assignment) {
      set_fault(frame, "VMError",
                "send cannot be both property access and assignment");
      return false;
    }
    if (!property_access && !property_assignment && *selector == "destroy!") {
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
    if (!property_access && !property_assignment) {
      Value result = Value::null();
      const SendStatus scalar_status = try_apply_scalar_send(
          frame, receiver, *selector, args, block, kw_args, &result);
      if (scalar_status == SendStatus::Faulted) {
        return false;
      }
      if (scalar_status == SendStatus::Matched) {
        if (!write_reg(frame, dst, std::move(result))) {
          return false;
        }
        ++frame.pc;
        return true;
      }
    }
    if (property_access) {
      if (!args.empty() || !kw_args.empty() || !block.is_null()) {
        set_fault(frame, "TypeError",
                  "property access does not accept arguments or block");
        return false;
      }
      if (receiver.is_native_type()) {
        Value result = Value::null();
        const SendStatus scalar_status = try_apply_scalar_send(
            frame, receiver, *selector, args, block, kw_args, &result);
        if (scalar_status == SendStatus::Faulted) {
          return false;
        }
        if (scalar_status == SendStatus::Matched) {
          if (!write_reg(frame, dst, std::move(result))) {
            return false;
          }
          ++frame.pc;
          return true;
        }
      }
      if (collection_size_selector(*selector)) {
        Value result = Value::null();
        const SendStatus scalar_status = try_apply_scalar_send(
            frame, receiver, *selector, args, block, kw_args, &result);
        if (scalar_status == SendStatus::Faulted) {
          return false;
        }
        if (scalar_status == SendStatus::Matched) {
          if (!write_reg(frame, dst, std::move(result))) {
            return false;
          }
          ++frame.pc;
          return true;
        }
      }
      if (const std::optional<RuntimeNativeTypeKind> target =
              conversion_target_for_alias(*selector)) {
        if (!ensure_lifecycle_access(frame, receiver)) {
          return false;
        }
        ConversionResult converted =
            convert_value_to_native_type(frame, receiver, *target);
        if (!converted.ok) {
          set_fault(frame, converted.error_name, converted.message);
          return false;
        }
        if (!write_reg(frame, dst, std::move(converted.value))) {
          return false;
        }
        ++frame.pc;
        return true;
      }
    }
    if (property_assignment) {
      if (args.size() != 1U || !kw_args.empty() || !block.is_null()) {
        set_fault(frame, "TypeError",
                  "property assignment requires exactly one value and no "
                  "keywords or block");
        return false;
      }
    }

    auto invoke_property_method = [&](const bytecode::BcMethod &getter) {
      if ((getter.flags & kMethodFlagPropertyGetter) == 0U) {
        set_fault(frame, "NoMethodError",
                  "bare member access requires a property getter");
        return false;
      }
      return invoke_method(frame, getter, {}, {}, receiver, Value::null(), dst);
    };

    auto invoke_property_result =
        [&](const bytecode::BcMethod &getter) -> bool {
      const std::vector<Value> no_pos_args;
      const std::vector<std::pair<std::uint32_t, Value>> no_kw_args;
      const std::optional<Value> getter_value = execute_method_to_value(
          frame, getter, no_pos_args, no_kw_args, receiver, Value::null());
      if (!getter_value.has_value()) {
        return false;
      }
      return invoke_callable_value(frame, *getter_value, args, kw_args, block,
                                   dst);
    };

    auto invoke_property_setter =
        [&](const bytecode::BcMethod &setter) -> bool {
      if ((setter.flags & kMethodFlagPropertySetter) == 0U) {
        set_fault(frame, "NoMethodError",
                  "assignment requires a property setter");
        return false;
      }
      const std::vector<std::pair<std::uint32_t, Value>> no_kw_args;
      const std::optional<Value> ignored = execute_method_to_value(
          frame, setter, args, no_kw_args, receiver, Value::null());
      if (!ignored.has_value()) {
        return false;
      }
      if (!write_reg(frame, dst, args.front())) {
        return false;
      }
      ++frame.pc;
      return true;
    };

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
        if (property_assignment) {
          return invoke_property_setter(*cached);
        }
        if (property_access) {
          return invoke_property_method(*cached);
        }
        if ((cached->flags & kMethodFlagPropertyGetter) != 0U) {
          return invoke_property_result(*cached);
        }
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
      if (property_assignment) {
        set_fault(frame, "NoMethodError",
                  "property setter is not implemented in current runtime "
                  "baseline");
        return false;
      }
      if (property_access) {
        set_fault(frame, "NoMethodError",
                  "property is not implemented in current runtime baseline");
        return false;
      }
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
    if (property_assignment) {
      if (site_id.has_value() && selector_symbol_id_for_cache.has_value() &&
          (method->flags & kMethodFlagPropertySetter) != 0U) {
        update_call_cache(frame, *site_id, class_index, dispatch_flags,
                          *selector_symbol_id_for_cache, 1, {}, Value::null(),
                          *method);
      }
      return invoke_property_setter(*method);
    }
    if (property_access) {
      if (site_id.has_value() && selector_symbol_id_for_cache.has_value() &&
          (method->flags & kMethodFlagPropertyGetter) != 0U) {
        update_call_cache(frame, *site_id, class_index, dispatch_flags,
                          *selector_symbol_id_for_cache, 0, {}, Value::null(),
                          *method);
      }
      return invoke_property_method(*method);
    }
    if ((method->flags & kMethodFlagPropertyGetter) != 0U) {
      return invoke_property_result(*method);
    }
    if ((method->flags & kMethodFlagPropertySetter) != 0U) {
      set_fault(frame, "NoMethodError",
                "property setter requires assignment syntax");
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
    const QuickInsn *quick = nullptr;
    if (frame.quick_code != nullptr &&
        frame.pc < frame.quick_code->instructions.size()) {
      quick = &frame.quick_code->instructions[frame.pc];
    }
    if (!is_pattern_opcode(insn.opcode) && has_active_pattern_state(frame)) {
      if (!finalize_pattern_success(frame, false)) {
        return;
      }
      if (frame.pc >= frame.code->instructions.size()) {
        set_fault(frame, "VMError", "program counter out of range");
        return;
      }
    }
    if (quick != nullptr) {
      switch (quick->quick_opcode) {
      case QuickOpcode::Fallback:
        break;
      case QuickOpcode::LoadK: {
        if (quick->b >= module_.const_pool.size()) {
          set_fault(frame, "VMError", "constant ref out of range");
          return;
        }
        const Constant &constant = module_.const_pool[quick->b];
        if (constant.kind == ConstantKind::Integer) {
          if (!write_integer_reg_unboxed(frame, quick->a, constant.int_value)) {
            return;
          }
        } else if (!write_reg(frame, quick->a,
                              load_constant(frame, quick->b))) {
          return;
        }
        ++frame.pc;
        return;
      }
      case QuickOpcode::LoadNull:
        if (!write_reg(frame, quick->a, Value::null())) {
          return;
        }
        ++frame.pc;
        return;
      case QuickOpcode::LoadBool:
        if (!write_reg(frame, quick->a, Value::boolean(quick->imm != 0))) {
          return;
        }
        ++frame.pc;
        return;
      case QuickOpcode::Move:
        if (!write_reg(frame, quick->a, read_reg(frame, quick->b))) {
          return;
        }
        ++frame.pc;
        return;
      case QuickOpcode::LoadSelf:
        if (!write_reg(frame, quick->a, frame.self)) {
          return;
        }
        ++frame.pc;
        return;
      case QuickOpcode::GetLast:
        if (!write_reg(frame, quick->a, frame.last_result)) {
          return;
        }
        ++frame.pc;
        return;
      case QuickOpcode::SetLast:
        frame.last_result = read_reg(frame, quick->a);
        if (fault_.has_value()) {
          return;
        }
        ++frame.pc;
        return;
      case QuickOpcode::LoadUpval:
        if (quick->b >= frame.captures.size()) {
          set_fault(frame, "VMError", "capture slot out of range");
          return;
        }
        if (!write_reg(frame, quick->a,
                       unwrap_watch_value_for_read(frame.captures[quick->b]))) {
          return;
        }
        ++frame.pc;
        return;
      case QuickOpcode::StoreUpval: {
        if (quick->a >= frame.captures.size()) {
          set_fault(frame, "VMError", "capture slot out of range");
          return;
        }
        const Value value = read_reg(frame, quick->b);
        if (fault_.has_value()) {
          return;
        }
        if (frame.captures[quick->a].is_watch_cell()) {
          const std::shared_ptr<RuntimeWatchCell> cell =
              frame.captures[quick->a].as_watch_cell();
          if (cell != nullptr) {
            const RuntimeWatchWriteResult write = cell->write(value);
            record_watch_write(cell, write);
          }
        } else {
          frame.captures[quick->a] = value;
        }
        ++frame.pc;
        return;
      }
      case QuickOpcode::IAdd:
      case QuickOpcode::ISub:
      case QuickOpcode::ILt:
      case QuickOpcode::IGt:
      case QuickOpcode::IMul:
      case QuickOpcode::IDiv:
      case QuickOpcode::IMod:
      case QuickOpcode::IFloorDiv:
      case QuickOpcode::ILe:
      case QuickOpcode::IGe:
      case QuickOpcode::IEq:
      case QuickOpcode::INe:
      case QuickOpcode::ICmp:
        step_integer_binary(frame, *quick, false);
        return;
      case QuickOpcode::IAddK:
      case QuickOpcode::ISubK:
      case QuickOpcode::ILtK:
      case QuickOpcode::IGtK:
      case QuickOpcode::IMulK:
      case QuickOpcode::IDivK:
      case QuickOpcode::IModK:
      case QuickOpcode::IFloorDivK:
      case QuickOpcode::ILeK:
      case QuickOpcode::IGeK:
      case QuickOpcode::IEqK:
      case QuickOpcode::INeK:
      case QuickOpcode::ICmpK:
        step_integer_binary(frame, *quick, true);
        return;
      case QuickOpcode::ILtJumpIfFalse:
      case QuickOpcode::IGtJumpIfFalse:
        step_compare_jump_if_false(frame, *quick, false);
        return;
      case QuickOpcode::ILtKJumpIfFalse:
      case QuickOpcode::IGtKJumpIfFalse:
        step_compare_jump_if_false(frame, *quick, true);
        return;
      case QuickOpcode::SendIAdd:
      case QuickOpcode::SendISub:
      case QuickOpcode::SendIMul:
      case QuickOpcode::SendIDiv:
      case QuickOpcode::SendIMod:
      case QuickOpcode::SendIFloorDiv:
      case QuickOpcode::SendILt:
      case QuickOpcode::SendIGt:
      case QuickOpcode::SendILe:
      case QuickOpcode::SendIGe:
      case QuickOpcode::SendIEq:
      case QuickOpcode::SendINe:
      case QuickOpcode::SendICmp: {
        const FastSendStatus status = step_quick_integer_send(
            frame, quick->quick_opcode, quick->a, quick->b, quick->c);
        if (status == FastSendStatus::Faulted) {
          return;
        }
        if (status == FastSendStatus::Matched) {
          ++frame.pc;
          return;
        }
        break;
      }
      case QuickOpcode::SendSeqIndex:
      case QuickOpcode::SendSeqCount:
      case QuickOpcode::SendSeqFirst: {
        const FastSendStatus status =
            step_quick_sequence_send(frame, quick->quick_opcode, quick->a,
                                     quick->b, quick->c, quick->imm);
        if (status == FastSendStatus::Faulted) {
          return;
        }
        if (status == FastSendStatus::Matched) {
          ++frame.pc;
          return;
        }
        break;
      }
      case QuickOpcode::Jump:
        if (quick->a >= frame.code->instructions.size()) {
          set_fault(frame, "VMError", "jump target out of range");
          return;
        }
        frame.pc = quick->a;
        return;
      case QuickOpcode::JumpIfTrue:
      case QuickOpcode::JumpIfFalse:
      case QuickOpcode::JumpIfNull: {
        if (quick->b >= frame.code->instructions.size()) {
          set_fault(frame, "VMError", "jump target out of range");
          return;
        }
        const Value cond = read_reg(frame, quick->a);
        if (fault_.has_value()) {
          return;
        }
        const bool take = quick->quick_opcode == QuickOpcode::JumpIfTrue
                              ? is_truthy(cond)
                              : (quick->quick_opcode == QuickOpcode::JumpIfFalse
                                     ? !is_truthy(cond)
                                     : cond.is_null());
        frame.pc = take ? quick->b : frame.pc + 1U;
        return;
      }
      case QuickOpcode::Return: {
        Value value = read_reg(frame, quick->a);
        if (fault_.has_value()) {
          return;
        }
        if (frame.return_override.has_value()) {
          value = *frame.return_override;
        }
        const bool capture_completed_frame =
            frames_.size() == 1U ||
            (frame.code != nullptr && frame.code->kind == CodeKind::Module);
        std::vector<Value> completed_regs;
        std::vector<std::uint8_t> completed_initialized;
        if (capture_completed_frame) {
          materialize_integer_regs(frame);
          completed_regs = frame.regs;
          completed_initialized = frame.initialized;
        }
        const BcCode *completed_code = frame.code;
        const std::optional<std::uint32_t> caller_reg = frame.caller_result_reg;
        Frame completed_frame = std::move(frames_.back());
        frames_.pop_back();
        if (capture_completed_frame) {
          last_completed_regs_ = std::move(completed_regs);
          last_completed_initialized_ = std::move(completed_initialized);
          if (completed_code != nullptr) {
            persist_module_bindings(*completed_code, last_completed_regs_,
                                    last_completed_initialized_);
          }
        }
        if (frames_.empty()) {
          final_value_ = value;
          recycle_frame(std::move(completed_frame));
          return;
        }
        Frame &caller = frames_.back();
        caller.active_call_pc.reset();
        if (!caller_reg.has_value() || !write_reg(caller, *caller_reg, value)) {
          recycle_frame(std::move(completed_frame));
          return;
        }
        recycle_frame(std::move(completed_frame));
        return;
      }
      case QuickOpcode::Raise: {
        const Value exception = read_reg(frame, quick->a);
        if (fault_.has_value()) {
          return;
        }
        raise_value(frame, exception);
        return;
      }
      case QuickOpcode::CloseUpvalues:
        ++frame.pc;
        return;
      case QuickOpcode::Safepoint:
        run_safepoint();
        ++frame.pc;
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
      if (const_id >= module_.const_pool.size()) {
        set_fault(frame, "VMError", "constant ref out of range");
        return;
      }
      const Constant &constant = module_.const_pool[const_id];
      if (constant.kind == ConstantKind::Integer) {
        if (!write_integer_reg_unboxed(frame, dst, constant.int_value)) {
          return;
        }
      } else if (!write_reg(frame, dst, load_constant(frame, const_id))) {
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
    case Opcode::MakeSet:
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
                              : (insn.opcode == Opcode::MakeSet
                                     ? make_set_value(std::move(items))
                                     : make_list_value(std::move(items)));
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
        Value value = read_reg(frame, reg);
        if (fault_.has_value()) {
          return;
        }
        const auto existing = std::find_if(
            entries.begin(), entries.end(), [symbol_id](const MapEntry &entry) {
              return entry.symbol_id == symbol_id;
            });
        if (existing == entries.end()) {
          entries.push_back({symbol_id, std::move(value)});
        } else {
          existing->value = std::move(value);
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
      } else if (value.is_set()) {
        const std::shared_ptr<SetValue> set = value.as_set();
        if (set == nullptr) {
          set_fault(frame, "TypeError", "set value is null");
          return;
        }
        if (!ensure_lifecycle_access(frame, value)) {
          return;
        }
        if (!write_reg(frame, dst, make_set_value(set->items, true))) {
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
      const std::optional<std::string> ivar_name =
          selector_text_from_symbol(symbol_id);
      if (!ivar_name.has_value()) {
        set_fault(frame, "VMError", "ivar symbol ref is out of range");
        return;
      }
      std::optional<std::uint32_t> slot =
          probe_ivar_cache(frame, site_id, *instance, symbol_id);
      if (!slot.has_value()) {
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
      if (!write_reg(frame, dst,
                     record_watch_ivar_read(instance, *ivar_name, value))) {
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
      if (!write_reg(frame, dst,
                     unwrap_watch_value_for_read(frame.captures[slot]))) {
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
      const Value value = read_reg(frame, src);
      if (fault_.has_value()) {
        return;
      }
      if (frame.captures[slot].is_watch_cell()) {
        const std::shared_ptr<RuntimeWatchCell> cell =
            frame.captures[slot].as_watch_cell();
        if (cell != nullptr) {
          const RuntimeWatchWriteResult write = cell->write(value);
          record_watch_write(cell, write);
        }
      } else {
        frame.captures[slot] = value;
      }
      ++frame.pc;
      return;
    }
    case Opcode::WatchLocal: {
      std::uint32_t dst = 0;
      std::uint32_t slot = 0;
      std::uint32_t flags = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &slot) ||
          !operand_u32(frame, insn, 2, &flags)) {
        return;
      }
      (void)flags;
      std::shared_ptr<RuntimeWatchCell> cell =
          ensure_local_storage_cell(frame, slot);
      if (cell == nullptr) {
        return;
      }
      const std::string target_name =
          frame.code == nullptr ? "l" + std::to_string(slot)
                                : local_name_for_slot(*frame.code, slot);
      std::shared_ptr<RuntimeWatchHandle> handle =
          watch_cell(cell, target_name);
      if (handle == nullptr ||
          !write_reg(frame, dst, Value::watch_handle(handle))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::WatchUpval: {
      std::uint32_t dst = 0;
      std::uint32_t slot = 0;
      std::uint32_t flags = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &slot) ||
          !operand_u32(frame, insn, 2, &flags)) {
        return;
      }
      (void)flags;
      std::shared_ptr<RuntimeWatchCell> cell =
          ensure_capture_storage_cell(frame, slot);
      if (cell == nullptr) {
        return;
      }
      const std::string target_name =
          frame.code == nullptr ? "u" + std::to_string(slot)
                                : capture_name_for_slot(*frame.code, slot);
      std::shared_ptr<RuntimeWatchHandle> handle =
          watch_cell(cell, target_name);
      if (handle == nullptr ||
          !write_reg(frame, dst, Value::watch_handle(handle))) {
        return;
      }
      ++frame.pc;
      return;
    }
    case Opcode::WatchIvar: {
      std::uint32_t dst = 0;
      std::uint32_t recv_reg = 0;
      std::uint32_t symbol_id = 0;
      std::uint32_t site_id = 0;
      std::uint32_t flags = 0;
      if (!operand_u32(frame, insn, 0, &dst) ||
          !operand_u32(frame, insn, 1, &recv_reg) ||
          !operand_u32(frame, insn, 2, &symbol_id) ||
          !operand_u32(frame, insn, 3, &site_id) ||
          !operand_u32(frame, insn, 4, &flags)) {
        return;
      }
      (void)site_id;
      (void)flags;
      const Value receiver = read_reg(frame, recv_reg);
      if (fault_.has_value()) {
        return;
      }
      std::shared_ptr<InstanceValue> instance;
      if (!expect_instance_receiver(frame, receiver, &instance)) {
        return;
      }
      const std::optional<std::string> ivar_name =
          selector_text_from_symbol(symbol_id);
      if (!ivar_name.has_value()) {
        set_fault(frame, "VMError", "ivar symbol ref is out of range");
        return;
      }
      std::shared_ptr<RuntimeWatchHandle> handle =
          watch_ivar(frame, instance, *ivar_name);
      if (handle == nullptr ||
          !write_reg(frame, dst, Value::watch_handle(handle))) {
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
          if (slot == dst) {
            std::shared_ptr<RuntimeWatchCell> cell =
                ensure_local_storage_cell(frame, slot, closure_value);
            if (cell == nullptr) {
              return;
            }
            closure->captures.push_back(Value::watch_cell(cell));
          } else {
            std::shared_ptr<RuntimeWatchCell> cell =
                ensure_local_storage_cell(frame, slot);
            if (cell == nullptr) {
              return;
            }
            closure->captures.push_back(Value::watch_cell(cell));
          }
        } else {
          std::shared_ptr<RuntimeWatchCell> cell =
              ensure_capture_storage_cell(frame, slot);
          if (cell == nullptr) {
            return;
          }
          closure->captures.push_back(Value::watch_cell(cell));
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
      const FastCallStatus fast_status = step_fast_closure_call(frame, insn);
      if (fast_status == FastCallStatus::Faulted) {
        return;
      }
      if (fast_status == FastCallStatus::Matched) {
        return;
      }

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

      if (packet.callee.is_native_function()) {
        Value result = Value::null();
        const SendStatus status = apply_kernel_output_helper(
            frame, packet.callee.as_native_function().kind, packet.pos_args,
            packet.block, packet.kw_args, &result);
        if (status == SendStatus::Faulted) {
          return;
        }
        if (status == SendStatus::Matched) {
          if (!write_reg(frame, packet.dst, std::move(result))) {
            return;
          }
          ++frame.pc;
          return;
        }
      }

      if (packet.callee.is_native_type()) {
        const RuntimeNativeTypeKind kind = packet.callee.as_native_type().kind;
        if (is_conversion_type(kind)) {
          if (packet.pos_args.size() != 1U) {
            set_fault(frame, "TypeError",
                      "conversion type call expects one argument");
            return;
          }
          if (!packet.kw_args.empty()) {
            set_fault(frame, "TypeError",
                      "conversion type call does not accept keywords");
            return;
          }
          if (!packet.block.is_null()) {
            set_fault(frame, "TypeError",
                      "conversion type call does not accept block");
            return;
          }
          ConversionResult converted =
              convert_value_to_native_type(frame, packet.pos_args[0], kind);
          if (!converted.ok) {
            set_fault(frame, converted.error_name, converted.message);
            return;
          }
          if (!write_reg(frame, packet.dst, converted.value)) {
            return;
          }
          ++frame.pc;
          return;
        }
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
    case Opcode::IAdd:
    case Opcode::ISub:
    case Opcode::ILt:
    case Opcode::IGt:
    case Opcode::IMul:
    case Opcode::IDiv:
    case Opcode::IMod:
    case Opcode::IFloorDiv:
    case Opcode::ILe:
    case Opcode::IGe:
    case Opcode::IEq:
    case Opcode::INe:
    case Opcode::ICmp:
      step_integer_binary(frame, insn, false);
      return;
    case Opcode::IAddK:
    case Opcode::ISubK:
    case Opcode::ILtK:
    case Opcode::IGtK:
    case Opcode::IMulK:
    case Opcode::IDivK:
    case Opcode::IModK:
    case Opcode::IFloorDivK:
    case Opcode::ILeK:
    case Opcode::IGeK:
    case Opcode::IEqK:
    case Opcode::INeK:
    case Opcode::ICmpK:
      step_integer_binary(frame, insn, true);
      return;
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
      const bool capture_completed_frame =
          frames_.size() == 1U ||
          (frame.code != nullptr && frame.code->kind == CodeKind::Module);
      std::vector<Value> completed_regs;
      std::vector<std::uint8_t> completed_initialized;
      if (capture_completed_frame) {
        materialize_integer_regs(frame);
        completed_regs = frame.regs;
        completed_initialized = frame.initialized;
      }
      const BcCode *completed_code = frame.code;
      const std::optional<std::uint32_t> caller_reg = frame.caller_result_reg;
      Frame completed_frame = std::move(frames_.back());
      frames_.pop_back();
      if (capture_completed_frame) {
        last_completed_regs_ = std::move(completed_regs);
        last_completed_initialized_ = std::move(completed_initialized);
        if (completed_code != nullptr) {
          persist_module_bindings(*completed_code, last_completed_regs_,
                                  last_completed_initialized_);
        }
      }
      if (frames_.empty()) {
        final_value_ = value;
        recycle_frame(std::move(completed_frame));
        return;
      }
      Frame &caller = frames_.back();
      caller.active_call_pc.reset();
      if (!caller_reg.has_value() || !write_reg(caller, *caller_reg, value)) {
        recycle_frame(std::move(completed_frame));
        return;
      }
      recycle_frame(std::move(completed_frame));
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

  BcModule module_;
  std::size_t initial_string_count_ = 0;
  std::size_t initial_symbol_count_ = 0;
  std::shared_ptr<RuntimeState> state_;
  std::string module_id_;
  std::unordered_map<std::uint32_t, QuickCode> quick_codes_;
  std::unordered_map<std::uint32_t, DirectClosureKind> direct_closure_kinds_;
  std::unordered_map<std::uint32_t, std::vector<Frame>> frame_pool_;
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
      << ((method.flags & kMethodFlagClass) != 0U ? "class" : "instance")
      << "|property="
      << (((method.flags & kMethodFlagPropertyGetter) != 0U) ? "1" : "0")
      << "|property_setter="
      << (((method.flags & kMethodFlagPropertySetter) != 0U) ? "1" : "0") << "|"
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
    method.flags = kMethodFlagInstance |
                   (method.flags &
                    (kMethodFlagPropertyGetter | kMethodFlagPropertySetter));
    runtime_owner.instance_method_table.entries[method.selector_sym_id] =
        std::move(method);
    changed = true;
  }
  for (bytecode::BcMethod method : tx.class_methods) {
    method.owner_dispatch_ref = tx.target_index;
    method.flags = kMethodFlagClass |
                   (method.flags &
                    (kMethodFlagPropertyGetter | kMethodFlagPropertySetter));
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

std::uint64_t RuntimeWorld::watch_epoch() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->watch_epoch;
}

std::vector<RuntimeWatchEvent> RuntimeWorld::watch_events() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  return impl_->state->watch_events;
}

void RuntimeWorld::begin_dependency_capture(std::uint64_t notebook_cell_id) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return;
  }
  impl_->state->begin_dependency_capture(notebook_cell_id);
}

RuntimeDependencySet RuntimeWorld::end_dependency_capture() {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  return impl_->state->end_dependency_capture();
}

RuntimeDependencySet RuntimeWorld::dependency_capture_snapshot() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  return impl_->state->dependency_capture_snapshot();
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
  mirror.watch_epoch = impl_->state->watch_epoch;
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
  for (const auto &[name, value] : impl_->state->module_bindings) {
    (void)name;
    const Value root = unwrap_watch_value(value);
    if (value_has_heap_payload_tag(root)) {
      all_roots.push_back(root);
    }
  }
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

std::string
value_to_debug_string(const Value &value, const bytecode::BcModule *module,
                      const std::vector<std::string> *runtime_strings,
                      const std::vector<std::string> *runtime_symbols) {
  const std::vector<std::string> *debug_strings =
      runtime_strings != nullptr && !runtime_strings->empty()
          ? runtime_strings
          : (module == nullptr ? nullptr : &module->strings);
  const std::vector<std::string> *debug_symbols =
      runtime_symbols != nullptr && !runtime_symbols->empty()
          ? runtime_symbols
          : (module == nullptr ? nullptr : &module->symbols);
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
    if (debug_symbols != nullptr && symbol.symbol_id < debug_symbols->size()) {
      return ":" + (*debug_symbols)[symbol.symbol_id];
    }
    return ":<invalid>";
  }
  if (value.is_string()) {
    const StringValue string = value.as_string();
    if (debug_strings != nullptr && string.string_id < debug_strings->size()) {
      return "\"" + (*debug_strings)[string.string_id] + "\"";
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
      if (debug_symbols != nullptr && symbol_id < debug_symbols->size()) {
        out << " " << (*debug_symbols)[symbol_id];
      } else {
        out << " #" << klass.class_index;
      }
    } else {
      out << " #" << klass.class_index;
    }
    out << ">";
    return out.str();
  }
  if (value.is_native_type()) {
    return std::string("<type ") +
           native_type_name(value.as_native_type().kind) + ">";
  }
  if (value.is_native_function()) {
    return std::string("<function ") +
           native_function_name(value.as_native_function().kind) + ">";
  }
  if (value.is_text_writer()) {
    const std::shared_ptr<RuntimeTextWriter> writer = value.as_text_writer();
    if (writer == nullptr) {
      return "<io.TextWriter null>";
    }
    if (writer->buffered()) {
      return "<io.Buffer>";
    }
    const std::string stream = writer->stream_name();
    return stream.empty() ? "<io.TextWriter>"
                          : "<io.TextWriter " + stream + ">";
  }
  if (value.is_logger()) {
    return value.as_logger() == nullptr ? "<io.Logger null>" : "<io.Logger>";
  }
  if (value.is_watch_cell()) {
    const std::shared_ptr<RuntimeWatchCell> cell = value.as_watch_cell();
    if (cell == nullptr) {
      return "<watch-cell null>";
    }
    const RuntimeWatchCellSnapshot snapshot = cell->snapshot();
    std::ostringstream out;
    out << "<watch-cell #" << snapshot.cell_id << " r" << snapshot.revision
        << " "
        << value_to_debug_string(snapshot.value, module, runtime_strings,
                                 runtime_symbols)
        << ">";
    return out.str();
  }
  if (value.is_watch_handle()) {
    const std::shared_ptr<RuntimeWatchHandle> handle = value.as_watch_handle();
    if (handle == nullptr) {
      return "<watch null>";
    }
    const RuntimeWatchCellSnapshot snapshot = handle->snapshot();
    std::ostringstream out;
    out << "<watch #" << handle->handle_id();
    if (snapshot.cell_id != 0) {
      out << " cell:" << snapshot.cell_id << " r" << snapshot.revision;
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
      if (debug_symbols != nullptr && symbol_id < debug_symbols->size()) {
        out << " " << (*debug_symbols)[symbol_id];
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
      out << value_to_debug_string(list->items[i], module, runtime_strings,
                                   runtime_symbols);
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
      out << value_to_debug_string(tuple->items[i], module, runtime_strings,
                                   runtime_symbols);
    }
    out << ")";
    return out.str();
  }
  if (value.is_set()) {
    const std::shared_ptr<SetValue> set = value.as_set();
    if (set == nullptr) {
      return "{<null-set>}";
    }
    const std::string lifecycle = lifecycle_debug_label(set->header);
    if (!lifecycle.empty()) {
      return "{<" + lifecycle + "-set>}";
    }
    if (set->items.empty()) {
      return "Set{}";
    }
    std::ostringstream out;
    out << "{";
    for (std::size_t i = 0; i < set->items.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << value_to_debug_string(set->items[i], module, runtime_strings,
                                   runtime_symbols);
    }
    if (set->items.size() == 1U) {
      out << ",";
    }
    out << "}";
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
      if (debug_symbols != nullptr &&
          map->entries[i].symbol_id < debug_symbols->size()) {
        out << (*debug_symbols)[map->entries[i].symbol_id];
      } else {
        out << "#" << map->entries[i].symbol_id;
      }
      out << ": "
          << value_to_debug_string(map->entries[i].value, module,
                                   runtime_strings, runtime_symbols);
    }
    out << "}";
    return out.str();
  }
  return "<unknown>";
}

} // namespace amber::runtime
