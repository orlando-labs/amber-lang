#include "runtime/io.h"

#include "runtime/context.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace amber::runtime {

namespace {

using Clock = std::chrono::steady_clock;
using Deadline = std::optional<Clock::time_point>;
std::atomic<std::uint64_t> next_io_resource_id{1};

RuntimeIoStatus io_ok(std::size_t count = 0) {
  RuntimeIoStatus result;
  result.ok = true;
  result.count = count;
  return result;
}

RuntimeIoStatus io_error(std::string name, std::string message) {
  RuntimeIoStatus result;
  result.error_name = std::move(name);
  result.message = std::move(message);
  return result;
}

RuntimeIoStatus io_timeout(const char *operation) {
  RuntimeIoStatus result =
      io_error("TimeoutError", std::string(operation) + " timed out");
  result.timed_out = true;
  return result;
}

RuntimeIoStatus io_cancelled(const char *operation) {
  RuntimeIoStatus result =
      io_error("CancelledError", std::string(operation) + " cancelled");
  result.cancelled = true;
  return result;
}

RuntimeIoStatus io_would_block() {
  RuntimeIoStatus result;
  result.would_block = true;
  return result;
}

RuntimeIoStatus io_eof() {
  RuntimeIoStatus result = io_ok();
  result.eof = true;
  return result;
}

std::string errno_error_name(int error) {
  switch (error) {
  case ENOENT:
    return "FileNotFoundError";
  case EACCES:
  case EPERM:
    return "PermissionDeniedError";
  case EISDIR:
    return "IsDirectoryError";
  case ENOTDIR:
    return "NotDirectoryError";
  case EEXIST:
    return "FileExistsError";
  case EBUSY:
    return "ResourceBusyError";
  case EPIPE:
    return "BrokenPipeError";
  case ECONNRESET:
    return "ConnectionResetError";
  case ECONNREFUSED:
    return "ConnectionRefusedError";
  case EADDRINUSE:
    return "AddressInUseError";
  case EADDRNOTAVAIL:
    return "AddressNotAvailableError";
  case ENETUNREACH:
  case EHOSTUNREACH:
    return "NetworkUnreachableError";
  default:
    return "IOError";
  }
}

RuntimeIoStatus errno_status(const char *operation, int error = errno) {
  return io_error(errno_error_name(error),
                  std::string(operation) + ": " + std::strerror(error));
}

Deadline make_deadline(std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max()) {
    return std::nullopt;
  }
  return Clock::now() + std::max(timeout, std::chrono::milliseconds(0));
}

bool deadline_expired(const Deadline &deadline) {
  return deadline.has_value() && Clock::now() >= *deadline;
}

std::chrono::milliseconds remaining_timeout(const Deadline &deadline) {
  if (!deadline.has_value()) {
    return std::chrono::milliseconds::max();
  }
  const auto now = Clock::now();
  if (now >= *deadline) {
    return std::chrono::milliseconds(0);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
}

std::optional<std::chrono::milliseconds>
wait_scope_timeout(const Deadline &deadline) {
  const std::chrono::milliseconds remaining = remaining_timeout(deadline);
  if (remaining == std::chrono::milliseconds::max()) {
    return std::nullopt;
  }
  return remaining;
}

RuntimeIoWaitInterest interest_from_poll(short events) {
  if ((events & POLLIN) != 0) {
    return RuntimeIoWaitInterest::Read;
  }
  if ((events & POLLOUT) != 0) {
    return RuntimeIoWaitInterest::Write;
  }
  return RuntimeIoWaitInterest::Other;
}

RuntimeIoStatus
wait_fd(int fd, short events, const Deadline &deadline, const char *operation,
        std::uint64_t resource_id = 0, std::string resource = {},
        RuntimeIoWaitInterest interest = RuntimeIoWaitInterest::Other) {
  if (current_runtime_task_cancel_requested()) {
    return io_cancelled(operation);
  }
  if (deadline_expired(deadline)) {
    return io_timeout(operation);
  }
  if (interest == RuntimeIoWaitInterest::Other) {
    interest = interest_from_poll(events);
  }
  RuntimeIoWaitScope wait_scope(operation, std::move(resource), interest,
                                resource_id, wait_scope_timeout(deadline));
  while (true) {
    if (current_runtime_task_cancel_requested()) {
      return io_cancelled(operation);
    }
    if (deadline_expired(deadline)) {
      return io_timeout(operation);
    }

    int wait_ms = 10;
    if (deadline.has_value()) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(*deadline -
                                                                Clock::now());
      wait_ms = static_cast<int>(
          std::clamp<std::int64_t>(remaining.count(), 0, wait_ms));
    }
    pollfd descriptor{fd, events, 0};
    const int rc = ::poll(&descriptor, 1, wait_ms);
    if (rc > 0) {
      if ((descriptor.revents & POLLNVAL) != 0) {
        return io_error("AlreadyClosedError", "resource is closed");
      }
      if ((descriptor.revents & (events | POLLERR | POLLHUP)) != 0) {
        return io_ok();
      }
    } else if (rc < 0 && errno != EINTR) {
      return errno_status("poll");
    }
  }
}

bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

RuntimeIoStatus fd_read_once(int fd, RuntimeByteBuffer &buffer,
                             bool nonblocking, const Deadline &deadline,
                             const char *operation,
                             std::uint64_t resource_id = 0,
                             std::string resource = {}) {
  if (buffer.read_mode() || buffer.remaining() == 0) {
    return io_error("ArgumentError", "read buffer has no remaining capacity");
  }

  std::vector<std::uint8_t> bytes(buffer.remaining());
  while (true) {
    const ssize_t count = ::read(fd, bytes.data(), bytes.size());
    if (count > 0) {
      buffer.append(bytes.data(), static_cast<std::size_t>(count));
      return io_ok(static_cast<std::size_t>(count));
    }
    if (count == 0) {
      return io_eof();
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (nonblocking) {
        return io_would_block();
      }
      RuntimeIoStatus waited =
          wait_fd(fd, POLLIN, deadline, operation, resource_id, resource);
      if (!waited.ok) {
        return waited;
      }
      continue;
    }
    return errno_status(operation);
  }
}

int socket_send_flags() {
#ifdef MSG_NOSIGNAL
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

RuntimeIoStatus fd_write_once(int fd, const std::string &bytes,
                              bool nonblocking, const Deadline &deadline,
                              const char *operation, bool socket,
                              std::uint64_t resource_id = 0,
                              std::string resource = {}) {
  if (bytes.empty()) {
    return io_ok();
  }
  while (true) {
    const ssize_t count =
        socket ? ::send(fd, bytes.data(), bytes.size(), socket_send_flags())
               : ::write(fd, bytes.data(), bytes.size());
    if (count > 0) {
      return io_ok(static_cast<std::size_t>(count));
    }
    if (count == 0) {
      return io_error("IOError", "write returned zero for non-empty input");
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (nonblocking) {
        return io_would_block();
      }
      RuntimeIoStatus waited =
          wait_fd(fd, POLLOUT, deadline, operation, resource_id, resource);
      if (!waited.ok) {
        return waited;
      }
      continue;
    }
    return errno_status(operation);
  }
}

RuntimeIoStatus fd_write_all(int fd, const std::string &bytes,
                             std::chrono::milliseconds timeout,
                             const char *operation, bool socket,
                             std::uint64_t resource_id = 0,
                             std::string resource = {}) {
  const Deadline deadline = make_deadline(timeout);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    RuntimeIoStatus result =
        fd_write_once(fd, bytes.substr(offset), false, deadline, operation,
                      socket, resource_id, resource);
    if (!result.ok) {
      return result;
    }
    if (result.count == 0) {
      return io_error("IOError",
                      std::string(operation) + " made no write progress");
    }
    offset += result.count;
  }
  return io_ok(offset);
}

RuntimeEndpoint endpoint_from_sockaddr(const sockaddr *address,
                                       socklen_t address_length) {
  char host[NI_MAXHOST] = {};
  char service[NI_MAXSERV] = {};
  if (::getnameinfo(address, address_length, host, sizeof(host), service,
                    sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return {};
  }
  RuntimeEndpoint endpoint;
  endpoint.host = host;
  try {
    endpoint.port = static_cast<std::uint16_t>(std::stoul(service));
  } catch (...) {
    endpoint.port = 0;
  }
  return endpoint;
}

RuntimeEndpoint socket_endpoint(int fd, bool peer) {
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  const int rc =
      peer ? ::getpeername(fd, reinterpret_cast<sockaddr *>(&address), &length)
           : ::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length);
  return rc == 0 ? endpoint_from_sockaddr(
                       reinterpret_cast<sockaddr *>(&address), length)
                 : RuntimeEndpoint{};
}

struct AddressList {
  addrinfo *head = nullptr;
  ~AddressList() {
    if (head != nullptr) {
      ::freeaddrinfo(head);
    }
  }
};

RuntimeIoStatus resolve_endpoint(const RuntimeEndpoint &endpoint, int socktype,
                                 bool passive, AddressList *addresses) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = socktype;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  const std::string service = std::to_string(endpoint.port);
  const char *host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
  const int rc = ::getaddrinfo(host, service.c_str(), &hints, &addresses->head);
  if (rc != 0) {
    return io_error("DnsError", ::gai_strerror(rc));
  }
  return io_ok();
}

void configure_socket_no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
  (void)fd;
#endif
}

template <typename Predicate>
RuntimeIoStatus
wait_condition(std::unique_lock<std::mutex> &lock, std::condition_variable &cv,
               const Deadline &deadline, const char *operation,
               std::uint64_t resource_id, std::string resource,
               RuntimeIoWaitInterest interest, Predicate predicate) {
  if (current_runtime_task_cancel_requested()) {
    return io_cancelled(operation);
  }
  if (deadline_expired(deadline)) {
    return io_timeout(operation);
  }
  RuntimeIoWaitScope wait_scope(operation, std::move(resource), interest,
                                resource_id, wait_scope_timeout(deadline));
  while (!predicate()) {
    if (current_runtime_task_cancel_requested()) {
      return io_cancelled(operation);
    }
    if (deadline_expired(deadline)) {
      return io_timeout(operation);
    }
    const auto duration =
        deadline.has_value()
            ? std::min(*deadline - Clock::now(),
                       Clock::duration(std::chrono::milliseconds(10)))
            : Clock::duration(std::chrono::milliseconds(10));
    cv.wait_for(lock, std::max(duration, Clock::duration::zero()));
  }
  return io_ok();
}

} // namespace

