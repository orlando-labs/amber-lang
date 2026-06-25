// net.http exchange-core unit tests (DESIGN-stdlib-net-http-io-2026-06-20
// Phase 2). Exercises URL parsing, request building/serialization, and the
// single request/response exchange over a deterministic in-memory transport
// (design decision D2). VM-independent: links only runtime/net_http.cpp +
// runtime/http_codec.cpp.

#include "runtime/net_http.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace {

using amber::runtime::http::HttpErrorKind;
using amber::runtime::http::HttpExchangeResult;
using amber::runtime::http::HttpHeaders;
using amber::runtime::http::HttpRequest;
using amber::runtime::http::HttpTransport;
using amber::runtime::http::HttpUrl;

int g_checks = 0;

void expect(bool condition, const std::string &message) {
  ++g_checks;
  if (!condition) {
    std::cerr << "net.http test failed: " << message << "\n";
    std::exit(1);
  }
}

// Deterministic transport: captures written request bytes and replays a canned
// response, optionally paced in small reads to exercise incremental parsing.
class FakeTransport : public HttpTransport {
public:
  std::string written;
  std::string to_read;
  std::size_t read_pos = 0;
  std::size_t read_chunk = std::string::npos; // bytes per read_some
  bool fail_write = false;
  bool fail_read = false;
  bool closed = false;
  std::shared_ptr<bool> closed_flag;

  bool write_all(const std::string &data, std::string *error) override {
    if (fail_write) {
      *error = "boom-write";
      return false;
    }
    written += data;
    return true;
  }

  long read_some(std::string *chunk, std::string *error) override {
    chunk->clear();
    if (fail_read) {
      *error = "boom-read";
      return -1;
    }
    if (read_pos >= to_read.size()) {
      return 0; // EOF
    }
    const std::size_t n = std::min(read_chunk, to_read.size() - read_pos);
    chunk->assign(to_read, read_pos, n);
    read_pos += n;
    return static_cast<long>(n);
  }

  void close() override {
    closed = true;
    if (closed_flag != nullptr) {
      *closed_flag = true;
    }
  }
};

HttpRequest build_or_die(const std::string &method, const std::string &url,
                         const HttpHeaders &headers, const std::string &body,
                         bool has_body, const std::string &what) {
  HttpRequest req;
  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  const bool ok = amber::runtime::http::http_build_request(
      method, url, headers, body, has_body, /*auto_host=*/true, &req, &kind,
      &error);
  expect(ok, what + " should build: " + error);
  return req;
}

void expect_build_error(const std::string &method, const std::string &url,
                        const HttpHeaders &headers, const std::string &body,
                        bool has_body, HttpErrorKind expected,
                        const std::string &what) {
  HttpRequest req;
  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  const bool ok = amber::runtime::http::http_build_request(
      method, url, headers, body, has_body, true, &req, &kind, &error);
  expect(!ok, what + " should fail");
  expect(kind == expected, what + " should fail with the expected kind");
}

// ---------------------------------------------------------------------------
// URL parsing (§19)
// ---------------------------------------------------------------------------

void test_url_parse_basic() {
  HttpUrl url;
  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  expect(amber::runtime::http::http_parse_url("http://example.com/items?q=1",
                                              &url, &kind, &error),
         "basic url parses");
  expect(url.scheme == "http", "scheme http");
  expect(url.host == "example.com", "host");
  expect(url.port == 80, "default port 80");
  expect(url.target == "/items?q=1", "origin-form target with query");
}

void test_url_parse_port_and_default_path() {
  HttpUrl url;
  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  expect(amber::runtime::http::http_parse_url("http://example.com:8080", &url,
                                              &kind, &error),
         "url with port parses");
  expect(url.port == 8080, "explicit port");
  expect(url.target == "/", "default path is /");

  HttpUrl q;
  expect(
      amber::runtime::http::http_parse_url("http://h?a=b", &q, &kind, &error),
      "url with bare query parses");
  expect(q.target == "/?a=b", "query without path gets default path");
}

void test_url_parse_strips_fragment() {
  HttpUrl url;
  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  expect(amber::runtime::http::http_parse_url("http://example.com/p?q=1#frag",
                                              &url, &kind, &error),
         "url with fragment parses");
  expect(url.target == "/p?q=1", "fragment stripped from wire target");
}

void test_url_parse_rejections() {
  HttpUrl url;
  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  expect(!amber::runtime::http::http_parse_url("https://example.com/", &url,
                                               &kind, &error) &&
             kind == HttpErrorKind::UnsupportedScheme,
         "https rejected as unsupported scheme");
  expect(!amber::runtime::http::http_parse_url("http://user@example.com/", &url,
                                               &kind, &error) &&
             kind == HttpErrorKind::InvalidUrl,
         "userinfo rejected");
  expect(!amber::runtime::http::http_parse_url("http:///path", &url, &kind,
                                               &error) &&
             kind == HttpErrorKind::InvalidUrl,
         "missing host rejected");
  expect(
      !amber::runtime::http::http_parse_url("notaurl", &url, &kind, &error) &&
          kind == HttpErrorKind::InvalidUrl,
      "scheme-less rejected");
}

