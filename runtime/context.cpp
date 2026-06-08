#include "runtime/context.h"

#include <utility>

namespace amber::runtime {

thread_local std::uint64_t tls_runtime_worker_id = 0;
thread_local std::uint64_t tls_runtime_strand_id = 0;
thread_local std::uint64_t tls_runtime_task_id = 0;
thread_local const std::atomic<bool> *tls_runtime_task_cancel_flag = nullptr;
thread_local std::uint32_t tls_runtime_task_sync_depth = 0;
thread_local std::shared_ptr<RuntimeTextWriter> tls_runtime_stdout;
thread_local std::shared_ptr<RuntimeTextWriter> tls_runtime_stderr;
thread_local std::string tls_runtime_task_annotation;
thread_local std::uint64_t tls_runtime_native_thread_id = 0;
thread_local RuntimeTextSourceLocation tls_runtime_text_source_location;

std::atomic<std::uint64_t> g_runtime_output_order{1};
std::atomic<std::uint64_t> g_runtime_native_thread_ids{1};

RuntimeTextSourceLocationScope::RuntimeTextSourceLocationScope(
    RuntimeTextSourceLocation location)
    : previous_(std::move(tls_runtime_text_source_location)) {
  tls_runtime_text_source_location = std::move(location);
}

RuntimeTextSourceLocationScope::~RuntimeTextSourceLocationScope() {
  tls_runtime_text_source_location = std::move(previous_);
}

RuntimeTaskScope::RuntimeTaskScope(std::uint64_t task_id,
                                   const std::atomic<bool> *cancel_flag)
    : previous_task_id_(tls_runtime_task_id),
      previous_cancel_flag_(tls_runtime_task_cancel_flag) {
  tls_runtime_task_id = task_id;
  tls_runtime_task_cancel_flag = cancel_flag;
}

RuntimeTaskScope::~RuntimeTaskScope() {
  tls_runtime_task_id = previous_task_id_;
  tls_runtime_task_cancel_flag = previous_cancel_flag_;
}

RuntimeTaskSyncScope::RuntimeTaskSyncScope() {
  ++tls_runtime_task_sync_depth;
}

RuntimeTaskSyncScope::~RuntimeTaskSyncScope() {
  if (tls_runtime_task_sync_depth > 0) {
    --tls_runtime_task_sync_depth;
  }
}

std::uint64_t current_runtime_owner_strand_id() {
  return tls_runtime_strand_id != 0 ? tls_runtime_strand_id
                                    : tls_runtime_worker_id;
}

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

} // namespace amber::runtime
