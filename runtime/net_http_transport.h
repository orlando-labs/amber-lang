#pragma once

// Plaintext TCP connector for net.http (DESIGN-stdlib-net-http-io-2026-06-20
// Phase 2, the default implementation of the D2 transport seam). Bridges the
// VM-independent HttpTransport interface to runtime/io.h's RuntimeTcpStream.
// This is the only net.http source that depends on the io subsystem; the
// codec and exchange core stay free of it. A future net.https TlsConnector is
// a sibling of this file behind the same HttpTransport interface.

#include "runtime/io.h"
#include "runtime/net_http.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace amber::runtime::http {

// HttpTransport backed by a connected RuntimeTcpStream. Reads use a reusable
// internal ByteBuffer; the per-operation timeout applies to each read/write.
class TcpHttpTransport : public HttpTransport {
public:
  explicit TcpHttpTransport(
      std::shared_ptr<RuntimeTcpStream> stream,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

  bool write_all(const std::string &data, std::string *error) override;
  long read_some(std::string *chunk, std::string *error) override;
  void close() override;

  const std::shared_ptr<RuntimeTcpStream> &stream() const { return stream_; }

private:
  std::shared_ptr<RuntimeTcpStream> stream_;
  std::chrono::milliseconds timeout_;
  RuntimeByteBuffer read_buffer_;
};

// Connect a plaintext TCP transport to host:port (the connect phase of an
// exchange). On failure returns nullptr and sets *kind (Connection) / *error
// with the underlying transport detail. DNS happens inside connect; cancellable
// DNS / open_timeout is deferred per §30.4.
std::unique_ptr<TcpHttpTransport>
http_tcp_connect(const std::string &host, std::uint16_t port,
                 std::chrono::milliseconds timeout, HttpErrorKind *kind,
                 std::string *error);

} // namespace amber::runtime::http