RuntimeBytes::RuntimeBytes() : bytes_(std::make_shared<const std::string>()) {}

RuntimeBytes::RuntimeBytes(std::string bytes)
    : bytes_(std::make_shared<const std::string>(std::move(bytes))) {}

std::size_t RuntimeBytes::count() const { return bytes_->size(); }

bool RuntimeBytes::empty() const { return bytes_->empty(); }

RuntimeByteResult RuntimeBytes::at(std::int64_t index) const {
  RuntimeByteResult result;
  std::int64_t normalized = index;
  if (normalized < 0) {
    normalized += static_cast<std::int64_t>(bytes_->size());
  }
  if (normalized < 0 ||
      static_cast<std::size_t>(normalized) >= bytes_->size()) {
    static_cast<RuntimeIoStatus &>(result) =
        io_error("IndexError", "Bytes index is out of bounds");
    return result;
  }
  result.ok = true;
  result.byte = static_cast<std::uint8_t>((*bytes_)[normalized]);
  result.count = 1;
  return result;
}

std::shared_ptr<RuntimeBytes>
RuntimeBytes::slice(std::int64_t start,
                    std::optional<std::size_t> length) const {
  std::int64_t normalized = start;
  if (normalized < 0) {
    normalized += static_cast<std::int64_t>(bytes_->size());
  }
  normalized = std::clamp<std::int64_t>(
      normalized, 0, static_cast<std::int64_t>(bytes_->size()));
  const std::size_t offset = static_cast<std::size_t>(normalized);
  const std::size_t count = std::min(length.value_or(bytes_->size() - offset),
                                     bytes_->size() - offset);
  return std::make_shared<RuntimeBytes>(bytes_->substr(offset, count));
}

RuntimeIoStatus RuntimeBytes::to_string(const std::string &encoding) const {
  if (encoding != "utf8") {
    return io_error("ArgumentError", "only utf8 encoding is supported");
  }
  RuntimeIoStatus result = io_ok(bytes_->size());
  result.bytes = *bytes_;
  return result;
}

std::string RuntimeBytes::hex() const {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes_->size() * 2U);
  for (unsigned char byte : *bytes_) {
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 0x0fU]);
  }
  return result;
}

const std::string &RuntimeBytes::string() const { return *bytes_; }

RuntimeByteSlice::RuntimeByteSlice(std::shared_ptr<RuntimeByteBuffer> owner,
                                   std::size_t start, std::size_t length)
    : owner_(std::move(owner)), start_(start), length_(length) {}

RuntimeByteSlice::RuntimeByteSlice(std::shared_ptr<RuntimeBytes> bytes)
    : detached_(std::move(bytes)) {
  length_ = detached_ == nullptr ? 0 : detached_->count();
}

bool RuntimeByteSlice::shareable() const { return detached_ != nullptr; }

std::size_t RuntimeByteSlice::count() const { return length_; }

std::shared_ptr<RuntimeBytes> RuntimeByteSlice::bytes() const {
  if (detached_ != nullptr) {
    return detached_;
  }
  return owner_ == nullptr
             ? std::make_shared<RuntimeBytes>()
             : std::make_shared<RuntimeBytes>(owner_->slice(start_, length_));
}

std::shared_ptr<RuntimeBytes> RuntimeByteSlice::copy_bytes() const {
  return std::make_shared<RuntimeBytes>(bytes()->string());
}

std::shared_ptr<RuntimeByteBuffer> RuntimeByteSlice::owner() const {
  return owner_;
}

RuntimeByteBuffer::RuntimeByteBuffer(std::size_t capacity)
    : storage_(capacity), limit_(capacity),
      owner_strand_id_(current_runtime_owner_strand_id()) {
  if (owner_strand_id_ == 0) {
    owner_strand_id_ = current_runtime_native_thread_id();
  }
}

RuntimeByteBuffer::RuntimeByteBuffer(const RuntimeBytes &bytes)
    : RuntimeByteBuffer(bytes.count()) {
  (void)put_all(bytes);
  (void)flip();
}

RuntimeIoStatus RuntimeByteBuffer::check_owner() const {
  std::uint64_t current = current_runtime_owner_strand_id();
  if (current == 0) {
    current = current_runtime_native_thread_id();
  }
  return current == owner_strand_id_
             ? io_ok()
             : io_error("IsolationError",
                        "ByteBuffer accessed from a non-owner strand");
}

std::pair<std::size_t, std::size_t> RuntimeByteBuffer::active_range() const {
  return read_mode_ ? std::pair<std::size_t, std::size_t>{position_, limit_}
                    : std::pair<std::size_t, std::size_t>{0, position_};
}

std::size_t RuntimeByteBuffer::count() const {
  const auto range = active_range();
  return range.second - range.first;
}

std::size_t RuntimeByteBuffer::capacity() const { return storage_.size(); }

std::size_t RuntimeByteBuffer::position() const { return position_; }

std::size_t RuntimeByteBuffer::limit() const { return limit_; }

std::size_t RuntimeByteBuffer::remaining() const { return limit_ - position_; }

bool RuntimeByteBuffer::empty() const { return count() == 0; }

bool RuntimeByteBuffer::full() const { return remaining() == 0; }

bool RuntimeByteBuffer::read_mode() const { return read_mode_; }

RuntimeIoStatus RuntimeByteBuffer::access_status() const {
  return check_owner();
}

RuntimeIoStatus RuntimeByteBuffer::clear() {
  RuntimeIoStatus access = check_owner();
  if (!access.ok) {
    return access;
  }
  position_ = 0;
  limit_ = storage_.size();
  read_mode_ = false;
  return io_ok();
}

RuntimeIoStatus RuntimeByteBuffer::flip() {
  RuntimeIoStatus access = check_owner();
  if (!access.ok) {
    return access;
  }
  limit_ = position_;
  position_ = 0;
  read_mode_ = true;
  return io_ok();
}

RuntimeIoStatus RuntimeByteBuffer::rewind() {
  RuntimeIoStatus access = check_owner();
  if (!access.ok) {
    return access;
  }
  position_ = 0;
  return io_ok();
}

RuntimeIoStatus RuntimeByteBuffer::compact() {
  RuntimeIoStatus access = check_owner();
  if (!access.ok) {
    return access;
  }
  if (!read_mode_) {
    return io_error("ArgumentError", "compact requires read mode");
  }
  const std::size_t unread = remaining();
  std::move(storage_.begin() + static_cast<std::ptrdiff_t>(position_),
            storage_.begin() + static_cast<std::ptrdiff_t>(limit_),
            storage_.begin());
  position_ = unread;
  limit_ = storage_.size();
  read_mode_ = false;
  return io_ok();
}

RuntimeByteResult RuntimeByteBuffer::get() {
  RuntimeByteResult result;
  RuntimeIoStatus access = check_owner();
  if (!access.ok) {
    static_cast<RuntimeIoStatus &>(result) = std::move(access);
    return result;
  }
  if (position_ >= limit_) {
    static_cast<RuntimeIoStatus &>(result) =
        io_error("IndexError", "ByteBuffer has no remaining bytes");
    return result;
  }
  result.ok = true;
  result.count = 1;
  result.byte = storage_[position_++];
  return result;
}

RuntimeByteResult RuntimeByteBuffer::get_at(std::int64_t index) const {
  RuntimeByteResult result;
  RuntimeIoStatus access = check_owner();
  if (!access.ok) {
    static_cast<RuntimeIoStatus &>(result) = std::move(access);
    return result;
  }
  const auto range = active_range();
  std::int64_t normalized = index;
  const std::size_t size = range.second - range.first;
  if (normalized < 0) {
    normalized += static_cast<std::int64_t>(size);
  }
  if (normalized < 0 || static_cast<std::size_t>(normalized) >= size) {
    static_cast<RuntimeIoStatus &>(result) =
        io_error("IndexError", "ByteBuffer index is out of bounds");
    return result;
  }
  result.ok = true;
  result.count = 1;
  result.byte = storage_[range.first + static_cast<std::size_t>(normalized)];
  return result;
}

RuntimeIoStatus RuntimeByteBuffer::put(std::int64_t byte) {
  RuntimeIoStatus access = check_owner();
  if (!access.ok) {
    return access;
  }
  if (byte < 0 || byte > 255) {
    return io_error("ArgumentError", "byte must be between 0 and 255");
  }
  if (read_mode_) {
    return io_error("ArgumentError", "put requires write mode");
  }
  if (remaining() == 0) {
    return io_error("ArgumentError", "ByteBuffer is full");
  }
  storage_[position_++] = static_cast<std::uint8_t>(byte);
  return io_ok(1);
}

