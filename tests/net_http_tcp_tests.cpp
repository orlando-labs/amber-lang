// net.http TCP connector tests (DESIGN-stdlib-net-http-io-2026-06-20 Phase 2,
// §30.5 "loopback net.tcp integration tests"). Drives the real
// RuntimeTcpStream-backed transport against an in-process loopback HTTP server,
// proving the connect/serialize/parse path over an actual socket.

#include "runtime/net_http_transport.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using amber::runtime::RuntimeByteBuffer;
using amber::runtime::RuntimeTcpListener;
using amber::runtime::http::HttpErrorKind;
using amber::runtime::http::HttpExchangeResult;
using amber::runtime::http::HttpHeaders;
using amber::runtime::http::HttpRequest;

int g_checks = 0;

void expect(bool condition, const std::string &message) {
  ++g_checks;
  if (!condition) {
    std::cerr << "net.http tcp test failed: " << message << "\n";
    std::exit(1);
  }
}

void test_tcp_get_loopback() {
  auto listening = RuntimeTcpListener::listen({"127.0.0.1", 0});
  expect(listening.ok, "listen failed: " + listening.error_name);
  const std::uint16_t port = listening.listener->local_endpoint().port;
  expect(port != 0, "listener got no port");
  listening.listener->allow_unchecked_sharing();

  std::string server_error;
  std::string got_request;
  std::thread server([&] {
    auto accepted = listening.listener->accept(2s);
    if (!accepted.ok) {
      server_error = "accept:" + accepted.error_name;
      return;
    }
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos) {
      RuntimeByteBuffer buf(4096);
      auto read = accepted.stream->read(buf, 2s);
      if (read.eof) {
        break;
      }
      if (!read.ok) {
        server_error = "read:" + read.error_name;
        return;
      }
      request += buf.bytes();
    }
    got_request = request;
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 13\r\n"
                                 "\r\n"
                                 "Hello, world!";
    auto written = accepted.stream->write_all(response, 2s);
    if (!written.ok) {
      server_error = "write:" + written.error_name;
    }
    accepted.stream->close();
  });

  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  auto transport = amber::runtime::http::http_tcp_connect("127.0.0.1", port, 2s,
                                                          &kind, &error);
  expect(transport != nullptr, "connect failed: " + error);

  const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
  HttpRequest req;
  HttpHeaders headers;
  const bool built = amber::runtime::http::http_build_request(
      "GET", url, headers, "", false, true, &req, &kind, &error);
  expect(built, "build failed: " + error);

  HttpExchangeResult res = amber::runtime::http::http_perform(*transport, req);
  transport->close();
  server.join();

  expect(server_error.empty(), "server error: " + server_error);
  expect(got_request.rfind("GET / HTTP/1.1\r\n", 0) == 0,
         "server received the GET request line");
  expect(res.ok, "exchange failed: " + res.error_message);
  expect(res.status == 200, "status 200 over loopback");
  expect(res.body == "Hello, world!", "body read over loopback");
  listening.listener->close();
}

void test_tcp_connect_refused() {
  // Bind then close a listener to obtain a port with no acceptor.
  auto listening = RuntimeTcpListener::listen({"127.0.0.1", 0});
  expect(listening.ok, "listen failed: " + listening.error_name);
  const std::uint16_t port = listening.listener->local_endpoint().port;
  listening.listener->close();

  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  auto transport = amber::runtime::http::http_tcp_connect("127.0.0.1", port, 1s,
                                                          &kind, &error);
  expect(transport == nullptr, "connect to closed port should fail");
  expect(kind == HttpErrorKind::Connection,
         "refused connect -> Connection kind");
}

} // namespace

int main() {
  test_tcp_get_loopback();
  test_tcp_connect_refused();
  std::cout << "net.http tcp tests passed (" << g_checks << " checks)\n";
  return 0;
}
