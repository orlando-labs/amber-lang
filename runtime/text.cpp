#include "runtime/context.h"

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace amber::runtime {

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
    impl_->events.push_back(
        RuntimeTextOutputEvent{impl_->stream.empty() ? "stdout" : impl_->stream,
                               text,
                               g_runtime_output_order.fetch_add(
                                   1, std::memory_order_relaxed),
                               tls_runtime_text_source_location});
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
        worker_(writer_->buffered() ? std::thread{}
                                    : std::thread([this]() { drain_loop(); })) {
  }

  ~Impl() { close(); }

  RuntimeTextWriteResult log(RuntimeLogLevel level,
                             const std::string &message) {
    if (!runtime_log_level_enabled(level_, level)) {
      return text_write_ok();
    }
    std::string line = format_runtime_log_line(level, color_enabled_, message);
    if (writer_->buffered()) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return text_write_error("ClosedResourceError", "logger is closed");
      }
      RuntimeTextWriteResult result = writer_->write_str(line + "\n");
      if (!result.ok && last_error_.ok) {
        last_error_ = result;
      }
      return result;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return text_write_error("ClosedResourceError", "logger is closed");
      }
      queue_.push_back(
          PendingLogLine{std::move(line), tls_runtime_text_source_location});
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
  struct PendingLogLine {
    std::string line;
    RuntimeTextSourceLocation source;
  };

  void drain_loop() {
    while (true) {
      std::deque<PendingLogLine> batch;
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

      for (const PendingLogLine &entry : batch) {
        RuntimeTextSourceLocationScope source_scope(entry.source);
        RuntimeTextWriteResult result = writer_->write_str(entry.line + "\n");
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
  std::deque<PendingLogLine> queue_;
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

} // namespace amber::runtime