RuntimeIoStatus RuntimeByteBuffer::put_all(const std::string &bytes) {
  RuntimeIoStatus access = check_owner();
  if (!access.ok) {
    return access;
  }
  if (read_mode_) {
    return io_error("ArgumentError", "put_all requires write mode");
  }
  if (bytes.size() > remaining()) {
    return io_error("ArgumentError", "bytes exceed ByteBuffer remaining space");
  }
  append(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
  return io_ok(bytes.size());
}

RuntimeIoStatus RuntimeByteBuffer::put_all(const RuntimeBytes &bytes) {
  return put_all(bytes.string());
}

std::shared_ptr<RuntimeByteSlice>
RuntimeByteBuffer::read_slice(std::optional<std::size_t> length) {
  const std::size_t count = std::min(length.value_or(remaining()), remaining());
  const std::size_t start = position_;
  position_ += count;
  try {
    return std::shared_ptr<RuntimeByteSlice>(
        new RuntimeByteSlice(shared_from_this(), start, count));
  } catch (const std::bad_weak_ptr &) {
    return std::shared_ptr<RuntimeByteSlice>(new RuntimeByteSlice(
        std::make_shared<RuntimeBytes>(slice(start, count))));
  }
}

std::shared_ptr<RuntimeByteSlice>
RuntimeByteBuffer::write_slice(std::optional<std::size_t> length) {
  const std::size_t count = std::min(length.value_or(remaining()), remaining());
  try {
    return std::shared_ptr<RuntimeByteSlice>(
        new RuntimeByteSlice(shared_from_this(), position_, count));
  } catch (const std::bad_weak_ptr &) {
    return std::shared_ptr<RuntimeByteSlice>(new RuntimeByteSlice(
        std::make_shared<RuntimeBytes>(slice(position_, count))));
  }
}

std::shared_ptr<RuntimeByteSlice>
RuntimeByteBuffer::byte_slice(std::int64_t start,
                              std::optional<std::size_t> length) {
  const auto range = active_range();
  std::int64_t normalized = start;
  const std::size_t size = range.second - range.first;
  if (normalized < 0) {
    normalized += static_cast<std::int64_t>(size);
  }
  normalized =
      std::clamp<std::int64_t>(normalized, 0, static_cast<std::int64_t>(size));
  const std::size_t offset = range.first + static_cast<std::size_t>(normalized);
  const std::size_t count =
      std::min(length.value_or(range.second - offset), range.second - offset);
  try {
    return std::shared_ptr<RuntimeByteSlice>(
        new RuntimeByteSlice(shared_from_this(), offset, count));
  } catch (const std::bad_weak_ptr &) {
    return std::shared_ptr<RuntimeByteSlice>(new RuntimeByteSlice(
        std::make_shared<RuntimeBytes>(slice(offset, count))));
  }
}

std::string RuntimeByteBuffer::bytes() const {
  const auto range = active_range();
  return std::string(
      reinterpret_cast<const char *>(storage_.data() + range.first),
      range.second - range.first);
}

std::shared_ptr<RuntimeBytes> RuntimeByteBuffer::copy_bytes() const {
  return std::make_shared<RuntimeBytes>(bytes());
}

std::shared_ptr<RuntimeBytes> RuntimeByteBuffer::freeze_bytes() const {
  return copy_bytes();
}

std::string RuntimeByteBuffer::slice(std::size_t start,
                                     std::optional<std::size_t> length) const {
  if (start > storage_.size()) {
    return {};
  }
  const std::size_t available = storage_.size() - start;
  const std::size_t size = std::min(length.value_or(available), available);
  return std::string(reinterpret_cast<const char *>(storage_.data() + start),
                     size);
}

std::size_t RuntimeByteBuffer::append(const std::uint8_t *data,
                                      std::size_t size) {
  if (read_mode_) {
    return 0;
  }
  const std::size_t copied = std::min(size, remaining());
  std::copy(data, data + copied, storage_.begin() + position_);
  position_ += copied;
  return copied;
}

RuntimeIoStatus RuntimeEndpoint::parse(const std::string &value,
                                       RuntimeEndpoint *endpoint) {
  if (endpoint == nullptr) {
    return io_error("TypeError", "endpoint output is null");
  }
  std::string host;
  std::string port_text;
  if (!value.empty() && value.front() == '[') {
    const std::size_t close = value.find(']');
    if (close == std::string::npos || close + 1 >= value.size() ||
        value[close + 1] != ':') {
      return io_error("ArgumentError", "invalid endpoint");
    }
    host = value.substr(1, close - 1);
    port_text = value.substr(close + 2);
  } else {
    const std::size_t colon = value.rfind(':');
    if (colon == std::string::npos) {
      return io_error("ArgumentError", "endpoint must include a port");
    }
    host = value.substr(0, colon);
    port_text = value.substr(colon + 1);
  }
  try {
    std::size_t consumed = 0;
    const unsigned long port = std::stoul(port_text, &consumed);
    if (consumed != port_text.size() || port > 65535UL) {
      return io_error("ArgumentError", "endpoint port is out of range");
    }
    endpoint->host = std::move(host);
    endpoint->port = static_cast<std::uint16_t>(port);
    return io_ok();
  } catch (...) {
    return io_error("ArgumentError", "invalid endpoint port");
  }
}

std::string RuntimeEndpoint::family() const {
  return host.find(':') == std::string::npos ? "inet" : "inet6";
}

std::string RuntimeEndpoint::to_string() const {
  return family() == "inet6" ? "[" + host + "]:" + std::to_string(port)
                             : host + ":" + std::to_string(port);
}

RuntimePath::RuntimePath(std::string path) : path_(std::move(path)) {}

const std::string &RuntimePath::string() const { return path_; }

RuntimePath RuntimePath::join(const RuntimePath &part) const {
  return join(part.string());
}

RuntimePath RuntimePath::join(const std::string &part) const {
  return RuntimePath((std::filesystem::path(path_) / part).string());
}

RuntimePath RuntimePath::join(const std::vector<std::string> &parts) const {
  std::filesystem::path result(path_);
  for (const std::string &part : parts) {
    result /= part;
  }
  return RuntimePath(result.string());
}

std::string RuntimePath::basename() const {
  return std::filesystem::path(path_).filename().string();
}

std::string RuntimePath::extname() const {
  return std::filesystem::path(path_).extension().string();
}

RuntimePath RuntimePath::parent() const {
  return RuntimePath(std::filesystem::path(path_).parent_path().string());
}

bool RuntimePath::absolute() const {
  return std::filesystem::path(path_).is_absolute();
}

RuntimePath RuntimePath::normalize() const {
  return RuntimePath(std::filesystem::path(path_).lexically_normal().string());
}

RuntimeIoResource::RuntimeIoResource(RuntimeIsolationMode isolation)
    : owner_strand_id_(current_runtime_owner_strand_id()),
      resource_id_(next_io_resource_id.fetch_add(1, std::memory_order_relaxed)),
      unchecked_sharing_(isolation == RuntimeIsolationMode::Unchecked) {
  if (owner_strand_id_ == 0) {
    owner_strand_id_ = current_runtime_native_thread_id();
  }
}

RuntimeIoResource::~RuntimeIoResource() = default;

bool RuntimeIoResource::closed() const { return closed_.load(); }

std::uint64_t RuntimeIoResource::owner_strand_id() const {
  return owner_strand_id_;
}

void RuntimeIoResource::allow_unchecked_sharing(bool allow) {
  unchecked_sharing_.store(allow);
}

bool RuntimeIoResource::unchecked_sharing_allowed() const {
  return unchecked_sharing_.load();
}

std::uint64_t RuntimeIoResource::resource_id() const { return resource_id_; }

RuntimeIoStatus RuntimeIoResource::access_status() const {
  return check_access();
}

RuntimeIoStatus RuntimeIoResource::close() {
  if (closed()) {
    return io_ok();
  }
  mark_closed();
  return io_ok();
}

RuntimeIoStatus RuntimeIoResource::check_access() const {
  std::uint64_t current_owner = current_runtime_owner_strand_id();
  if (current_owner == 0) {
    current_owner = current_runtime_native_thread_id();
  }
  if (!shareable() && !unchecked_sharing_allowed() && owner_strand_id_ != 0 &&
      current_owner != owner_strand_id_) {
    return io_error("IsolationError",
                    "IO resource accessed from a non-owner strand");
  }
  if (closed()) {
    return io_error("AlreadyClosedError", "resource is already closed");
  }
  return io_ok();
}

void RuntimeIoResource::mark_closed() { closed_.store(true); }

RuntimeFile::RuntimeFile(int fd, RuntimeFileMode mode, std::string path,
                         RuntimeIsolationMode isolation)
    : RuntimeIoResource(isolation), fd_(fd), mode_(mode),
      path_(std::move(path)) {}

RuntimeFile::~RuntimeFile() { (void)close(); }

RuntimeFileOpenResult RuntimeFile::open(const RuntimePath &path,
                                        RuntimeFileMode mode,
                                        RuntimeFileOpenOptions options,
                                        RuntimeIsolationMode isolation) {
  RuntimeFileOpenResult result;
  if (mode == RuntimeFileMode::Read && options.truncate) {
    static_cast<RuntimeIoStatus &>(result) =
        io_error("ArgumentError", "read mode cannot truncate");
    return result;
  }
  if (mode == RuntimeFileMode::Append && options.truncate) {
    static_cast<RuntimeIoStatus &>(result) =
        io_error("ArgumentError", "append mode cannot truncate");
    return result;
  }
  if (options.exclusive && !options.create) {
    static_cast<RuntimeIoStatus &>(result) =
        io_error("ArgumentError", "exclusive open requires create: true");
    return result;
  }

  int flags = O_CLOEXEC | O_NONBLOCK;
  switch (mode) {
  case RuntimeFileMode::Read:
    flags |= O_RDONLY;
    break;
  case RuntimeFileMode::Write:
    flags |= O_WRONLY;
    break;
  case RuntimeFileMode::Append:
    flags |= O_WRONLY | O_APPEND;
    break;
  case RuntimeFileMode::ReadWrite:
    flags |= O_RDWR;
    break;
  }
  if (options.create) {
    flags |= O_CREAT;
  }
  if (options.truncate) {
    flags |= O_TRUNC;
  }
  if (options.append || mode == RuntimeFileMode::Append) {
    flags |= O_APPEND;
  }
  if (options.exclusive) {
    flags |= O_EXCL;
  }
  const mode_t permissions =
      static_cast<mode_t>(options.permissions.value_or(0666));
  const int fd = ::open(path.string().c_str(), flags, permissions);
  if (fd < 0) {
    static_cast<RuntimeIoStatus &>(result) = errno_status("open");
    return result;
  }
  result.ok = true;
  result.file = std::shared_ptr<RuntimeFile>(
      new RuntimeFile(fd, mode, path.string(), isolation));
  return result;
}

RuntimePath RuntimeFile::path() const { return RuntimePath(path_); }

RuntimeFileMode RuntimeFile::mode() const { return mode_; }

RuntimeIoStatus RuntimeFile::read(RuntimeByteBuffer &buffer,
                                  std::chrono::milliseconds timeout) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (mode_ == RuntimeFileMode::Write || mode_ == RuntimeFileMode::Append) {
    return io_error("PermissionDeniedError", "file is not open for reading");
  }
  return fd_read_once(fd_, buffer, false, make_deadline(timeout), "file read",
                      resource_id(), path_);
}

RuntimeIoStatus RuntimeFile::try_read(RuntimeByteBuffer &buffer) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (mode_ == RuntimeFileMode::Write || mode_ == RuntimeFileMode::Append) {
    return io_error("PermissionDeniedError", "file is not open for reading");
  }
  return fd_read_once(fd_, buffer, true, std::nullopt, "file read");
}

