// HTTP/1.1 wire-codec unit tests (DESIGN-stdlib-net-http-io-2026-06-20 Phase
// 1).
//
// Covers conformance matrix §25.2 (headers) and §25.6 (protocol parser) plus
// the request serializer and chunked/length/close body decoders. The codec is
// VM-independent, so these tests link only runtime/http_codec.cpp.

#include "runtime/http_codec.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using amber::runtime::http::HttpErrorKind;
using amber::runtime::http::HttpChunkExtension;
using amber::runtime::http::HttpHeaders;
using amber::runtime::http::HttpResponseParser;
using amber::runtime::http::HttpResponseParserLimits;

int g_checks = 0;

void expect(bool condition, const std::string &message) {
  ++g_checks;
  if (!condition) {
    std::cerr << "http codec test failed: " << message << "\n";
    std::exit(1);
  }
}

// Feed an entire response in one shot.
HttpResponseParser parse_all(const std::string &bytes, bool finish = true,
                             bool head = false,
                             HttpResponseParserLimits limits = {}) {
  HttpResponseParser parser(limits, head);
  parser.feed(bytes);
  if (finish && !parser.has_error() && !parser.message_complete()) {
    parser.finish();
  }
  return parser;
}

// ---------------------------------------------------------------------------
// §25.2 Headers
// ---------------------------------------------------------------------------

void test_headers_case_insensitive_and_canonical() {
  HttpHeaders headers;
  std::string error;
  expect(headers.add("Content-Type", "application/json", &error),
         "add valid header");
  // Case-insensitive lookup.
  expect(headers.contains("content-type"), "contains lower");
  expect(headers.contains("CONTENT-TYPE"), "contains upper");
  expect(headers.first("Content-Type").value() == "application/json",
         "first lookup");
  // Lowercase canonical storage.
  expect(headers.pairs().size() == 1U, "one pair");
  expect(headers.pairs()[0].first == "content-type",
         "stored name is lowercase canonical");
}

void test_headers_duplicate_and_order_preserved() {
  HttpHeaders headers;
  std::string error;
  expect(headers.add("x-tag", "one", &error), "add x-tag one");
  expect(headers.add("accept", "text/html", &error), "add accept");
  expect(headers.add("x-tag", "two", &error), "add x-tag two");
  // Duplicate preservation + field-line order.
  const auto all = headers.all("x-tag");
  expect(all.size() == 2U && all[0] == "one" && all[1] == "two",
         "all preserves duplicates in order");
  expect(headers.first("x-tag").value() == "one", "first is earliest line");
  // to_pairs order preservation.
  const auto &pairs = headers.pairs();
  expect(pairs.size() == 3U, "three lines");
  expect(pairs[0].first == "x-tag" && pairs[1].first == "accept" &&
             pairs[2].first == "x-tag",
         "field-line order preserved");
}

void test_headers_set_replaces_all() {
  HttpHeaders headers;
  std::string error;
  headers.add("x-tag", "one", &error);
  headers.add("x-tag", "two", &error);
  expect(headers.set("x-tag", "replacement", &error), "set x-tag");
  const auto all = headers.all("x-tag");
  expect(all.size() == 1U && all[0] == "replacement", "set! replaces all");
}

void test_headers_value_trimmed() {
  HttpHeaders headers;
  std::string error;
  expect(headers.add("accept", "  application/json  ", &error), "add padded");
  expect(headers.first("accept").value() == "application/json",
         "OWS trimmed from value");
}

void test_headers_reject_invalid_names() {
  HttpHeaders headers;
  std::string error;
  expect(!headers.add("bad name", "x", &error), "reject space in name");
  expect(!headers.add("", "x", &error), "reject empty name");
  expect(!headers.add("bad:name", "x", &error), "reject colon in name");
  expect(!headers.add("bad\x01name", "x", &error), "reject control in name");
  expect(headers.empty(), "no invalid header stored");
}

void test_headers_reject_crlf_nul_values() {
  HttpHeaders headers;
  std::string error;
  expect(!headers.add("x", std::string("a\rb"), &error), "reject CR");
  expect(!headers.add("x", std::string("a\nb"), &error), "reject LF");
  // Obsolete folding presents as CRLF inside a value, caught by the same rule.
  expect(!headers.add("x", std::string("a\r\n b"), &error),
         "reject obsolete folded value");
  expect(!headers.add("x", std::string("a\0b", 3), &error), "reject NUL");
  expect(headers.empty(), "no invalid value stored");
}

