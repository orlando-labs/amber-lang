#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace amber::runtime {

class RuntimeIoValue {
public:
  virtual ~RuntimeIoValue() = default;
  virtual const char *type_name() const = 0;
  virtual bool shareable() const { return false; }
};

struct RuntimeIoStatus {
  bool ok = false;
  bool eof = false;
  bool would_block = false;
  bool timed_out = false;
  bool cancelled = false;
  // Layer B cooperative IO yield: set by wait_fd when parking is enabled
  // (tls_runtime_io_park_enabled) instead of blocking. The VM turns this into a
  // strand park + reactor wait_async; the blocking op is retried on resume.
  bool park = false;
  std::size_t count = 0;
  std::string bytes;
  std::string error_name;
  std::string message;
};

struct RuntimeByteResult : RuntimeIoStatus {
  std::uint8_t byte = 0;
};

class RuntimeBytes final : public RuntimeIoValue {
public:
  RuntimeBytes();
  explicit RuntimeBytes(std::string bytes);

  const char *type_name() const override { return "Bytes"; }
  bool shareable() const override { return true; }

  std::size_t count() const;
  bool empty() const;
  RuntimeByteResult at(std::int64_t index) const;
  std::shared_ptr<RuntimeBytes>
  slice(std::int64_t start,
        std::optional<std::size_t> length = std::nullopt) const;
  RuntimeIoStatus to_string(const std::string &encoding = "utf8") const;
  std::string hex() const;
  const std::string &string() const;

private:
  std::shared_ptr<const std::string> bytes_;
};

class RuntimeByteBuffer;

class RuntimeByteSlice final : public RuntimeIoValue {
public:
  const char *type_name() const override { return "io.ByteSlice"; }
  bool shareable() const override;

  std::size_t count() const;
  std::shared_ptr<RuntimeBytes> bytes() const;
  std::shared_ptr<RuntimeBytes> copy_bytes() const;
  std::shared_ptr<RuntimeByteBuffer> owner() const;

private:
  RuntimeByteSlice(std::shared_ptr<RuntimeByteBuffer> owner, std::size_t start,
                   std::size_t length);
  explicit RuntimeByteSlice(std::shared_ptr<RuntimeBytes> bytes);

  std::shared_ptr<RuntimeByteBuffer> owner_;
  std::shared_ptr<RuntimeBytes> detached_;
  std::size_t start_ = 0;
  std::size_t length_ = 0;

  friend class RuntimeByteBuffer;
};

struct RuntimeEndpoint final : RuntimeIoValue {
  std::string host;
  std::uint16_t port = 0;

  RuntimeEndpoint() = default;
  RuntimeEndpoint(std::string endpoint_host, std::uint16_t endpoint_port)
      : host(std::move(endpoint_host)), port(endpoint_port) {}

  const char *type_name() const override { return "net.Endpoint"; }
  bool shareable() const override { return true; }

  static RuntimeIoStatus parse(const std::string &value,
                               RuntimeEndpoint *endpoint);
  std::string family() const;
  std::string to_string() const;

  bool operator==(const RuntimeEndpoint &other) const {
    return host == other.host && port == other.port;
  }
};

struct RuntimeDatagramResult : RuntimeIoStatus {
  RuntimeEndpoint endpoint;
};