RuntimeIoStatus RuntimeFile::write(const std::string &bytes,
                                   std::chrono::milliseconds timeout) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (mode_ == RuntimeFileMode::Read) {
    return io_error("PermissionDeniedError", "file is not open for writing");
  }
  return fd_write_once(fd_, bytes, false, make_deadline(timeout), "file write",
                       false, resource_id(), path_);
}

RuntimeIoStatus RuntimeFile::try_write(const std::string &bytes) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (mode_ == RuntimeFileMode::Read) {
    return io_error("PermissionDeniedError", "file is not open for writing");
  }
  return fd_write_once(fd_, bytes, true, std::nullopt, "file write", false);
}

RuntimeIoStatus RuntimeFile::write_all(const std::string &bytes,
                                       std::chrono::milliseconds timeout) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (mode_ == RuntimeFileMode::Read) {
    return io_error("PermissionDeniedError", "file is not open for writing");
  }
  return fd_write_all(fd_, bytes, timeout, "file write", false, resource_id(),
                      path_);
}

RuntimeIoStatus RuntimeFile::read_exact(std::size_t count,
                                        std::chrono::milliseconds timeout) {
  RuntimeByteBuffer buffer(count);
  const Deadline deadline = make_deadline(timeout);
  while (buffer.count() < count) {
    RuntimeIoStatus result = read(buffer, remaining_timeout(deadline));
    if (result.eof) {
      return io_error("EOFError", "stream ended before requested byte count");
    }
    if (!result.ok) {
      return result;
    }
  }
  RuntimeIoStatus result = io_ok(count);
  result.bytes = buffer.bytes();
  return result;
}

RuntimeIoStatus RuntimeFile::read_all(std::optional<std::size_t> limit) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  std::string bytes;
  while (true) {
    RuntimeByteBuffer buffer(8192);
    RuntimeIoStatus result = read(buffer);
    if (result.eof) {
      RuntimeIoStatus done = io_ok(bytes.size());
      done.bytes = std::move(bytes);
      return done;
    }
    if (!result.ok) {
      return result;
    }
    if (limit.has_value() && bytes.size() + result.count > *limit) {
      return io_error("ArgumentError", "read_all limit exceeded");
    }
    bytes += buffer.bytes();
  }
}

RuntimeIoStatus RuntimeFile::read_line(std::optional<std::size_t> limit,
                                       std::chrono::milliseconds timeout) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  const Deadline deadline = make_deadline(timeout);
  std::string line;
  while (true) {
    RuntimeByteBuffer buffer(1);
    RuntimeIoStatus result = read(buffer, remaining_timeout(deadline));
    if (result.eof) {
      if (line.empty()) {
        return result;
      }
      RuntimeIoStatus done = io_ok(line.size());
      done.bytes = std::move(line);
      return done;
    }
    if (!result.ok) {
      return result;
    }
    const char byte = buffer.bytes()[0];
    if (byte == '\n') {
      RuntimeIoStatus done = io_ok(line.size());
      done.bytes = std::move(line);
      return done;
    }
    line.push_back(byte);
    if (limit.has_value() && line.size() > *limit) {
      return io_error("ArgumentError", "read_line limit exceeded");
    }
  }
}

RuntimeIoStatus RuntimeFile::seek(std::int64_t offset,
                                  RuntimeSeekWhence whence) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  int native_whence = SEEK_SET;
  if (whence == RuntimeSeekWhence::Current) {
    native_whence = SEEK_CUR;
  } else if (whence == RuntimeSeekWhence::End) {
    native_whence = SEEK_END;
  }
  const off_t position =
      ::lseek(fd_, static_cast<off_t>(offset), native_whence);
  if (position < 0) {
    return errno_status("seek");
  }
  return io_ok(static_cast<std::size_t>(position));
}

RuntimeIoStatus RuntimeFile::flush(std::chrono::milliseconds timeout) {
  (void)timeout;
  return check_access();
}

RuntimeIoStatus RuntimeFile::sync(std::chrono::milliseconds timeout) {
  (void)timeout;
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  return ::fsync(fd_) == 0 ? io_ok() : errno_status("sync");
}

RuntimeIoStatus RuntimeFile::tell() {
  return seek(0, RuntimeSeekWhence::Current);
}

RuntimeIoStatus RuntimeFile::size() {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  struct stat info{};
  if (::fstat(fd_, &info) != 0) {
    return errno_status("file size");
  }
  return io_ok(static_cast<std::size_t>(info.st_size));
}

RuntimeIoStatus RuntimeFile::metadata(RuntimeMetadata *metadata) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (metadata == nullptr) {
    return io_error("TypeError", "metadata output is null");
  }
  struct stat info{};
  if (::fstat(fd_, &info) != 0) {
    return errno_status("file metadata");
  }
  metadata->path = RuntimePath(path_);
  metadata->size = static_cast<std::uint64_t>(info.st_size);
  metadata->file = S_ISREG(info.st_mode);
  metadata->directory = S_ISDIR(info.st_mode);
  metadata->symlink = S_ISLNK(info.st_mode);
  return io_ok();
}

RuntimeIoStatus RuntimeFile::close() {
  if (closed()) {
    return io_ok();
  }
  const int fd = fd_;
  fd_ = -1;
  mark_closed();
  if (fd >= 0 && ::close(fd) != 0) {
    return errno_status("close");
  }
  return io_ok();
}

RuntimeMemoryFile::RuntimeMemoryFile(
    std::string path, RuntimeFileMode mode, std::string bytes,
    RuntimeIsolationMode isolation,
    RuntimeMemoryFileCloseCallback close_callback)
    : RuntimeIoResource(isolation), mode_(mode), path_(std::move(path)),
      bytes_(std::move(bytes)), close_callback_(std::move(close_callback)) {
  position_ = mode_ == RuntimeFileMode::Append ? bytes_.size() : 0;
}

RuntimeMemoryFile::~RuntimeMemoryFile() { (void)close(); }

RuntimePath RuntimeMemoryFile::path() const { return RuntimePath(path_); }

RuntimeFileMode RuntimeMemoryFile::mode() const { return mode_; }

const std::string &RuntimeMemoryFile::bytes() const { return bytes_; }

bool RuntimeMemoryFile::readable() const {
  return mode_ == RuntimeFileMode::Read || mode_ == RuntimeFileMode::ReadWrite;
}

bool RuntimeMemoryFile::writable() const {
  return mode_ == RuntimeFileMode::Write || mode_ == RuntimeFileMode::Append ||
         mode_ == RuntimeFileMode::ReadWrite;
}

RuntimeIoStatus RuntimeMemoryFile::read(RuntimeByteBuffer &buffer,
                                        std::chrono::milliseconds timeout) {
  (void)timeout;
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (!readable()) {
    return io_error("PermissionDeniedError", "file is not open for reading");
  }
  if (buffer.read_mode() || buffer.remaining() == 0) {
    return io_error("ArgumentError", "read buffer has no remaining capacity");
  }
  if (position_ >= bytes_.size()) {
    return io_eof();
  }
  const std::size_t count =
      std::min(buffer.remaining(), bytes_.size() - position_);
  buffer.append(
      reinterpret_cast<const std::uint8_t *>(bytes_.data()) + position_, count);
  position_ += count;
  return io_ok(count);
}

RuntimeIoStatus RuntimeMemoryFile::try_read(RuntimeByteBuffer &buffer) {
  return read(buffer, std::chrono::milliseconds(0));
}

RuntimeIoStatus RuntimeMemoryFile::write(const std::string &bytes,
                                         std::chrono::milliseconds timeout) {
  (void)timeout;
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (!writable()) {
    return io_error("PermissionDeniedError", "file is not open for writing");
  }
  if (bytes.empty()) {
    return io_ok();
  }
  if (mode_ == RuntimeFileMode::Append) {
    position_ = bytes_.size();
  }
  if (position_ > bytes_.size()) {
    bytes_.resize(position_, '\0');
  }
  if (position_ + bytes.size() > bytes_.size()) {
    bytes_.resize(position_ + bytes.size());
  }
  std::copy(bytes.begin(), bytes.end(), bytes_.begin() + position_);
  position_ += bytes.size();
  dirty_ = true;
  return io_ok(bytes.size());
}

RuntimeIoStatus RuntimeMemoryFile::try_write(const std::string &bytes) {
  return write(bytes, std::chrono::milliseconds(0));
}

RuntimeIoStatus
RuntimeMemoryFile::write_all(const std::string &bytes,
                             std::chrono::milliseconds timeout) {
  return write(bytes, timeout);
}

RuntimeIoStatus
RuntimeMemoryFile::read_exact(std::size_t count,
                              std::chrono::milliseconds timeout) {
  RuntimeByteBuffer buffer(count);
  while (buffer.count() < count) {
    RuntimeIoStatus result = read(buffer, timeout);
    if (result.eof) {
      return io_error("EOFError", "stream ended before requested byte count");
    }
    if (!result.ok) {
      return result;
    }
  }
  RuntimeIoStatus result = io_ok(count);
  result.bytes = buffer.bytes();
  return result;
}

RuntimeIoStatus RuntimeMemoryFile::read_all(std::optional<std::size_t> limit) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (!readable()) {
    return io_error("PermissionDeniedError", "file is not open for reading");
  }
  const std::size_t remaining =
      position_ >= bytes_.size() ? 0 : bytes_.size() - position_;
  if (limit.has_value() && remaining > *limit) {
    return io_error("ArgumentError", "read_all limit exceeded");
  }
  RuntimeIoStatus result = io_ok(remaining);
  result.bytes = bytes_.substr(position_, remaining);
  position_ = bytes_.size();
  return result;
}

RuntimeIoStatus
RuntimeMemoryFile::read_line(std::optional<std::size_t> limit,
                             std::chrono::milliseconds timeout) {
  (void)timeout;
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (!readable()) {
    return io_error("PermissionDeniedError", "file is not open for reading");
  }
  if (position_ >= bytes_.size()) {
    return io_eof();
  }
  std::string line;
  while (position_ < bytes_.size()) {
    const char byte = bytes_[position_++];
    if (byte == '\n') {
      RuntimeIoStatus result = io_ok(line.size());
      result.bytes = std::move(line);
      return result;
    }
    line.push_back(byte);
    if (limit.has_value() && line.size() > *limit) {
      return io_error("ArgumentError", "read_line limit exceeded");
    }
  }
  RuntimeIoStatus result = io_ok(line.size());
  result.bytes = std::move(line);
  return result;
}