void test_headers_no_hidden_comma_folding() {
  HttpHeaders headers;
  std::string error;
  headers.add("set-cookie", "a=1", &error);
  headers.add("set-cookie", "b=2", &error);
  // Two distinct lines, never folded into one comma value.
  expect(headers.all("set-cookie").size() == 2U,
         "set-cookie kept as two lines");
  expect(!HttpHeaders::is_list_combinable("set-cookie"),
         "set-cookie is not list-combinable");
  expect(HttpHeaders::is_list_combinable("accept"),
         "accept is list-combinable");
  // combined() is opt-in only; it never runs implicitly.
  headers.add("accept", "text/html", &error);
  headers.add("accept", "application/json", &error);
  expect(headers.combined("accept").value() == "text/html, application/json",
         "explicit combine joins with comma");
}

// ---------------------------------------------------------------------------
// §25.6 Protocol parser
// ---------------------------------------------------------------------------

void test_valid_status_line_and_headers() {
  HttpResponseParser parser = parse_all("HTTP/1.1 200 OK\r\n"
                                        "Content-Type: text/plain\r\n"
                                        "Content-Length: 5\r\n"
                                        "\r\n"
                                        "hello");
  expect(!parser.has_error(), "no error");
  expect(parser.message_complete(), "complete");
  expect(parser.status() == 200, "status 200");
  expect(parser.reason() == "OK", "reason OK");
  expect(parser.minor_version() == 1, "minor 1");
  expect(parser.headers().first("content-type").value() == "text/plain",
         "header parsed");
  expect(parser.body() == "hello", "content-length body");
}

void test_status_line_http_1_0_and_empty_reason() {
  HttpResponseParser parser = parse_all("HTTP/1.0 204 \r\n\r\n");
  expect(!parser.has_error(), "no error 1.0");
  expect(parser.status() == 204, "status 204");
  expect(parser.minor_version() == 0, "minor 0");
  expect(parser.reason().empty(), "empty reason");
  expect(parser.message_complete(), "204 complete with no body");
}

void test_status_line_no_trailing_space() {
  // A status line ending right after the 3-digit code (no SP, no reason).
  HttpResponseParser parser =
      parse_all("HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n");
  expect(!parser.has_error(), "no error code-only status line");
  expect(parser.status() == 200, "status 200 code-only");
}

void test_oversized_status_line() {
  HttpResponseParserLimits limits;
  limits.max_status_line_bytes = 16;
  HttpResponseParser parser(limits);
  parser.feed("HTTP/1.1 200 This reason phrase is definitely too long\r\n");
  expect(parser.has_error(), "oversized status line errors");
  expect(parser.error_kind() == HttpErrorKind::StatusLineLimit,
         "StatusLineLimit kind");
}

void test_oversized_header_section() {
  HttpResponseParserLimits limits;
  limits.max_header_bytes = 32;
  HttpResponseParser parser(limits);
  parser.feed("HTTP/1.1 200 OK\r\n"
              "X-Long-Header: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
              "\r\n");
  expect(parser.has_error(), "oversized header section errors");
  expect(parser.error_kind() == HttpErrorKind::HeaderLimit, "HeaderLimit kind");
}

void test_malformed_crlf() {
  HttpResponseParser parser;
  parser.feed("HTTP/1.1 200 OK\nContent-Length: 0\r\n\r\n"); // bare LF
  expect(parser.has_error(), "bare LF status line errors");
  expect(parser.error_kind() == HttpErrorKind::Protocol, "Protocol kind");

  HttpResponseParser parser2;
  parser2.feed("HTTP/1.1 200 OK\r\nContent-Length: 0\nfoo\r\n\r\n"); // bare LF
  expect(parser2.has_error(), "bare LF header errors");
}

void test_duplicate_equal_content_length() {
  HttpResponseParser parser = parse_all("HTTP/1.1 200 OK\r\n"
                                        "Content-Length: 3\r\n"
                                        "Content-Length: 3\r\n"
                                        "\r\n"
                                        "abc");
  expect(!parser.has_error(), "equal duplicate CL accepted");
  expect(parser.body() == "abc", "body decoded with equal CL");
}