class RuntimeByteBuffer final
    : public RuntimeIoValue,
      public std::enable_shared_from_this<RuntimeByteBuffer> {
public:
  explicit RuntimeByteBuffer(std::size_t capacity);
  explicit RuntimeByteBuffer(const RuntimeBytes &bytes);

  const char *type_name() const override { return "io.ByteBuffer"; }

  std::size_t count() const;
  std::size_t capacity() const;
  std::size_t position() const;
  std::size_t limit() const;
  std::size_t remaining() const;
  bool empty() const;
  bool full() const;
  bool read_mode() const;
  RuntimeIoStatus access_status() const;
  // Re-binds this buffer's confinement owner to the current strand, so a buffer
  // created on one strand can be explicitly handed to a task and used there.
  void adopt_to_current_owner();
  RuntimeIoStatus clear();
  RuntimeIoStatus flip();
  RuntimeIoStatus rewind();
  RuntimeIoStatus compact();
  RuntimeByteResult get();
  RuntimeByteResult get_at(std::int64_t index) const;
  RuntimeIoStatus put(std::int64_t byte);
  RuntimeIoStatus put_all(const std::string &bytes);
  RuntimeIoStatus put_all(const RuntimeBytes &bytes);
  std::shared_ptr<RuntimeByteSlice>
  read_slice(std::optional<std::size_t> length = std::nullopt);
  std::shared_ptr<RuntimeByteSlice>
  write_slice(std::optional<std::size_t> length = std::nullopt);
  std::shared_ptr<RuntimeByteSlice>
  byte_slice(std::int64_t start,
             std::optional<std::size_t> length = std::nullopt);
  std::string bytes() const;
  std::shared_ptr<RuntimeBytes> copy_bytes() const;
  std::shared_ptr<RuntimeBytes> freeze_bytes() const;
  std::string slice(std::size_t start,
                    std::optional<std::size_t> length = std::nullopt) const;

  // Runtime readers use this to commit bytes into writable capacity.
  std::size_t append(const std::uint8_t *data, std::size_t size);

private:
  RuntimeIoStatus check_owner() const;
  std::pair<std::size_t, std::size_t> active_range() const;

  std::vector<std::uint8_t> storage_;
  std::size_t position_ = 0;
  std::size_t limit_ = 0;
  bool read_mode_ = false;
  // Atomic so an explicit adopt_to_current_owner() handoff can re-stamp the
  // owner while another strand concurrently observes it in check_owner().
  std::atomic<std::uint64_t> owner_strand_id_{0};

  friend class RuntimeFile;
  friend class RuntimePipeReader;
  friend class RuntimeTcpStream;
};

class RuntimePath final : public RuntimeIoValue {
public:
  explicit RuntimePath(std::string path);

  const char *type_name() const override { return "fs.Path"; }
  bool shareable() const override { return true; }

  const std::string &string() const;
  RuntimePath join(const RuntimePath &part) const;
  RuntimePath join(const std::string &part) const;
  RuntimePath join(const std::vector<std::string> &parts) const;
  std::string basename() const;
  std::string extname() const;
  RuntimePath parent() const;
  bool absolute() const;
  RuntimePath normalize() const;

private:
  std::string path_;
};

enum class RuntimeFileMode { Read, Write, Append, ReadWrite };
enum class RuntimeSeekWhence { Start, Current, End };
enum class RuntimeShutdownSide { Read, Write, Both };
enum class RuntimeIsolationMode { Checked, Unchecked };

struct RuntimeMetadata final : RuntimeIoValue {
  RuntimePath path{std::string{}};
  std::uint64_t size = 0;
  bool file = false;
  bool directory = false;
  bool symlink = false;

  const char *type_name() const override { return "fs.Metadata"; }
  bool shareable() const override { return true; }
};

struct RuntimeFileOpenOptions {
  bool create = false;
  bool truncate = false;
  bool append = false;
  bool exclusive = false;
  std::optional<std::uint32_t> permissions;
};

class RuntimeIoResource : public RuntimeIoValue {
public:
  explicit RuntimeIoResource(
      RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked);
  virtual ~RuntimeIoResource();

  RuntimeIoResource(const RuntimeIoResource &) = delete;
  RuntimeIoResource &operator=(const RuntimeIoResource &) = delete;

  bool closed() const;
  std::uint64_t owner_strand_id() const;
  void allow_unchecked_sharing(bool allow = true);
  bool unchecked_sharing_allowed() const;
  // Re-binds this resource's confinement owner to the current strand. This is
  // the explicit handoff primitive: an accept-on-one-strand socket can be moved
  // into a worker task, which adopts it before use.
  void adopt_to_current_owner();
  std::uint64_t resource_id() const;
  RuntimeIoStatus access_status() const;
  virtual RuntimeIoStatus close();

protected:
  RuntimeIoStatus check_access() const;
  void mark_closed();

private:
  // Atomic so adopt_to_current_owner() can re-stamp the owner while another
  // strand concurrently observes it in check_access().
  std::atomic<std::uint64_t> owner_strand_id_{0};
  std::uint64_t resource_id_ = 0;
  std::atomic<bool> closed_{false};
  std::atomic<bool> unchecked_sharing_{false};
};