RuntimeIoStatus RuntimeMemoryFile::seek(std::int64_t offset,
                                        RuntimeSeekWhence whence) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  std::int64_t base = 0;
  if (whence == RuntimeSeekWhence::Current) {
    base = static_cast<std::int64_t>(position_);
  } else if (whence == RuntimeSeekWhence::End) {
    base = static_cast<std::int64_t>(bytes_.size());
  }
  const std::int64_t next = base + offset;
  if (next < 0) {
    return io_error("ArgumentError", "seek position must be non-negative");
  }
  position_ = static_cast<std::size_t>(next);
  return io_ok(position_);
}

RuntimeIoStatus RuntimeMemoryFile::flush(std::chrono::milliseconds timeout) {
  (void)timeout;
  return check_access();
}

RuntimeIoStatus RuntimeMemoryFile::sync(std::chrono::milliseconds timeout) {
  (void)timeout;
  return check_access();
}

RuntimeIoStatus RuntimeMemoryFile::tell() {
  return seek(0, RuntimeSeekWhence::Current);
}

RuntimeIoStatus RuntimeMemoryFile::size() {
  RuntimeIoStatus access = check_access();
  return access.ok ? io_ok(bytes_.size()) : access;
}

RuntimeIoStatus RuntimeMemoryFile::metadata(RuntimeMetadata *metadata) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (metadata == nullptr) {
    return io_error("TypeError", "metadata output is null");
  }
  metadata->path = RuntimePath(path_);
  metadata->size = static_cast<std::uint64_t>(bytes_.size());
  metadata->file = true;
  metadata->directory = false;
  metadata->symlink = false;
  return io_ok();
}

RuntimeIoStatus RuntimeMemoryFile::close() {
  if (closed()) {
    return io_ok();
  }
  if (close_callback_) {
    RuntimeIoStatus result = close_callback_(bytes_);
    if (!result.ok) {
      return result;
    }
  }
  mark_closed();
  return io_ok();
}

struct RuntimePipe::State {
  explicit State(std::size_t pipe_capacity) : capacity(pipe_capacity) {}

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::uint8_t> bytes;
  std::size_t capacity = 0;
  std::size_t waiting_readers = 0;
  bool reader_open = true;
  bool writer_open = true;
  std::weak_ptr<RuntimePipe> pipe;
  std::weak_ptr<RuntimePipeReader> reader;
  std::weak_ptr<RuntimePipeWriter> writer;
};

RuntimePipe::RuntimePipe(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

std::shared_ptr<RuntimePipe>
RuntimePipe::from_state(const std::shared_ptr<State> &state) {
  std::lock_guard<std::mutex> lock(state->mutex);
  if (std::shared_ptr<RuntimePipe> existing = state->pipe.lock()) {
    return existing;
  }
  auto pipe = std::shared_ptr<RuntimePipe>(new RuntimePipe(state));
  pipe->reader_ = state->reader.lock();
  pipe->writer_ = state->writer.lock();
  if (pipe->reader_ == nullptr) {
    pipe->reader_ =
        std::shared_ptr<RuntimePipeReader>(new RuntimePipeReader(state));
    state->reader = pipe->reader_;
  }
  if (pipe->writer_ == nullptr) {
    pipe->writer_ =
        std::shared_ptr<RuntimePipeWriter>(new RuntimePipeWriter(state));
    state->writer = pipe->writer_;
  }
  state->pipe = pipe;
  return pipe;
}

RuntimePipeResult RuntimePipe::create(std::int64_t capacity,
                                      RuntimeIsolationMode isolation) {
  RuntimePipeResult result;
  if (capacity < 0) {
    static_cast<RuntimeIoStatus &>(result) =
        io_error("ArgumentError", "pipe capacity must be non-negative");
    return result;
  }
  auto state = std::make_shared<State>(static_cast<std::size_t>(capacity));
  auto pipe = std::shared_ptr<RuntimePipe>(new RuntimePipe(state));
  pipe->reader_ =
      std::shared_ptr<RuntimePipeReader>(new RuntimePipeReader(state));
  pipe->writer_ =
      std::shared_ptr<RuntimePipeWriter>(new RuntimePipeWriter(state));
  state->pipe = pipe;
  state->reader = pipe->reader_;
  state->writer = pipe->writer_;
  if (isolation == RuntimeIsolationMode::Unchecked) {
    pipe->reader_->allow_unchecked_sharing();
    pipe->writer_->allow_unchecked_sharing();
  }
  result.ok = true;
  result.pipe = pipe;
  result.reader = pipe->reader_;
  result.writer = pipe->writer_;
  return result;
}

std::shared_ptr<RuntimePipeReader> RuntimePipe::reader() const {
  return reader_;
}

std::shared_ptr<RuntimePipeWriter> RuntimePipe::writer() const {
  return writer_;
}

std::size_t RuntimePipe::capacity() const { return state_->capacity; }

std::size_t RuntimePipe::buffered() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->bytes.size();
}

bool RuntimePipe::closed() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return !state_->reader_open && !state_->writer_open;
}

RuntimeIoStatus RuntimePipe::close() {
  RuntimeIoStatus reader_result = reader_->close();
  RuntimeIoStatus writer_result = writer_->close();
  return !reader_result.ok ? reader_result : writer_result;
}

RuntimePipeReader::RuntimePipeReader(std::shared_ptr<RuntimePipe::State> state)
    : state_(std::move(state)) {}

RuntimePipeReader::~RuntimePipeReader() { (void)close(); }

RuntimeIoStatus RuntimePipeReader::read(RuntimeByteBuffer &buffer,
                                        std::chrono::milliseconds timeout) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (buffer.read_mode() || buffer.remaining() == 0) {
    return io_error("ArgumentError", "read buffer has no remaining capacity");
  }
  const Deadline deadline = make_deadline(timeout);
  std::unique_lock<std::mutex> lock(state_->mutex);
  ++state_->waiting_readers;
  state_->cv.notify_all();
  RuntimeIoStatus waited =
      wait_condition(lock, state_->cv, deadline, "pipe read", resource_id(),
                     "pipe", RuntimeIoWaitInterest::Read, [&] {
                       return !state_->bytes.empty() || !state_->writer_open ||
                              !state_->reader_open;
                     });
  --state_->waiting_readers;
  if (!waited.ok) {
    return waited;
  }
  if (!state_->reader_open) {
    return io_error("AlreadyClosedError", "pipe reader is closed");
  }
  if (state_->bytes.empty() && !state_->writer_open) {
    return io_eof();
  }
  const std::size_t count = std::min(buffer.remaining(), state_->bytes.size());
  std::vector<std::uint8_t> copied(count);
  for (std::size_t i = 0; i < count; ++i) {
    copied[i] = state_->bytes.front();
    state_->bytes.pop_front();
  }
  buffer.append(copied.data(), copied.size());
  lock.unlock();
  state_->cv.notify_all();
  return io_ok(count);
}

RuntimeIoStatus RuntimePipeReader::try_read(RuntimeByteBuffer &buffer) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (buffer.read_mode() || buffer.remaining() == 0) {
    return io_error("ArgumentError", "read buffer has no remaining capacity");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->bytes.empty()) {
    return state_->writer_open ? io_would_block() : io_eof();
  }
  const std::size_t count = std::min(buffer.remaining(), state_->bytes.size());
  std::vector<std::uint8_t> copied(count);
  for (std::size_t i = 0; i < count; ++i) {
    copied[i] = state_->bytes.front();
    state_->bytes.pop_front();
  }
  buffer.append(copied.data(), copied.size());
  state_->cv.notify_all();
  return io_ok(count);
}

RuntimeIoStatus RuntimePipeReader::close() {
  if (closed()) {
    return io_ok();
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->reader_open = false;
    state_->bytes.clear();
  }
  mark_closed();
  state_->cv.notify_all();
  return io_ok();
}

std::shared_ptr<RuntimePipe> RuntimePipeReader::pipe() const {
  return RuntimePipe::from_state(state_);
}

RuntimePipeWriter::RuntimePipeWriter(std::shared_ptr<RuntimePipe::State> state)
    : state_(std::move(state)) {}

RuntimePipeWriter::~RuntimePipeWriter() { (void)close(); }

RuntimeIoStatus RuntimePipeWriter::write(const std::string &bytes,
                                         std::chrono::milliseconds timeout) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (bytes.empty()) {
    return io_ok();
  }
  const Deadline deadline = make_deadline(timeout);
  std::unique_lock<std::mutex> lock(state_->mutex);
  RuntimeIoStatus waited = wait_condition(
      lock, state_->cv, deadline, "pipe write", resource_id(), "pipe",
      RuntimeIoWaitInterest::Write, [&] {
        if (!state_->reader_open || !state_->writer_open) {
          return true;
        }
        if (state_->capacity == 0) {
          return state_->waiting_readers > 0 && state_->bytes.empty();
        }
        return state_->bytes.size() < state_->capacity;
      });
  if (!waited.ok) {
    return waited;
  }
  if (!state_->reader_open) {
    return io_error("BrokenPipeError", "pipe reader is closed");
  }
  if (!state_->writer_open) {
    return io_error("AlreadyClosedError", "pipe writer is closed");
  }
  const std::size_t available =
      state_->capacity == 0 ? 1 : state_->capacity - state_->bytes.size();
  const std::size_t count = std::min(bytes.size(), available);
  for (std::size_t i = 0; i < count; ++i) {
    state_->bytes.push_back(static_cast<std::uint8_t>(bytes[i]));
  }
  lock.unlock();
  state_->cv.notify_all();
  return io_ok(count);
}