// ---------------------------------------------------------------------------
// Request building + serialization (§18.1)
// ---------------------------------------------------------------------------

void test_build_get_synthesizes_host_no_body() {
  HttpHeaders headers;
  HttpRequest req = build_or_die("get", "http://example.com/items?q=1", headers,
                                 "", false, "GET");
  expect(req.method == "GET", "method uppercased");
  expect(req.host == "example.com" && req.port == 80, "origin");
  expect(req.headers.first("host").value() == "example.com",
         "Host synthesized");
  expect(!req.headers.contains("content-length"),
         "no Content-Length without body");

  FakeTransport t;
  t.to_read = "HTTP/1.1 204 No Content\r\n\r\n";
  HttpExchangeResult res = amber::runtime::http::http_perform(t, req);
  expect(res.ok, "GET exchange ok");
  expect(t.written == "GET /items?q=1 HTTP/1.1\r\n"
                      "host: example.com\r\n\r\n",
         "GET request serialized in origin-form with Host");
  expect(res.status == 204, "status 204");
}

void test_build_post_synthesizes_content_length() {
  HttpHeaders headers;
  std::string error;
  headers.add("content-type", "application/json", &error);
  HttpRequest req = build_or_die("POST", "http://example.com/submit", headers,
                                 "{}", true, "POST");
  expect(req.headers.first("content-length").value() == "2",
         "Content-Length synthesized from body");

  FakeTransport t;
  t.to_read = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nyes";
  HttpExchangeResult res = amber::runtime::http::http_perform(t, req);
  expect(res.ok, "POST exchange ok");
  expect(t.written == "POST /submit HTTP/1.1\r\n"
                      "content-type: application/json\r\n"
                      "host: example.com\r\n"
                      "content-length: 2\r\n\r\n{}",
         "POST request serialized with body and synthesized headers");
  expect(res.body == "yes", "response body read");
}

void test_build_host_includes_nondefault_port() {
  HttpHeaders headers;
  HttpRequest req = build_or_die("GET", "http://example.com:8080/", headers, "",
                                 false, "GET");
  expect(req.headers.first("host").value() == "example.com:8080",
         "Host includes non-default port");
}

void test_build_respects_caller_host() {
  HttpHeaders headers;
  std::string error;
  headers.add("host", "override.example", &error);
  HttpRequest req =
      build_or_die("GET", "http://example.com/", headers, "", false, "GET");
  expect(req.headers.all("host").size() == 1U, "single Host line");
  expect(req.headers.first("host").value() == "override.example",
         "caller Host preserved, not overwritten");
}

void test_build_content_length_match_and_mismatch() {
  HttpHeaders match;
  std::string error;
  match.add("content-length", "2", &error);
  HttpRequest req =
      build_or_die("POST", "http://h/x", match, "{}", true, "matching CL");
  expect(req.headers.first("content-length").value() == "2", "CL preserved");

  HttpHeaders mismatch;
  mismatch.add("content-length", "5", &error);
  expect_build_error("POST", "http://h/x", mismatch, "{}", true,
                     HttpErrorKind::InvalidHeader, "mismatched CL");
}

void test_build_rejects_transfer_encoding_and_bad_method() {
  HttpHeaders te;
  std::string error;
  te.add("transfer-encoding", "chunked", &error);
  expect_build_error("POST", "http://h/x", te, "", false,
                     HttpErrorKind::InvalidHeader, "caller TE");
  HttpHeaders empty;
  expect_build_error("BAD METHOD", "http://h/x", empty, "", false,
                     HttpErrorKind::InvalidMethod, "bad method");
}

// ---------------------------------------------------------------------------
// Exchange behaviour (§25.4 response bodies, transport errors)
// ---------------------------------------------------------------------------

void test_exchange_chunked_response() {
  HttpHeaders headers;
  HttpRequest req = build_or_die("GET", "http://h/", headers, "", false, "GET");
  FakeTransport t;
  t.to_read = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
              "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
  HttpExchangeResult res = amber::runtime::http::http_perform(t, req);
  expect(res.ok, "chunked exchange ok");
  expect(res.body == "Wikipedia", "chunked body reassembled");
}