void test_duplicate_conflicting_content_length() {
  HttpResponseParser parser = parse_all("HTTP/1.1 200 OK\r\n"
                                        "Content-Length: 3\r\n"
                                        "Content-Length: 4\r\n"
                                        "\r\n",
                                        /*finish=*/false);
  expect(parser.has_error(), "conflicting CL errors");
  expect(parser.error_kind() == HttpErrorKind::Protocol, "Protocol kind");
}

void test_list_content_length_conflict() {
  HttpResponseParser parser = parse_all("HTTP/1.1 200 OK\r\n"
                                        "Content-Length: 3, 4\r\n"
                                        "\r\n",
                                        /*finish=*/false);
  expect(parser.has_error(), "comma-list conflicting CL errors");
}

void test_transfer_encoding_plus_content_length() {
  HttpResponseParser parser = parse_all("HTTP/1.1 200 OK\r\n"
                                        "Transfer-Encoding: chunked\r\n"
                                        "Content-Length: 5\r\n"
                                        "\r\n",
                                        /*finish=*/false);
  expect(parser.has_error(), "TE + CL errors");
  expect(parser.error_kind() == HttpErrorKind::Protocol, "Protocol kind");
}

void test_unsupported_transfer_coding() {
  HttpResponseParser parser = parse_all("HTTP/1.1 200 OK\r\n"
                                        "Transfer-Encoding: gzip\r\n"
                                        "\r\n",
                                        /*finish=*/false);
  expect(parser.has_error(), "unsupported coding errors");
  expect(parser.error_kind() == HttpErrorKind::Protocol, "Protocol kind");
}

void test_chunked_body_with_extensions_and_trailers() {
  HttpResponseParser parser = parse_all("HTTP/1.1 200 OK\r\n"
                                        "Transfer-Encoding: chunked\r\n"
                                        "Trailer: X-Checksum\r\n"
                                        "\r\n"
                                        "5;name=value\r\n"
                                        "hello\r\n"
                                        "6\r\n"
                                        " world\r\n"
                                        "0\r\n"
                                        "X-Checksum: abc123\r\n"
                                        "\r\n");
  expect(!parser.has_error(), "chunked no error");
  expect(parser.message_complete(), "chunked complete");
  expect(parser.body() == "hello world", "chunked body reassembled");
  expect(parser.chunk_extensions().size() == 1U &&
             parser.chunk_extensions()[0] == "name=value",
         "chunk extension captured");
  expect(parser.trailers().first("x-checksum").value() == "abc123",
         "trailer parsed");
}

void test_invalid_chunk_size() {
  HttpResponseParser parser = parse_all("HTTP/1.1 200 OK\r\n"
                                        "Transfer-Encoding: chunked\r\n"
                                        "\r\n"
                                        "zz\r\n",
                                        /*finish=*/false);
  expect(parser.has_error(), "invalid chunk size errors");
  expect(parser.error_kind() == HttpErrorKind::Chunk, "Chunk kind");
}

void test_oversized_chunk_header() {
  HttpResponseParserLimits limits;
  limits.max_chunk_header_bytes = 4;
  HttpResponseParser parser(limits);
  parser.feed("HTTP/1.1 200 OK\r\n"
              "Transfer-Encoding: chunked\r\n"
              "\r\n"
              "5;aaaaaaaaaaaaaaaa\r\n");
  expect(parser.has_error(), "oversized chunk header errors");
  expect(parser.error_kind() == HttpErrorKind::Chunk, "Chunk kind");
}

void test_unexpected_eof() {
  HttpResponseParser parser;
  parser.feed("HTTP/1.1 200 OK\r\n"
              "Content-Length: 10\r\n"
              "\r\n"
              "short");
  expect(!parser.has_error(), "no error before eof");
  parser.finish();
  expect(parser.has_error(), "eof mid-body errors");
  expect(parser.error_kind() == HttpErrorKind::UnexpectedEof,
         "UnexpectedEof kind");
}

void test_unexpected_eof_in_headers() {
  HttpResponseParser parser;
  parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n");
  parser.finish();
  expect(parser.has_error(), "eof mid-headers errors");
  expect(parser.error_kind() == HttpErrorKind::UnexpectedEof,
         "UnexpectedEof kind in headers");
}