RuntimeIoStatus RuntimePipeWriter::try_write(const std::string &bytes) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (bytes.empty()) {
    return io_ok();
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!state_->reader_open) {
    return io_error("BrokenPipeError", "pipe reader is closed");
  }
  const bool unavailable =
      state_->capacity == 0
          ? state_->waiting_readers == 0 || !state_->bytes.empty()
          : state_->bytes.size() >= state_->capacity;
  if (unavailable) {
    return io_would_block();
  }
  const std::size_t available =
      state_->capacity == 0 ? 1 : state_->capacity - state_->bytes.size();
  const std::size_t count = std::min(bytes.size(), available);
  for (std::size_t i = 0; i < count; ++i) {
    state_->bytes.push_back(static_cast<std::uint8_t>(bytes[i]));
  }
  state_->cv.notify_all();
  return io_ok(count);
}

RuntimeIoStatus
RuntimePipeWriter::write_all(const std::string &bytes,
                             std::chrono::milliseconds timeout) {
  const Deadline deadline = make_deadline(timeout);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    RuntimeIoStatus result =
        write(bytes.substr(offset), remaining_timeout(deadline));
    if (!result.ok) {
      return result;
    }
    if (result.count == 0) {
      return io_error("IOError", "pipe write made no progress");
    }
    offset += result.count;
  }
  return io_ok(offset);
}

RuntimeIoStatus RuntimePipeWriter::close() {
  if (closed()) {
    return io_ok();
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->writer_open = false;
  }
  mark_closed();
  state_->cv.notify_all();
  return io_ok();
}

RuntimeIoStatus RuntimePipeWriter::flush(std::chrono::milliseconds timeout) {
  (void)timeout;
  return check_access();
}

std::shared_ptr<RuntimePipe> RuntimePipeWriter::pipe() const {
  return RuntimePipe::from_state(state_);
}

RuntimeTcpStream::RuntimeTcpStream(int fd, RuntimeIsolationMode isolation)
    : RuntimeIoResource(isolation), fd_(fd) {
  configure_socket_no_sigpipe(fd_);
}

RuntimeTcpStream::~RuntimeTcpStream() { (void)close(); }

RuntimeTcpConnectResult
RuntimeTcpStream::connect(const RuntimeEndpoint &endpoint,
                          std::chrono::milliseconds timeout,
                          RuntimeIsolationMode isolation) {
  RuntimeTcpConnectResult result;
  AddressList addresses;
  RuntimeIoStatus resolved =
      resolve_endpoint(endpoint, SOCK_STREAM, false, &addresses);
  if (!resolved.ok) {
    static_cast<RuntimeIoStatus &>(result) = std::move(resolved);
    return result;
  }
  const Deadline deadline = make_deadline(timeout);
  RuntimeIoStatus last = io_error("ConnectionRefusedError", "connect failed");
  for (addrinfo *address = addresses.head; address != nullptr;
       address = address->ai_next) {
    const int fd = ::socket(address->ai_family, address->ai_socktype,
                            address->ai_protocol);
    if (fd < 0) {
      last = errno_status("socket");
      continue;
    }
    (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (!set_nonblocking(fd)) {
      last = errno_status("fcntl");
      ::close(fd);
      continue;
    }
    configure_socket_no_sigpipe(fd);
    if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
      result.ok = true;
      result.stream = std::shared_ptr<RuntimeTcpStream>(
          new RuntimeTcpStream(fd, isolation));
      return result;
    }
    if (errno == EINPROGRESS) {
      RuntimeIoStatus waited =
          wait_fd(fd, POLLOUT, deadline, "TCP connect", 0, endpoint.to_string(),
                  RuntimeIoWaitInterest::Connect);
      if (waited.ok) {
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) ==
                0 &&
            socket_error == 0) {
          result.ok = true;
          result.stream = std::shared_ptr<RuntimeTcpStream>(
              new RuntimeTcpStream(fd, isolation));
          return result;
        }
        last = errno_status("connect", socket_error);
      } else {
        last = std::move(waited);
      }
    } else {
      last = errno_status("connect");
    }
    ::close(fd);
    if (last.timed_out || last.cancelled) {
      break;
    }
  }
  static_cast<RuntimeIoStatus &>(result) = std::move(last);
  return result;
}

RuntimeIoStatus RuntimeTcpStream::read(RuntimeByteBuffer &buffer,
                                       std::chrono::milliseconds timeout) {
  RuntimeIoStatus access = check_access();
  return access.ok ? fd_read_once(fd_, buffer, false, make_deadline(timeout),
                                  "TCP read", resource_id(),
                                  remote_endpoint().to_string())
                   : access;
}

RuntimeIoStatus RuntimeTcpStream::try_read(RuntimeByteBuffer &buffer) {
  RuntimeIoStatus access = check_access();
  return access.ok ? fd_read_once(fd_, buffer, true, std::nullopt, "TCP read")
                   : access;
}

RuntimeIoStatus RuntimeTcpStream::write(const std::string &bytes,
                                        std::chrono::milliseconds timeout) {
  RuntimeIoStatus access = check_access();
  return access.ok ? fd_write_once(fd_, bytes, false, make_deadline(timeout),
                                   "TCP write", true, resource_id(),
                                   remote_endpoint().to_string())
                   : access;
}

RuntimeIoStatus RuntimeTcpStream::try_write(const std::string &bytes) {
  RuntimeIoStatus access = check_access();
  return access.ok
             ? fd_write_once(fd_, bytes, true, std::nullopt, "TCP write", true)
             : access;
}

RuntimeIoStatus RuntimeTcpStream::write_all(const std::string &bytes,
                                            std::chrono::milliseconds timeout) {
  RuntimeIoStatus access = check_access();
  return access.ok ? fd_write_all(fd_, bytes, timeout, "TCP write", true,
                                  resource_id(), remote_endpoint().to_string())
                   : access;
}

RuntimeIoStatus RuntimeTcpStream::shutdown(RuntimeShutdownSide side) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  int how = SHUT_RDWR;
  if (side == RuntimeShutdownSide::Read) {
    how = SHUT_RD;
  } else if (side == RuntimeShutdownSide::Write) {
    how = SHUT_WR;
  }
  return ::shutdown(fd_, how) == 0 ? io_ok() : errno_status("shutdown");
}

RuntimeIoStatus RuntimeTcpStream::close_read() {
  return shutdown(RuntimeShutdownSide::Read);
}

RuntimeIoStatus RuntimeTcpStream::close_write() {
  return shutdown(RuntimeShutdownSide::Write);
}

RuntimeIoStatus RuntimeTcpStream::flush(std::chrono::milliseconds timeout) {
  (void)timeout;
  return check_access();
}

RuntimeIoStatus RuntimeTcpStream::set_nodelay(bool enabled) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  const int value = enabled ? 1 : 0;
  return ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) == 0
             ? io_ok()
             : errno_status("setsockopt TCP_NODELAY");
}

RuntimeIoStatus RuntimeTcpStream::set_keepalive(bool enabled) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  const int value = enabled ? 1 : 0;
  return ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value)) == 0
             ? io_ok()
             : errno_status("setsockopt SO_KEEPALIVE");
}

RuntimeIoStatus RuntimeTcpStream::set_recv_buffer(std::int64_t size) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (size <= 0 || size > std::numeric_limits<int>::max()) {
    return io_error("ArgumentError", "receive buffer size must be positive");
  }
  const int value = static_cast<int>(size);
  return ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) == 0
             ? io_ok()
             : errno_status("setsockopt SO_RCVBUF");
}

RuntimeIoStatus RuntimeTcpStream::set_send_buffer(std::int64_t size) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (size <= 0 || size > std::numeric_limits<int>::max()) {
    return io_error("ArgumentError", "send buffer size must be positive");
  }
  const int value = static_cast<int>(size);
  return ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)) == 0
             ? io_ok()
             : errno_status("setsockopt SO_SNDBUF");
}

RuntimeIoStatus RuntimeTcpStream::get_option(const std::string &name,
                                             std::int64_t *value) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (value == nullptr) {
    return io_error("TypeError", "socket option output is null");
  }
  int level = SOL_SOCKET;
  int option = 0;
  if (name == "nodelay") {
    level = IPPROTO_TCP;
    option = TCP_NODELAY;
  } else if (name == "keepalive") {
    option = SO_KEEPALIVE;
  } else if (name == "recv_buffer") {
    option = SO_RCVBUF;
  } else if (name == "send_buffer") {
    option = SO_SNDBUF;
  } else {
    return io_error("ArgumentError", "unsupported TCP stream option");
  }
  int native_value = 0;
  socklen_t length = sizeof(native_value);
  if (::getsockopt(fd_, level, option, &native_value, &length) != 0) {
    return errno_status("getsockopt");
  }
  *value = native_value;
  return io_ok();
}

RuntimeIoStatus RuntimeTcpStream::close() {
  if (closed()) {
    return io_ok();
  }
  const int fd = fd_;
  fd_ = -1;
  mark_closed();
  if (fd >= 0) {
    (void)::shutdown(fd, SHUT_RDWR);
    if (::close(fd) != 0) {
      return errno_status("close");
    }
  }
  return io_ok();
}

RuntimeEndpoint RuntimeTcpStream::local_endpoint() const {
  return fd_ < 0 ? RuntimeEndpoint{} : socket_endpoint(fd_, false);
}

RuntimeEndpoint RuntimeTcpStream::remote_endpoint() const {
  return fd_ < 0 ? RuntimeEndpoint{} : socket_endpoint(fd_, true);
}

RuntimeTcpListener::RuntimeTcpListener(int fd, RuntimeIsolationMode isolation)
    : RuntimeIoResource(isolation), isolation_mode_(isolation), fd_(fd) {}

RuntimeTcpListener::~RuntimeTcpListener() { (void)close(); }

