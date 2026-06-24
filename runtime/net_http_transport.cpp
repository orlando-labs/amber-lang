#include "runtime/net_http_transport.h"

#include <string>
#include <utility>

namespace amber::runtime::http {

namespace {

// Prefer the runtime error class name, fall back to the human message.
std::string io_detail(const RuntimeIoStatus &status, const char *fallback) {
  if (!status.error_name.empty()) {
    return status.message.empty() ? status.error_name
                                  : status.error_name + ": " + status.message;
  }
  if (!status.message.empty()) {
    return status.message;
  }
  return fallback;
}

} // namespace

TcpHttpTransport::TcpHttpTransport(std::shared_ptr<RuntimeTcpStream> stream,
                                   std::chrono::milliseconds timeout)
    : stream_(std::move(stream)), timeout_(timeout), read_buffer_(64U * 1024U) {
}

bool TcpHttpTransport::write_all(const std::string &data, std::string *error) {
  const RuntimeIoStatus status = stream_->write_all(data, timeout_);
  if (!status.ok) {
    *error = io_detail(status, "failed to write request");
    return false;
  }
  return true;
}

long TcpHttpTransport::read_some(std::string *chunk, std::string *error) {
  chunk->clear();
  const RuntimeIoStatus cleared = read_buffer_.clear();
  if (!cleared.ok) {
    *error = io_detail(cleared, "read buffer reset failed");
    return -1;
  }
  const RuntimeIoStatus status = stream_->read(read_buffer_, timeout_);
  if (!status.ok && !status.eof) {
    *error = io_detail(status, "failed to read response");
    return -1;
  }
  std::string data = read_buffer_.bytes();
  if (status.eof || data.empty()) {
    return 0; // peer closed
  }
  *chunk = std::move(data);
  return static_cast<long>(chunk->size());
}

void TcpHttpTransport::close() {
  if (stream_ != nullptr) {
    stream_->close();
  }
}

std::unique_ptr<TcpHttpTransport>
http_tcp_connect(const std::string &host, std::uint16_t port,
                 std::chrono::milliseconds timeout, HttpErrorKind *kind,
                 std::string *error) {
  const RuntimeEndpoint endpoint(host, port);
  RuntimeTcpConnectResult result = RuntimeTcpStream::connect(endpoint, timeout);
  if (!result.ok || result.stream == nullptr) {
    *kind = HttpErrorKind::Connection;
    *error = io_detail(result, "connection failed");
    return nullptr;
  }
  return std::make_unique<TcpHttpTransport>(std::move(result.stream), timeout);
}

} // namespace amber::runtime::http