class RuntimeFile;

struct RuntimeFileOpenResult : RuntimeIoStatus {
  std::shared_ptr<RuntimeFile> file;
};

class RuntimeFile final : public RuntimeIoResource {
public:
  ~RuntimeFile() override;
  const char *type_name() const override { return "fs.File"; }

  static RuntimeFileOpenResult
  open(const RuntimePath &path, RuntimeFileMode mode,
       RuntimeFileOpenOptions options = {},
       RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked);

  RuntimePath path() const;
  RuntimeFileMode mode() const;

  RuntimeIoStatus
  read(RuntimeByteBuffer &buffer,
       std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus try_read(RuntimeByteBuffer &buffer);
  RuntimeIoStatus
  write(const std::string &bytes,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus try_write(const std::string &bytes);
  RuntimeIoStatus write_all(
      const std::string &bytes,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus read_exact(
      std::size_t count,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus read_all(std::optional<std::size_t> limit = std::nullopt);
  RuntimeIoStatus read_line(
      std::optional<std::size_t> limit = std::nullopt,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus seek(std::int64_t offset, RuntimeSeekWhence whence);
  RuntimeIoStatus
  flush(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus
  sync(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus tell();
  RuntimeIoStatus size();
  RuntimeIoStatus metadata(RuntimeMetadata *metadata);
  RuntimeIoStatus close() override;

private:
  RuntimeFile(int fd, RuntimeFileMode mode, std::string path,
              RuntimeIsolationMode isolation);

  int fd_ = -1;
  RuntimeFileMode mode_ = RuntimeFileMode::Read;
  std::string path_;
  std::string line_pending_;
};

using RuntimeMemoryFileCloseCallback =
    std::function<RuntimeIoStatus(const std::string &)>;

class RuntimeMemoryFile final : public RuntimeIoResource {
public:
  ~RuntimeMemoryFile() override;
  const char *type_name() const override { return "fs.File"; }

  RuntimeMemoryFile(
      std::string path, RuntimeFileMode mode, std::string bytes = {},
      RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked,
      RuntimeMemoryFileCloseCallback close_callback = {});

  RuntimePath path() const;
  RuntimeFileMode mode() const;
  const std::string &bytes() const;

  RuntimeIoStatus
  read(RuntimeByteBuffer &buffer,
       std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus try_read(RuntimeByteBuffer &buffer);
  RuntimeIoStatus
  write(const std::string &bytes,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus try_write(const std::string &bytes);
  RuntimeIoStatus write_all(
      const std::string &bytes,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus read_exact(
      std::size_t count,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus read_all(std::optional<std::size_t> limit = std::nullopt);
  RuntimeIoStatus read_line(
      std::optional<std::size_t> limit = std::nullopt,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus seek(std::int64_t offset, RuntimeSeekWhence whence);
  RuntimeIoStatus
  flush(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus
  sync(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus tell();
  RuntimeIoStatus size();
  RuntimeIoStatus metadata(RuntimeMetadata *metadata);
  RuntimeIoStatus close() override;

private:
  bool readable() const;
  bool writable() const;

  RuntimeFileMode mode_ = RuntimeFileMode::Read;
  std::string path_;
  std::string bytes_;
  std::size_t position_ = 0;
  bool dirty_ = false;
  RuntimeMemoryFileCloseCallback close_callback_;
};

class RuntimePipeReader;
class RuntimePipeWriter;

struct RuntimePipeResult : RuntimeIoStatus {
  std::shared_ptr<class RuntimePipe> pipe;
  std::shared_ptr<RuntimePipeReader> reader;
  std::shared_ptr<RuntimePipeWriter> writer;
};

class RuntimePipe final : public RuntimeIoValue,
                          public std::enable_shared_from_this<RuntimePipe> {
public:
  const char *type_name() const override { return "io.Pipe"; }
  bool shareable() const override { return true; }

  static RuntimePipeResult
  create(std::int64_t capacity = 64 * 1024,
         RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked);
  std::shared_ptr<RuntimePipeReader> reader() const;
  std::shared_ptr<RuntimePipeWriter> writer() const;
  std::size_t capacity() const;
  std::size_t buffered() const;
  bool closed() const;
  RuntimeIoStatus close();

private:
  struct State;
  explicit RuntimePipe(std::shared_ptr<State> state);
  static std::shared_ptr<RuntimePipe>
  from_state(const std::shared_ptr<State> &state);
  std::shared_ptr<State> state_;
  std::shared_ptr<RuntimePipeReader> reader_;
  std::shared_ptr<RuntimePipeWriter> writer_;

  friend class RuntimePipeReader;
  friend class RuntimePipeWriter;
};

class RuntimePipeReader final : public RuntimeIoResource {
public:
  ~RuntimePipeReader() override;
  const char *type_name() const override { return "io.PipeReader"; }
  bool shareable() const override { return true; }

  RuntimeIoStatus
  read(RuntimeByteBuffer &buffer,
       std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus try_read(RuntimeByteBuffer &buffer);
  RuntimeIoStatus close() override;
  std::shared_ptr<RuntimePipe> pipe() const;

private:
  explicit RuntimePipeReader(std::shared_ptr<RuntimePipe::State> state);
  std::shared_ptr<RuntimePipe::State> state_;

  friend class RuntimePipe;
  friend class RuntimePipeWriter;
};

class RuntimePipeWriter final : public RuntimeIoResource {
public:
  ~RuntimePipeWriter() override;
  const char *type_name() const override { return "io.PipeWriter"; }
  bool shareable() const override { return true; }

  RuntimeIoStatus
  write(const std::string &bytes,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus try_write(const std::string &bytes);
  RuntimeIoStatus write_all(
      const std::string &bytes,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus close() override;
  RuntimeIoStatus
  flush(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  std::shared_ptr<RuntimePipe> pipe() const;

private:
  explicit RuntimePipeWriter(std::shared_ptr<RuntimePipe::State> state);
  std::shared_ptr<RuntimePipe::State> state_;

  friend class RuntimePipe;
};

class RuntimeTcpStream;
class RuntimeTcpListener;
class RuntimeUdpSocket;

struct RuntimeTcpConnectResult : RuntimeIoStatus {
  std::shared_ptr<RuntimeTcpStream> stream;
};

struct RuntimeTcpListenResult : RuntimeIoStatus {
  std::shared_ptr<RuntimeTcpListener> listener;
};

struct RuntimeTcpAcceptResult : RuntimeIoStatus {
  std::shared_ptr<RuntimeTcpStream> stream;
};

class RuntimeTcpStream final : public RuntimeIoResource {
public:
  ~RuntimeTcpStream() override;
  const char *type_name() const override { return "net.TcpStream"; }

  static RuntimeTcpConnectResult
  connect(const RuntimeEndpoint &endpoint,
          std::chrono::milliseconds timeout = std::chrono::milliseconds::max(),
          RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked);

  RuntimeIoStatus
  read(RuntimeByteBuffer &buffer,
       std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus try_read(RuntimeByteBuffer &buffer);
  RuntimeIoStatus
  write(const std::string &bytes,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus try_write(const std::string &bytes);
  RuntimeIoStatus write_all(
      const std::string &bytes,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus shutdown(RuntimeShutdownSide side);
  RuntimeIoStatus close_read();
  RuntimeIoStatus close_write();
  RuntimeIoStatus
  flush(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus set_nodelay(bool enabled);
  RuntimeIoStatus set_keepalive(bool enabled);
  RuntimeIoStatus set_recv_buffer(std::int64_t size);
  RuntimeIoStatus set_send_buffer(std::int64_t size);
  RuntimeIoStatus get_option(const std::string &name, std::int64_t *value);
  RuntimeIoStatus close() override;
  RuntimeEndpoint local_endpoint() const;
  RuntimeEndpoint remote_endpoint() const;

private:
  explicit RuntimeTcpStream(
      int fd, RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked);
  int fd_ = -1;

  friend class RuntimeTcpListener;
};

class RuntimeTcpListener final : public RuntimeIoResource {
public:
  ~RuntimeTcpListener() override;
  const char *type_name() const override { return "net.TcpListener"; }

  static RuntimeTcpListenResult
  listen(const RuntimeEndpoint &endpoint, int backlog = 128,
         bool reuse_addr = false,
         RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked);
  RuntimeTcpAcceptResult
  accept(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeTcpAcceptResult try_accept();
  RuntimeIoStatus set_reuse_addr(bool enabled);
  RuntimeIoStatus set_reuse_port(bool enabled);
  RuntimeIoStatus get_option(const std::string &name, std::int64_t *value);
  RuntimeIoStatus close() override;
  RuntimeEndpoint local_endpoint() const;

private:
  RuntimeIsolationMode isolation_mode_ = RuntimeIsolationMode::Checked;
  explicit RuntimeTcpListener(
      int fd, RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked);
  int fd_ = -1;
};

struct RuntimeUdpBindResult : RuntimeIoStatus {
  std::shared_ptr<RuntimeUdpSocket> socket;
};

class RuntimeUdpSocket final : public RuntimeIoResource {
public:
  ~RuntimeUdpSocket() override;
  const char *type_name() const override { return "net.UdpSocket"; }

  static RuntimeUdpBindResult
  bind(const RuntimeEndpoint &endpoint,
       RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked);
  static RuntimeUdpBindResult
  open(const std::string &family = "inet",
       RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked);
  RuntimeDatagramResult recv_from(
      std::size_t max = 65507, bool truncate = false,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeDatagramResult try_recv_from(std::size_t max = 65507,
                                      bool truncate = false);
  RuntimeIoStatus
  send_to(const std::string &bytes, const RuntimeEndpoint &endpoint,
          std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus try_send_to(const std::string &bytes,
                              const RuntimeEndpoint &endpoint);
  RuntimeIoStatus connect(const RuntimeEndpoint &endpoint);
  RuntimeIoStatus recv(std::size_t max, bool truncate,
                       std::chrono::milliseconds timeout, std::string *bytes);
  RuntimeIoStatus
  send(const std::string &bytes,
       std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
  RuntimeIoStatus set_recv_buffer(std::int64_t size);
  RuntimeIoStatus set_send_buffer(std::int64_t size);
  RuntimeIoStatus get_option(const std::string &name, std::int64_t *value);
  RuntimeIoStatus close() override;
  RuntimeEndpoint local_endpoint() const;

private:
  explicit RuntimeUdpSocket(
      int fd, RuntimeIsolationMode isolation = RuntimeIsolationMode::Checked);
  int fd_ = -1;
};

std::optional<RuntimeFileMode>
runtime_file_mode_from_name(const std::string &name);
std::optional<RuntimeSeekWhence>
runtime_seek_whence_from_name(const std::string &name);
std::optional<RuntimeShutdownSide>
runtime_shutdown_side_from_name(const std::string &name);
std::optional<RuntimeIsolationMode>
runtime_isolation_mode_from_name(const std::string &name);

RuntimeIoStatus runtime_fs_exists(const RuntimePath &path, bool *exists);
RuntimeIoStatus runtime_fs_file(const RuntimePath &path, bool *file);
RuntimeIoStatus runtime_fs_dir(const RuntimePath &path, bool *directory);
RuntimeIoStatus runtime_fs_metadata(const RuntimePath &path,
                                    RuntimeMetadata *metadata);
RuntimeIoStatus runtime_fs_read_bytes(const RuntimePath &path,
                                      std::optional<std::size_t> limit,
                                      std::shared_ptr<RuntimeBytes> *bytes);
RuntimeIoStatus runtime_fs_write_bytes(const RuntimePath &path,
                                       const std::string &bytes,
                                       bool create = true,
                                       bool truncate = true);
RuntimeIoStatus runtime_fs_mkdir(const RuntimePath &path);
RuntimeIoStatus runtime_fs_mkdir_p(const RuntimePath &path);
RuntimeIoStatus runtime_fs_remove(const RuntimePath &path);
RuntimeIoStatus runtime_fs_rename(const RuntimePath &from,
                                  const RuntimePath &to);
RuntimeIoStatus runtime_fs_copy(const RuntimePath &from, const RuntimePath &to,
                                std::size_t *count);

} // namespace amber::runtime