RuntimeTcpListenResult
RuntimeTcpListener::listen(const RuntimeEndpoint &endpoint, int backlog,
                           bool reuse_addr, RuntimeIsolationMode isolation) {
  RuntimeTcpListenResult result;
  if (backlog <= 0) {
    static_cast<RuntimeIoStatus &>(result) =
        io_error("ArgumentError", "listener backlog must be positive");
    return result;
  }
  AddressList addresses;
  RuntimeIoStatus resolved =
      resolve_endpoint(endpoint, SOCK_STREAM, true, &addresses);
  if (!resolved.ok) {
    static_cast<RuntimeIoStatus &>(result) = std::move(resolved);
    return result;
  }
  RuntimeIoStatus last = io_error("IOError", "listen failed");
  for (addrinfo *address = addresses.head; address != nullptr;
       address = address->ai_next) {
    const int fd = ::socket(address->ai_family, address->ai_socktype,
                            address->ai_protocol);
    if (fd < 0) {
      last = errno_status("socket");
      continue;
    }
    (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    (void)set_nonblocking(fd);
    int reuse = reuse_addr ? 1 : 0;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (::bind(fd, address->ai_addr, address->ai_addrlen) == 0 &&
        ::listen(fd, backlog) == 0) {
      result.ok = true;
      result.listener = std::shared_ptr<RuntimeTcpListener>(
          new RuntimeTcpListener(fd, isolation));
      return result;
    }
    last = errno_status("listen");
    ::close(fd);
  }
  static_cast<RuntimeIoStatus &>(result) = std::move(last);
  return result;
}

RuntimeTcpAcceptResult
RuntimeTcpListener::accept(std::chrono::milliseconds timeout) {
  RuntimeTcpAcceptResult result;
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    static_cast<RuntimeIoStatus &>(result) = std::move(access);
    return result;
  }
  const Deadline deadline = make_deadline(timeout);
  while (true) {
    const int client = ::accept(fd_, nullptr, nullptr);
    if (client >= 0) {
      (void)::fcntl(client, F_SETFD, FD_CLOEXEC);
      (void)set_nonblocking(client);
      result.ok = true;
      result.stream = std::shared_ptr<RuntimeTcpStream>(
          new RuntimeTcpStream(client, isolation_mode_));
      return result;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      RuntimeIoStatus waited =
          wait_fd(fd_, POLLIN, deadline, "TCP accept", resource_id(),
                  local_endpoint().to_string(), RuntimeIoWaitInterest::Accept);
      if (!waited.ok) {
        static_cast<RuntimeIoStatus &>(result) = std::move(waited);
        return result;
      }
      continue;
    }
    static_cast<RuntimeIoStatus &>(result) = errno_status("accept");
    return result;
  }
}

RuntimeTcpAcceptResult RuntimeTcpListener::try_accept() {
  RuntimeTcpAcceptResult result = accept(std::chrono::milliseconds(0));
  if (result.timed_out) {
    static_cast<RuntimeIoStatus &>(result) = io_would_block();
  }
  return result;
}

RuntimeIoStatus RuntimeTcpListener::set_reuse_addr(bool enabled) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  const int value = enabled ? 1 : 0;
  return ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) == 0
             ? io_ok()
             : errno_status("setsockopt SO_REUSEADDR");
}

RuntimeIoStatus RuntimeTcpListener::set_reuse_port(bool enabled) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
#ifdef SO_REUSEPORT
  const int value = enabled ? 1 : 0;
  return ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value)) == 0
             ? io_ok()
             : errno_status("setsockopt SO_REUSEPORT");
#else
  (void)enabled;
  return io_error("UnsupportedOperationError",
                  "SO_REUSEPORT is not supported by this host");
#endif
}

RuntimeIoStatus RuntimeTcpListener::get_option(const std::string &name,
                                               std::int64_t *value) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (value == nullptr) {
    return io_error("TypeError", "socket option output is null");
  }
  int option = 0;
  if (name == "reuse_addr") {
    option = SO_REUSEADDR;
  }
#ifdef SO_REUSEPORT
  else if (name == "reuse_port") {
    option = SO_REUSEPORT;
  }
#endif
  else {
    return io_error("ArgumentError", "unsupported TCP listener option");
  }
  int native_value = 0;
  socklen_t length = sizeof(native_value);
  if (::getsockopt(fd_, SOL_SOCKET, option, &native_value, &length) != 0) {
    return errno_status("getsockopt");
  }
  *value = native_value;
  return io_ok();
}

RuntimeIoStatus RuntimeTcpListener::close() {
  if (closed()) {
    return io_ok();
  }
  const int fd = fd_;
  fd_ = -1;
  mark_closed();
  return fd < 0 || ::close(fd) == 0 ? io_ok() : errno_status("close");
}

RuntimeEndpoint RuntimeTcpListener::local_endpoint() const {
  return fd_ < 0 ? RuntimeEndpoint{} : socket_endpoint(fd_, false);
}

RuntimeUdpSocket::RuntimeUdpSocket(int fd, RuntimeIsolationMode isolation)
    : RuntimeIoResource(isolation), fd_(fd) {}

RuntimeUdpSocket::~RuntimeUdpSocket() { (void)close(); }

RuntimeUdpBindResult RuntimeUdpSocket::bind(const RuntimeEndpoint &endpoint,
                                            RuntimeIsolationMode isolation) {
  RuntimeUdpBindResult result;
  AddressList addresses;
  RuntimeIoStatus resolved =
      resolve_endpoint(endpoint, SOCK_DGRAM, true, &addresses);
  if (!resolved.ok) {
    static_cast<RuntimeIoStatus &>(result) = std::move(resolved);
    return result;
  }
  RuntimeIoStatus last = io_error("IOError", "UDP bind failed");
  for (addrinfo *address = addresses.head; address != nullptr;
       address = address->ai_next) {
    const int fd = ::socket(address->ai_family, address->ai_socktype,
                            address->ai_protocol);
    if (fd < 0) {
      last = errno_status("socket");
      continue;
    }
    (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    (void)set_nonblocking(fd);
    if (::bind(fd, address->ai_addr, address->ai_addrlen) == 0) {
      result.ok = true;
      result.socket = std::shared_ptr<RuntimeUdpSocket>(
          new RuntimeUdpSocket(fd, isolation));
      return result;
    }
    last = errno_status("UDP bind");
    ::close(fd);
  }
  static_cast<RuntimeIoStatus &>(result) = std::move(last);
  return result;
}

RuntimeUdpBindResult RuntimeUdpSocket::open(const std::string &family,
                                            RuntimeIsolationMode isolation) {
  if (family == "inet") {
    return bind({"0.0.0.0", 0}, isolation);
  }
  if (family == "inet6") {
    return bind({"::", 0}, isolation);
  }
  RuntimeUdpBindResult result;
  static_cast<RuntimeIoStatus &>(result) =
      io_error("ArgumentError", "UDP family must be inet or inet6");
  return result;
}

RuntimeDatagramResult
RuntimeUdpSocket::recv_from(std::size_t max, bool truncate,
                            std::chrono::milliseconds timeout) {
  RuntimeDatagramResult result;
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    static_cast<RuntimeIoStatus &>(result) = std::move(access);
    return result;
  }
  if (max == 0 || max > 65507) {
    static_cast<RuntimeIoStatus &>(result) =
        io_error("ArgumentError", "UDP max must be between 1 and 65507");
    return result;
  }
  const Deadline deadline = make_deadline(timeout);
  std::vector<char> bytes(max + 1U);
  while (true) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    const ssize_t count =
        ::recvfrom(fd_, bytes.data(), bytes.size(), 0,
                   reinterpret_cast<sockaddr *>(&address), &length);
    if (count >= 0) {
      if (static_cast<std::size_t>(count) > max && !truncate) {
        static_cast<RuntimeIoStatus &>(result) = io_error(
            "DatagramTooLargeError", "UDP datagram exceeds receive maximum");
        return result;
      }
      const std::size_t accepted =
          std::min(static_cast<std::size_t>(count), max);
      result.ok = true;
      result.count = accepted;
      result.bytes.assign(bytes.data(), accepted);
      result.endpoint = endpoint_from_sockaddr(
          reinterpret_cast<sockaddr *>(&address), length);
      return result;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      RuntimeIoStatus waited =
          wait_fd(fd_, POLLIN, deadline, "UDP receive", resource_id(),
                  local_endpoint().to_string(), RuntimeIoWaitInterest::Read);
      if (!waited.ok) {
        static_cast<RuntimeIoStatus &>(result) = std::move(waited);
        return result;
      }
      continue;
    }
    static_cast<RuntimeIoStatus &>(result) = errno_status("UDP receive");
    return result;
  }
}

RuntimeDatagramResult RuntimeUdpSocket::try_recv_from(std::size_t max,
                                                      bool truncate) {
  RuntimeDatagramResult result;
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    static_cast<RuntimeIoStatus &>(result) = std::move(access);
    return result;
  }
  if (max == 0 || max > 65507) {
    static_cast<RuntimeIoStatus &>(result) =
        io_error("ArgumentError", "UDP max must be between 1 and 65507");
    return result;
  }
  std::vector<char> bytes(max + 1U);
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  const ssize_t count =
      ::recvfrom(fd_, bytes.data(), bytes.size(), 0,
                 reinterpret_cast<sockaddr *>(&address), &length);
  if (count >= 0) {
    if (static_cast<std::size_t>(count) > max && !truncate) {
      static_cast<RuntimeIoStatus &>(result) = io_error(
          "DatagramTooLargeError", "UDP datagram exceeds receive maximum");
      return result;
    }
    const std::size_t accepted = std::min(static_cast<std::size_t>(count), max);
    result.ok = true;
    result.count = accepted;
    result.bytes.assign(bytes.data(), accepted);
    result.endpoint =
        endpoint_from_sockaddr(reinterpret_cast<sockaddr *>(&address), length);
    return result;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    result.would_block = true;
    return result;
  }
  static_cast<RuntimeIoStatus &>(result) = errno_status("UDP receive");
  return result;
}

RuntimeIoStatus RuntimeUdpSocket::send_to(const std::string &bytes,
                                          const RuntimeEndpoint &endpoint,
                                          std::chrono::milliseconds timeout) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  AddressList addresses;
  RuntimeIoStatus resolved =
      resolve_endpoint(endpoint, SOCK_DGRAM, false, &addresses);
  if (!resolved.ok) {
    return resolved;
  }
  const Deadline deadline = make_deadline(timeout);
  while (true) {
    const ssize_t count =
        ::sendto(fd_, bytes.data(), bytes.size(), 0, addresses.head->ai_addr,
                 addresses.head->ai_addrlen);
    if (count >= 0) {
      return io_ok(static_cast<std::size_t>(count));
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      RuntimeIoStatus waited =
          wait_fd(fd_, POLLOUT, deadline, "UDP send", resource_id(),
                  endpoint.to_string(), RuntimeIoWaitInterest::Write);
      if (!waited.ok) {
        return waited;
      }
      continue;
    }
    return errno_status("UDP send");
  }
}