void test_informational_1xx_before_final() {
  HttpResponseParser parser = parse_all("HTTP/1.1 100 Continue\r\n"
                                        "\r\n"
                                        "HTTP/1.1 200 OK\r\n"
                                        "Content-Length: 2\r\n"
                                        "\r\n"
                                        "hi");
  expect(!parser.has_error(), "1xx skip no error");
  expect(parser.status() == 200, "final status after 1xx");
  expect(parser.body() == "hi", "final body after 1xx");
}

void test_unsupported_101() {
  HttpResponseParser parser = parse_all("HTTP/1.1 101 Switching Protocols\r\n"
                                        "\r\n",
                                        /*finish=*/false);
  expect(parser.has_error(), "101 errors");
  expect(parser.error_kind() == HttpErrorKind::Protocol, "Protocol kind");
}

void test_head_response_no_body() {
  HttpResponseParser parser = parse_all("HTTP/1.1 200 OK\r\n"
                                        "Content-Length: 100\r\n"
                                        "\r\n",
                                        /*finish=*/false,
                                        /*head=*/true);
  expect(!parser.has_error(), "HEAD no error");
  expect(parser.message_complete(), "HEAD complete despite Content-Length");
  expect(parser.body().empty(), "HEAD body suppressed");
}

void test_304_no_body() {
  HttpResponseParser parser = parse_all("HTTP/1.1 304 Not Modified\r\n"
                                        "Content-Length: 50\r\n"
                                        "\r\n",
                                        /*finish=*/false);
  expect(!parser.has_error(), "304 no error");
  expect(parser.message_complete(), "304 complete, body suppressed");
  expect(parser.body().empty(), "304 body suppressed");
}

void test_close_delimited_body() {
  HttpResponseParser parser;
  parser.feed("HTTP/1.1 200 OK\r\n\r\n");
  parser.feed("streamed ");
  parser.feed("payload");
  expect(!parser.message_complete(), "close-delimited not complete until eof");
  parser.finish();
  expect(!parser.has_error(), "close-delimited no error");
  expect(parser.message_complete(), "complete after eof");
  expect(parser.body() == "streamed payload", "close-delimited body");
}

void test_incremental_byte_at_a_time() {
  // Feeding one byte at a time must produce the same parse as feeding all.
  const std::string raw = "HTTP/1.1 200 OK\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "4\r\nWiki\r\n"
                          "5\r\npedia\r\n"
                          "0\r\n\r\n";
  HttpResponseParser parser;
  for (char c : raw) {
    parser.feed(&c, 1U);
    expect(!parser.has_error(), "no error during incremental feed");
  }
  expect(parser.message_complete(), "incremental complete");
  expect(parser.body() == "Wikipedia", "incremental chunked body");
}

void test_take_body_drains() {
  HttpResponseParser parser =
      parse_all("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nxyz");
  expect(parser.take_body() == "xyz", "take_body returns body");
  expect(parser.body().empty(), "take_body drained body");
}

// ---------------------------------------------------------------------------
// Request serialization (§18.1)
// ---------------------------------------------------------------------------

void test_serialize_request_head() {
  HttpHeaders headers;
  std::string error;
  headers.add("Host", "example.com", &error);
  headers.add("Accept", "*/*", &error);
  const std::string head = amber::runtime::http::http_serialize_request_head(
      "GET", "/items?q=amber", headers);
  expect(head == "GET /items?q=amber HTTP/1.1\r\n"
                 "host: example.com\r\n"
                 "accept: */*\r\n"
                 "\r\n",
         "request head serialized in origin-form with canonical headers");
}

void test_normalize_method() {
  std::string out;
  expect(amber::runtime::http::http_normalize_method("post", &out) &&
             out == "POST",
         "method uppercased");
  expect(!amber::runtime::http::http_normalize_method("bad method", &out),
         "method with space rejected");
  expect(!amber::runtime::http::http_normalize_method("", &out),
         "empty method rejected");
}