void test_exchange_incremental_reads() {
  HttpHeaders headers;
  HttpRequest req = build_or_die("GET", "http://h/", headers, "", false, "GET");
  FakeTransport t;
  t.to_read = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  t.read_chunk = 1U; // one byte per read
  HttpExchangeResult res = amber::runtime::http::http_perform(t, req);
  expect(res.ok, "byte-paced exchange ok");
  expect(res.body == "hello", "body assembled from one-byte reads");
}

void test_exchange_head_suppresses_body() {
  HttpHeaders headers;
  HttpRequest req =
      build_or_die("HEAD", "http://h/", headers, "", false, "HEAD");
  FakeTransport t;
  t.to_read = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n";
  HttpExchangeResult res =
      amber::runtime::http::http_perform(t, req, /*head_request=*/true);
  expect(res.ok, "HEAD exchange ok without reading 100 body bytes");
  expect(res.body.empty(), "HEAD body suppressed");
}

void test_exchange_close_delimited_body() {
  HttpHeaders headers;
  HttpRequest req = build_or_die("GET", "http://h/", headers, "", false, "GET");
  FakeTransport t;
  t.to_read = "HTTP/1.1 200 OK\r\n\r\nstreamed payload"; // no CL, no chunked
  HttpExchangeResult res = amber::runtime::http::http_perform(t, req);
  expect(res.ok, "close-delimited exchange ok");
  expect(res.body == "streamed payload", "close-delimited body via EOF");
}

void test_exchange_transport_errors() {
  HttpHeaders headers;
  HttpRequest req = build_or_die("GET", "http://h/", headers, "", false, "GET");

  FakeTransport wfail;
  wfail.fail_write = true;
  HttpExchangeResult wr = amber::runtime::http::http_perform(wfail, req);
  expect(!wr.ok && wr.error_kind == HttpErrorKind::Connection,
         "write failure -> ConnectionError");

  FakeTransport rfail;
  rfail.fail_read = true;
  HttpExchangeResult rr = amber::runtime::http::http_perform(rfail, req);
  expect(!rr.ok && rr.error_kind == HttpErrorKind::Connection,
         "read failure -> ConnectionError");
}

void test_exchange_truncated_body() {
  HttpHeaders headers;
  HttpRequest req = build_or_die("GET", "http://h/", headers, "", false, "GET");
  FakeTransport t;
  t.to_read = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort"; // 5 < 10
  HttpExchangeResult res = amber::runtime::http::http_perform(t, req);
  expect(!res.ok && res.error_kind == HttpErrorKind::UnexpectedEof,
         "truncated body -> UnexpectedEof");
}

// ---------------------------------------------------------------------------
// Phase 3 request/response streaming
// ---------------------------------------------------------------------------

void test_streaming_fixed_length_request() {
  HttpHeaders headers;
  HttpRequest req;
  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  expect(amber::runtime::http::http_build_streaming_request(
             "POST", "http://h/upload", headers, 5U, true, &req, &kind,
             &error),
         "fixed streaming request builds");
  FakeTransport t;
  std::uint64_t written = 0;
  expect(amber::runtime::http::http_write_request_head(t, req, &kind, &error),
         "fixed streaming head writes");
  expect(amber::runtime::http::http_write_request_body_chunk(
             t, req, "he", &written, &kind, &error),
         "fixed streaming first chunk writes");
  expect(amber::runtime::http::http_write_request_body_chunk(
             t, req, "llo", &written, &kind, &error),
         "fixed streaming second chunk writes");
  expect(amber::runtime::http::http_finish_request_body(t, req, written, &kind,
                                                        &error),
         "fixed streaming finish validates length");
  expect(t.written == "POST /upload HTTP/1.1\r\n"
                      "host: h\r\n"
                      "content-length: 5\r\n\r\n"
                      "hello",
         "fixed streaming wire bytes");
}

void test_streaming_chunked_request() {
  HttpHeaders headers;
  HttpRequest req;
  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  expect(amber::runtime::http::http_build_streaming_request(
             "POST", "http://h/upload", headers, std::nullopt, true, &req,
             &kind, &error),
         "chunked streaming request builds");
  FakeTransport t;
  std::uint64_t written = 0;
  expect(amber::runtime::http::http_write_request_head(t, req, &kind, &error),
         "chunked streaming head writes");
  expect(amber::runtime::http::http_write_request_body_chunk(
             t, req, "abc", &written, &kind, &error),
         "chunked streaming chunk writes");
  expect(amber::runtime::http::http_finish_request_body(t, req, written, &kind,
                                                        &error),
         "chunked streaming finish writes terminator");
  expect(t.written == "POST /upload HTTP/1.1\r\n"
                      "host: h\r\n"
                      "transfer-encoding: chunked\r\n\r\n"
                      "3\r\nabc\r\n0\r\n\r\n",
         "chunked streaming wire bytes");
}