RuntimeIoStatus RuntimeUdpSocket::try_send_to(const std::string &bytes,
                                              const RuntimeEndpoint &endpoint) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  AddressList addresses;
  RuntimeIoStatus resolved =
      resolve_endpoint(endpoint, SOCK_DGRAM, false, &addresses);
  if (!resolved.ok) {
    return resolved;
  }
  const ssize_t count =
      ::sendto(fd_, bytes.data(), bytes.size(), 0, addresses.head->ai_addr,
               addresses.head->ai_addrlen);
  if (count >= 0) {
    return io_ok(static_cast<std::size_t>(count));
  }
  return errno == EAGAIN || errno == EWOULDBLOCK ? io_would_block()
                                                 : errno_status("UDP send");
}

RuntimeIoStatus RuntimeUdpSocket::connect(const RuntimeEndpoint &endpoint) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  AddressList addresses;
  RuntimeIoStatus resolved =
      resolve_endpoint(endpoint, SOCK_DGRAM, false, &addresses);
  if (!resolved.ok) {
    return resolved;
  }
  return ::connect(fd_, addresses.head->ai_addr, addresses.head->ai_addrlen) ==
                 0
             ? io_ok()
             : errno_status("UDP connect");
}

RuntimeIoStatus RuntimeUdpSocket::recv(std::size_t max, bool truncate,
                                       std::chrono::milliseconds timeout,
                                       std::string *bytes) {
  RuntimeDatagramResult result = recv_from(max, truncate, timeout);
  if (result.ok && bytes != nullptr) {
    *bytes = std::move(result.bytes);
  }
  return result;
}

RuntimeIoStatus RuntimeUdpSocket::send(const std::string &bytes,
                                       std::chrono::milliseconds timeout) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  return fd_write_once(fd_, bytes, false, make_deadline(timeout), "UDP send",
                       true);
}

RuntimeIoStatus RuntimeUdpSocket::set_recv_buffer(std::int64_t size) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (size <= 0 || size > std::numeric_limits<int>::max()) {
    return io_error("ArgumentError", "receive buffer size must be positive");
  }
  const int value = static_cast<int>(size);
  return ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) == 0
             ? io_ok()
             : errno_status("setsockopt SO_RCVBUF");
}

RuntimeIoStatus RuntimeUdpSocket::set_send_buffer(std::int64_t size) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (size <= 0 || size > std::numeric_limits<int>::max()) {
    return io_error("ArgumentError", "send buffer size must be positive");
  }
  const int value = static_cast<int>(size);
  return ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)) == 0
             ? io_ok()
             : errno_status("setsockopt SO_SNDBUF");
}

RuntimeIoStatus RuntimeUdpSocket::get_option(const std::string &name,
                                             std::int64_t *value) {
  RuntimeIoStatus access = check_access();
  if (!access.ok) {
    return access;
  }
  if (value == nullptr) {
    return io_error("TypeError", "socket option output is null");
  }
  int option = 0;
  if (name == "recv_buffer") {
    option = SO_RCVBUF;
  } else if (name == "send_buffer") {
    option = SO_SNDBUF;
  } else {
    return io_error("ArgumentError", "unsupported UDP socket option");
  }
  int native_value = 0;
  socklen_t length = sizeof(native_value);
  if (::getsockopt(fd_, SOL_SOCKET, option, &native_value, &length) != 0) {
    return errno_status("getsockopt");
  }
  *value = native_value;
  return io_ok();
}

RuntimeIoStatus RuntimeUdpSocket::close() {
  if (closed()) {
    return io_ok();
  }
  const int fd = fd_;
  fd_ = -1;
  mark_closed();
  return fd < 0 || ::close(fd) == 0 ? io_ok() : errno_status("close");
}

RuntimeEndpoint RuntimeUdpSocket::local_endpoint() const {
  return fd_ < 0 ? RuntimeEndpoint{} : socket_endpoint(fd_, false);
}

std::optional<RuntimeFileMode>
runtime_file_mode_from_name(const std::string &name) {
  if (name == "read") {
    return RuntimeFileMode::Read;
  }
  if (name == "write") {
    return RuntimeFileMode::Write;
  }
  if (name == "append") {
    return RuntimeFileMode::Append;
  }
  if (name == "read_write") {
    return RuntimeFileMode::ReadWrite;
  }
  return std::nullopt;
}

std::optional<RuntimeSeekWhence>
runtime_seek_whence_from_name(const std::string &name) {
  if (name == "start") {
    return RuntimeSeekWhence::Start;
  }
  if (name == "current") {
    return RuntimeSeekWhence::Current;
  }
  if (name == "end") {
    return RuntimeSeekWhence::End;
  }
  return std::nullopt;
}

std::optional<RuntimeShutdownSide>
runtime_shutdown_side_from_name(const std::string &name) {
  if (name == "read") {
    return RuntimeShutdownSide::Read;
  }
  if (name == "write") {
    return RuntimeShutdownSide::Write;
  }
  if (name == "both") {
    return RuntimeShutdownSide::Both;
  }
  return std::nullopt;
}

std::optional<RuntimeIsolationMode>
runtime_isolation_mode_from_name(const std::string &name) {
  if (name == "checked") {
    return RuntimeIsolationMode::Checked;
  }
  if (name == "unchecked") {
    return RuntimeIsolationMode::Unchecked;
  }
  return std::nullopt;
}

RuntimeIoStatus runtime_fs_exists(const RuntimePath &path, bool *exists) {
  if (exists == nullptr) {
    return io_error("TypeError", "exists output is null");
  }
  std::error_code error;
  *exists = std::filesystem::exists(path.string(), error);
  return error ? errno_status("filesystem exists", error.value()) : io_ok();
}

RuntimeIoStatus runtime_fs_file(const RuntimePath &path, bool *file) {
  if (file == nullptr) {
    return io_error("TypeError", "file output is null");
  }
  std::error_code error;
  *file = std::filesystem::is_regular_file(path.string(), error);
  return error ? errno_status("filesystem file", error.value()) : io_ok();
}

RuntimeIoStatus runtime_fs_dir(const RuntimePath &path, bool *directory) {
  if (directory == nullptr) {
    return io_error("TypeError", "directory output is null");
  }
  std::error_code error;
  *directory = std::filesystem::is_directory(path.string(), error);
  return error ? errno_status("filesystem directory", error.value()) : io_ok();
}

RuntimeIoStatus runtime_fs_metadata(const RuntimePath &path,
                                    RuntimeMetadata *metadata) {
  if (metadata == nullptr) {
    return io_error("TypeError", "metadata output is null");
  }
  struct stat info{};
  if (::lstat(path.string().c_str(), &info) != 0) {
    return errno_status("filesystem metadata");
  }
  metadata->path = path;
  metadata->size = static_cast<std::uint64_t>(info.st_size);
  metadata->file = S_ISREG(info.st_mode);
  metadata->directory = S_ISDIR(info.st_mode);
  metadata->symlink = S_ISLNK(info.st_mode);
  return io_ok();
}

RuntimeIoStatus runtime_fs_read_bytes(const RuntimePath &path,
                                      std::optional<std::size_t> limit,
                                      std::shared_ptr<RuntimeBytes> *bytes) {
  if (bytes == nullptr) {
    return io_error("TypeError", "read_bytes output is null");
  }
  RuntimeFileOpenResult opened = RuntimeFile::open(path, RuntimeFileMode::Read);
  if (!opened.ok) {
    return opened;
  }
  RuntimeIoStatus result = opened.file->read_all(limit);
  RuntimeIoStatus close_result = opened.file->close();
  if (!result.ok) {
    return result;
  }
  if (!close_result.ok) {
    return close_result;
  }
  *bytes = std::make_shared<RuntimeBytes>(std::move(result.bytes));
  return io_ok((*bytes)->count());
}

RuntimeIoStatus runtime_fs_write_bytes(const RuntimePath &path,
                                       const std::string &bytes, bool create,
                                       bool truncate) {
  RuntimeFileOpenOptions options;
  options.create = create;
  options.truncate = truncate;
  RuntimeFileOpenResult opened =
      RuntimeFile::open(path, RuntimeFileMode::Write, options);
  if (!opened.ok) {
    return opened;
  }
  RuntimeIoStatus result = opened.file->write_all(bytes);
  if (result.ok) {
    result = opened.file->flush();
  }
  RuntimeIoStatus close_result = opened.file->close();
  return !result.ok ? result : close_result;
}

RuntimeIoStatus runtime_fs_mkdir(const RuntimePath &path) {
  std::error_code error;
  const bool created = std::filesystem::create_directory(path.string(), error);
  if (error) {
    return errno_status("mkdir", error.value());
  }
  return created ? io_ok() : io_error("FileExistsError", "path already exists");
}

RuntimeIoStatus runtime_fs_mkdir_p(const RuntimePath &path) {
  std::error_code error;
  (void)std::filesystem::create_directories(path.string(), error);
  return error ? errno_status("mkdir_p", error.value()) : io_ok();
}

RuntimeIoStatus runtime_fs_remove(const RuntimePath &path) {
  std::error_code error;
  const bool removed = std::filesystem::remove(path.string(), error);
  if (error) {
    return errno_status("remove", error.value());
  }
  return removed ? io_ok()
                 : io_error("FileNotFoundError", "path does not exist");
}

RuntimeIoStatus runtime_fs_rename(const RuntimePath &from,
                                  const RuntimePath &to) {
  std::error_code error;
  std::filesystem::rename(from.string(), to.string(), error);
  return error ? errno_status("rename", error.value()) : io_ok();
}

RuntimeIoStatus runtime_fs_copy(const RuntimePath &from, const RuntimePath &to,
                                std::size_t *count) {
  std::error_code error;
  const bool copied = std::filesystem::copy_file(
      from.string(), to.string(),
      std::filesystem::copy_options::overwrite_existing, error);
  if (error) {
    return errno_status("copy", error.value());
  }
  if (!copied) {
    return io_error("IOError", "file copy did not complete");
  }
  const std::uintmax_t size = std::filesystem::file_size(to.string(), error);
  if (error) {
    return errno_status("copy size", error.value());
  }
  if (count != nullptr) {
    *count = static_cast<std::size_t>(size);
  }
  return io_ok(static_cast<std::size_t>(size));
}

} // namespace amber::runtime