void test_encode_chunks() {
  expect(amber::runtime::http::http_encode_chunk("hello") == "5\r\nhello\r\n",
         "chunk encoded with hex size");
  expect(amber::runtime::http::http_encode_chunk("").empty(),
         "empty data encodes to nothing");
  expect(amber::runtime::http::http_encode_last_chunk() == "0\r\n\r\n",
         "last chunk terminator");
  // 16-byte chunk -> hex "10".
  expect(amber::runtime::http::http_encode_chunk(std::string(16, 'a'))
                 .rfind("10\r\n", 0) == 0,
         "16-byte chunk uses hex size 10");
}

void test_chunk_extensions_and_trailers() {
  std::vector<HttpChunkExtension> extensions = {
      {"trace", std::string("abc")},
      {"flag", std::nullopt},
      {"trace", std::string("quoted value\\\"")}};
  std::string wire;
  std::string error;
  expect(amber::runtime::http::http_encode_chunk("data", extensions, &wire,
                                                 &error),
         "chunk with extensions encodes");
  expect(wire ==
             "4;trace=abc;flag;trace=\"quoted value\\\\\\\"\"\r\ndata\r\n",
         "chunk extensions preserve order, duplicates, and quoting");

  const std::size_t crlf = wire.find("\r\n");
  const std::size_t semi = wire.find(';');
  std::vector<HttpChunkExtension> parsed;
  expect(amber::runtime::http::http_parse_chunk_extensions(
             wire.substr(semi, crlf - semi), &parsed, &error),
         "encoded extensions parse");
  expect(parsed.size() == 3U && parsed[0].name == "trace" &&
             parsed[0].value == "abc" && parsed[1].name == "flag" &&
             !parsed[1].value.has_value() && parsed[2].name == "trace" &&
             parsed[2].value == "quoted value\\\"",
         "extension parser decodes ordered values");
  expect(!amber::runtime::http::http_parse_chunk_extensions(
             ";bad=\"x\r\ny\"", &parsed, &error),
         "extension parser rejects CRLF injection");

  HttpHeaders trailers;
  expect(trailers.add("Digest", "sha-256=xyz", &error),
         "valid trailer added");
  expect(amber::runtime::http::http_encode_last_chunk(trailers, &wire, &error),
         "last chunk with trailers encodes");
  expect(wire == "0\r\ndigest: sha-256=xyz\r\n\r\n",
         "trailer wire follows terminating chunk");
}

void test_roundtrip_serialize_then_parse_chunked() {
  // Build a chunked request body and feed it back through the parser as if it
  // were a chunked response body, proving the encoder/decoder agree.
  std::string wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
  wire += amber::runtime::http::http_encode_chunk("part-one ");
  wire += amber::runtime::http::http_encode_chunk("part-two");
  wire += amber::runtime::http::http_encode_last_chunk();
  HttpResponseParser parser = parse_all(wire);
  expect(!parser.has_error(), "roundtrip no error");
  expect(parser.body() == "part-one part-two", "roundtrip body matches");
}

} // namespace

int main() {
  test_headers_case_insensitive_and_canonical();
  test_headers_duplicate_and_order_preserved();
  test_headers_set_replaces_all();
  test_headers_value_trimmed();
  test_headers_reject_invalid_names();
  test_headers_reject_crlf_nul_values();
  test_headers_no_hidden_comma_folding();

  test_valid_status_line_and_headers();
  test_status_line_http_1_0_and_empty_reason();
  test_status_line_no_trailing_space();
  test_oversized_status_line();
  test_oversized_header_section();
  test_malformed_crlf();
  test_duplicate_equal_content_length();
  test_duplicate_conflicting_content_length();
  test_list_content_length_conflict();
  test_transfer_encoding_plus_content_length();
  test_unsupported_transfer_coding();
  test_chunked_body_with_extensions_and_trailers();
  test_invalid_chunk_size();
  test_oversized_chunk_header();
  test_unexpected_eof();
  test_unexpected_eof_in_headers();
  test_informational_1xx_before_final();
  test_unsupported_101();
  test_head_response_no_body();
  test_304_no_body();
  test_close_delimited_body();
  test_incremental_byte_at_a_time();
  test_take_body_drains();

  test_serialize_request_head();
  test_normalize_method();
  test_encode_chunks();
  test_chunk_extensions_and_trailers();
  test_roundtrip_serialize_then_parse_chunked();

  std::cout << "http codec tests passed (" << g_checks << " checks)\n";
  return 0;
}