void test_streaming_fixed_length_mismatch() {
  HttpHeaders headers;
  HttpRequest req;
  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  expect(amber::runtime::http::http_build_streaming_request(
             "POST", "http://h/upload", headers, 4U, true, &req, &kind,
             &error),
         "fixed streaming mismatch request builds");
  FakeTransport t;
  std::uint64_t written = 0;
  expect(amber::runtime::http::http_write_request_head(t, req, &kind, &error),
         "fixed mismatch head writes");
  expect(!amber::runtime::http::http_write_request_body_chunk(
             t, req, "abcde", &written, &kind, &error) &&
             kind == HttpErrorKind::BodyLength,
         "fixed streaming overwrite -> BodyLength");
  expect(!amber::runtime::http::http_finish_request_body(t, req, written, &kind,
                                                         &error) &&
             kind == HttpErrorKind::BodyLength,
         "fixed streaming underwrite -> BodyLength");
}

void test_response_start_streams_body() {
  auto transport = std::make_unique<FakeTransport>();
  std::shared_ptr<bool> closed = std::make_shared<bool>(false);
  transport->closed_flag = closed;
  transport->to_read =
      "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world";
  transport->read_chunk = 9;
  amber::runtime::http::HttpResponseStartResult started =
      amber::runtime::http::http_read_response_start(std::move(transport));
  expect(started.ok, "response start ok: " + started.error_message);
  expect(started.status == 200, "streaming response status");
  std::string first;
  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  expect(started.body->read(5, &first, &kind, &error),
         "streaming response first read");
  expect(first == "hello", "first streaming body chunk");
  std::string rest;
  expect(started.body->read_all(std::nullopt, &rest, &kind, &error),
         "streaming response read_all rest");
  expect(rest == " world", "remaining streaming body bytes");
  expect(started.body->consumed(), "body consumed after read_all");
  expect(*closed, "transport closed when body lease releases");
}

void test_response_body_release_callback_reusable_on_drain() {
  auto transport = std::make_unique<FakeTransport>();
  std::shared_ptr<bool> closed = std::make_shared<bool>(false);
  transport->closed_flag = closed;

  amber::runtime::http::HttpResponseParser parser;
  expect(parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"),
         "parser accepts complete response");

  int releases = 0;
  bool reusable = false;
  amber::runtime::http::HttpResponseBodyStream body(
      std::move(transport), std::move(parser),
      [&](std::unique_ptr<HttpTransport> released, bool can_reuse) {
        ++releases;
        reusable = can_reuse;
        released->close();
      });

  std::string bytes;
  HttpErrorKind kind = HttpErrorKind::None;
  std::string error;
  expect(body.read_all(std::nullopt, &bytes, &kind, &error),
         "draining body succeeds");
  expect(bytes == "ok", "drained body bytes");
  expect(releases == 1, "drained body releases exactly once");
  expect(reusable, "drained body releases reusable transport");
  expect(*closed, "release callback owns and closes transport");
}

void test_response_body_release_callback_non_reusable_on_close() {
  auto transport = std::make_unique<FakeTransport>();
  std::shared_ptr<bool> closed = std::make_shared<bool>(false);
  transport->closed_flag = closed;

  amber::runtime::http::HttpResponseParser parser;
  expect(parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nte"),
         "parser accepts partial response body");

  int releases = 0;
  bool reusable = true;
  amber::runtime::http::HttpResponseBodyStream body(
      std::move(transport), std::move(parser),
      [&](std::unique_ptr<HttpTransport> released, bool can_reuse) {
        ++releases;
        reusable = can_reuse;
        released->close();
      });

  body.close();
  body.close();
  expect(releases == 1, "early close releases exactly once");
  expect(!reusable, "early close releases non-reusable transport");
  expect(*closed, "early close callback owns and closes transport");
}

} // namespace

int main() {
  test_url_parse_basic();
  test_url_parse_port_and_default_path();
  test_url_parse_strips_fragment();
  test_url_parse_rejections();

  test_build_get_synthesizes_host_no_body();
  test_build_post_synthesizes_content_length();
  test_build_host_includes_nondefault_port();
  test_build_respects_caller_host();
  test_build_content_length_match_and_mismatch();
  test_build_rejects_transfer_encoding_and_bad_method();

  test_exchange_chunked_response();
  test_exchange_incremental_reads();
  test_exchange_head_suppresses_body();
  test_exchange_close_delimited_body();
  test_exchange_transport_errors();
  test_exchange_truncated_body();
  test_streaming_fixed_length_request();
  test_streaming_chunked_request();
  test_streaming_fixed_length_mismatch();
  test_response_start_streams_body();
  test_response_body_release_callback_reusable_on_drain();
  test_response_body_release_callback_non_reusable_on_close();

  std::cout << "net.http exchange tests passed (" << g_checks << " checks)\n";
  return 0;
}
