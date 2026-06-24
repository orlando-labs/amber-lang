#pragma once

// net.http exchange core (DESIGN-stdlib-net-http-io-2026-06-20 Phase 2,
// "core request/response, single exchange"). Like runtime/http_codec.{h,cpp},
// this layer is deliberately VM-independent (no Value/Frame/Vm): it composes
// the Phase 1 wire codec with an injected byte transport to perform one
// HTTP/1.1 request/response exchange. The transport seam is design decision D2
// — the VM wraps a RuntimeTcpStream connector behind it; tests inject a
// deterministic in-memory fake. Sockets, the Amber value surface, pooling,
// timeouts, and redirects layer on top in later slices.

#include "runtime/http_codec.h"

#include <cstdint>
#include <string>

namespace amber::runtime::http {

// Parsed origin + origin-form request target for an http:// URL (§19). v1
// accepts only the http scheme; userinfo is rejected and the fragment is
// stripped (never sent on the wire). `target` is path + optional "?query"
// with a default path of "/".
struct HttpUrl {
  std::string scheme; // always "http" in v1
  std::string host;
  std::uint16_t port = 80;
  std::string target; // origin-form: path[?query]
};

// Parse and validate an http:// URL. Returns false and sets *kind/*error on a
// malformed URL (InvalidUrl), a non-http scheme (UnsupportedScheme), userinfo,
// or a missing host.
bool http_parse_url(const std::string &url, HttpUrl *out, HttpErrorKind *kind,
                    std::string *error);

// A fully-resolved, immutable request ready to serialize (§8). Header synthesis
// (Host, Content-Length) and validation happen in http_build_request; this
// struct is what http_perform writes to the wire.
struct HttpRequest {
  std::string method; // normalized uppercase token
  std::string host;   // origin host (for connect + Host header)
  std::uint16_t port = 80;
  std::string target;  // origin-form request target
  HttpHeaders headers; // final header block (Host/Content-Length synthesized)
  std::string body;    // static request body (v1: static bodies only)
  bool has_body = false;
};

// Build a resolved HttpRequest from a method, URL, caller headers, and optional
// static body (§18.1 serialization rules). Synthesizes a Host header from the
// URL when absent (auto_host), and a Content-Length for a static body when
// absent; a caller-supplied Content-Length must match the body length. Rejects
// a caller-supplied Transfer-Encoding (unsupported in v1 request headers), an
// invalid method (InvalidMethod), and propagates URL errors. Returns false and
// sets *kind/*error on failure.
bool http_build_request(const std::string &method, const std::string &url,
                        const HttpHeaders &user_headers,
                        const std::string &body, bool has_body, bool auto_host,
                        HttpRequest *out, HttpErrorKind *kind,
                        std::string *error);

// Abstract byte transport (an io.Duplex viewed VM-independently, D2). The VM
// connector implements this over a RuntimeTcpStream; tests implement an
// in-memory fake. All methods report failure through the return value; the
// human-readable reason goes to *error.
class HttpTransport {
public:
  virtual ~HttpTransport() = default;

  // Write every byte of `data`. Returns false and sets *error on failure.
  virtual bool write_all(const std::string &data, std::string *error) = 0;

  // Read up to an implementation-chosen amount into `chunk` (cleared first).
  // Returns the number of bytes read; 0 means the peer closed (EOF); a negative
  // return means failure and sets *error.
  virtual long read_some(std::string *chunk, std::string *error) = 0;

  // Release the transport. Idempotent.
  virtual void close() = 0;
};

// Outcome of a single exchange. On `ok`, the response is fully read: status,
// headers, and the decoded body are populated. On failure, `error_kind`/
// `error_message` carry the §21 category and detail.
struct HttpExchangeResult {
  bool ok = false;
  HttpErrorKind error_kind = HttpErrorKind::None;
  std::string error_message;

  int status = 0;
  std::string reason;
  int minor_version = 1;
  HttpHeaders headers;
  std::string body;
  HttpHeaders trailers;
};

// Perform one request/response exchange over `transport`: serialize and write
// the request head and static body, then read and parse the response to
// completion (or to EOF for a close-delimited body). `head_request` suppresses
// the response body per HEAD semantics. The transport is not closed here; the
// caller owns its lifetime (pooling decisions belong to a later slice).
HttpExchangeResult http_perform(HttpTransport &transport,
                                const HttpRequest &request,
                                bool head_request = false,
                                const HttpResponseParserLimits &limits = {});

} // namespace amber::runtime::http
