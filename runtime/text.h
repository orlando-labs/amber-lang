#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace amber::runtime {

struct RuntimeTextWriteResult {
  bool ok = true;
  std::string error_name;
  std::string message;
};

struct RuntimeTextSourceLocation {
  bool present = false;
  std::uint32_t code_id = 0;
  std::uint32_t pc = 0;
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

struct RuntimeTextOutputEvent {
  std::string stream;
  std::string text;
  std::uint64_t order = 0;
  RuntimeTextSourceLocation source;
};

class RuntimeTextWriter {
public:
  RuntimeTextWriter();
  RuntimeTextWriter(const RuntimeTextWriter &) = delete;
  RuntimeTextWriter &operator=(const RuntimeTextWriter &) = delete;
  RuntimeTextWriter(RuntimeTextWriter &&) noexcept;
  RuntimeTextWriter &operator=(RuntimeTextWriter &&) noexcept;
  ~RuntimeTextWriter();

  static std::shared_ptr<RuntimeTextWriter> host_stdout();
  static std::shared_ptr<RuntimeTextWriter> host_stderr();
  static std::shared_ptr<RuntimeTextWriter> buffer();
  static std::shared_ptr<RuntimeTextWriter>
  cell_stream(std::string stream_name);

  RuntimeTextWriteResult write_str(const std::string &text);
  RuntimeTextWriteResult write_line(const std::string &text = {});
  RuntimeTextWriteResult flush();
  RuntimeTextWriteResult close();
  bool closed() const;
  bool buffered() const;
  bool xterm_color_available() const;
  std::string to_string() const;
  std::vector<RuntimeTextOutputEvent> events() const;
  std::string stream_name() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

std::shared_ptr<RuntimeTextWriter> current_runtime_stdout();
std::shared_ptr<RuntimeTextWriter> current_runtime_stderr();

class RuntimeOutputScope {
public:
  RuntimeOutputScope(std::shared_ptr<RuntimeTextWriter> stdout_writer = {},
                     std::shared_ptr<RuntimeTextWriter> stderr_writer = {});
  RuntimeOutputScope(const RuntimeOutputScope &) = delete;
  RuntimeOutputScope &operator=(const RuntimeOutputScope &) = delete;
  ~RuntimeOutputScope();

private:
  std::shared_ptr<RuntimeTextWriter> previous_stdout_;
  std::shared_ptr<RuntimeTextWriter> previous_stderr_;
};

enum class RuntimeLogLevel {
  Fatal = 0,
  Error = 1,
  Warn = 2,
  Info = 3,
  Debug = 4
};

enum class RuntimeLogColorMode { Auto, Always, Never };

std::uint64_t current_runtime_native_thread_id();
std::string current_runtime_task_annotation();

class RuntimeTaskAnnotationScope {
public:
  explicit RuntimeTaskAnnotationScope(std::string annotation);
  RuntimeTaskAnnotationScope(const RuntimeTaskAnnotationScope &) = delete;
  RuntimeTaskAnnotationScope &
  operator=(const RuntimeTaskAnnotationScope &) = delete;
  ~RuntimeTaskAnnotationScope();

private:
  std::string previous_annotation_;
};

class RuntimeLogger {
public:
  explicit RuntimeLogger(
      std::shared_ptr<RuntimeTextWriter> writer = {},
      RuntimeLogLevel level = RuntimeLogLevel::Info,
      RuntimeLogColorMode color_mode = RuntimeLogColorMode::Auto);
  RuntimeLogger(const RuntimeLogger &) = delete;
  RuntimeLogger &operator=(const RuntimeLogger &) = delete;
  RuntimeLogger(RuntimeLogger &&) noexcept;
  RuntimeLogger &operator=(RuntimeLogger &&) noexcept;
  ~RuntimeLogger();

  RuntimeTextWriteResult log(RuntimeLogLevel level, const std::string &message);
  RuntimeTextWriteResult fatal(const std::string &message);
  RuntimeTextWriteResult error(const std::string &message);
  RuntimeTextWriteResult warn(const std::string &message);
  RuntimeTextWriteResult info(const std::string &message);
  RuntimeTextWriteResult debug(const std::string &message);
  RuntimeTextWriteResult flush();
  RuntimeTextWriteResult close();
  bool closed() const;
  RuntimeLogLevel level() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace amber::runtime
