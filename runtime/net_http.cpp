#include "runtime/net_http.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace amber::runtime::http {

namespace {

std::string ascii_lower(std::string text) { return ascii_lower_copy(text); }

bool parse_port_digits(const std::string &text, std::uint16_t *out,
                       std::string *error) {
  if (text.empty()) {
    *error = "empty URL port";
    return false;
  }
  std::uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      *error = "URL port must contain only digits";
      return false;
    }
    value = value * 10U + static_cast<std::uint32_t>(c - '0');
    if (value > 65535U) {
      *error = "URL port is outside 0..65535";
      return false;
    }
  }
  *out = static_cast<std::uint16_t>(value);
  return true;
}

} // namespace

bool http_parse_url(const std::string &url, HttpUrl *out, HttpErrorKind *kind,
                    std::string *error) {
  // scheme://host[:port][/path][?query][#fragment]
  const std::size_t scheme_sep = url.find("://");
  if (scheme_sep == std::string::npos) {
    *kind = HttpErrorKind::InvalidUrl;
    *error = "URL is missing scheme://";
    return false;
  }
  const std::string scheme = ascii_lower(url.substr(0, scheme_sep));
  if (scheme.empty()) {
    *kind = HttpErrorKind::InvalidUrl;
    *error = "URL is missing a scheme";
    return false;
  }
  if (scheme != "http") {
    *kind = HttpErrorKind::UnsupportedScheme;
    *error = "net.http v1 supports only the http scheme, got: " + scheme;
    return false;
  }

  const std::size_t authority_start = scheme_sep + 3U;
  // The authority runs until the first '/', '?', or '#'.
  const std::size_t authority_end = url.find_first_of("/?#", authority_start);
  const std::string authority =
      authority_end == std::string::npos
          ? url.substr(authority_start)
          : url.substr(authority_start, authority_end - authority_start);
  if (authority.find('@') != std::string::npos) {
    *kind = HttpErrorKind::InvalidUrl;
    *error = "URL userinfo is not allowed";
    return false;
  }
  if (authority.empty()) {
    *kind = HttpErrorKind::InvalidUrl;
    *error = "URL is missing a host";
    return false;
  }

  // host[:port] — bracketed IPv6 literals keep their brackets in `host`.
  std::string host;
  std::uint16_t port = 80;
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string::npos) {
      *kind = HttpErrorKind::InvalidUrl;
      *error = "unterminated IPv6 URL host";
      return false;
    }
    host = ascii_lower(authority.substr(0, close + 1U));
    if (close + 1U < authority.size()) {
      if (authority[close + 1U] != ':') {
        *kind = HttpErrorKind::InvalidUrl;
        *error = "unexpected text after bracketed URL host";
        return false;
      }
      if (!parse_port_digits(authority.substr(close + 2U), &port, error)) {
        *kind = HttpErrorKind::InvalidUrl;
        return false;
      }
    }
  } else {
    const std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
      host = ascii_lower(authority);
    } else {
      host = ascii_lower(authority.substr(0, colon));
      if (!parse_port_digits(authority.substr(colon + 1U), &port, error)) {
        *kind = HttpErrorKind::InvalidUrl;
        return false;
      }
    }
  }
  if (host.empty()) {
    *kind = HttpErrorKind::InvalidUrl;
    *error = "URL is missing a host";
    return false;
  }

  // path[?query] — strip the fragment, default the path to "/".
  std::string path_query;
  if (authority_end != std::string::npos) {
    const std::size_t fragment = url.find('#', authority_end);
    path_query = fragment == std::string::npos
                     ? url.substr(authority_end)
                     : url.substr(authority_end, fragment - authority_end);
  }
  if (path_query.empty() || path_query.front() == '?') {
    path_query = "/" + path_query;
  }

  out->scheme = scheme;
  out->host = host;
  out->port = port;
  out->target = path_query;
  return true;
}

bool http_build_request(const std::string &method, const std::string &url,
                        const HttpHeaders &user_headers,
                        const std::string &body, bool has_body, bool auto_host,
                        HttpRequest *out, HttpErrorKind *kind,
                        std::string *error) {
  std::string normalized_method;
  if (!http_normalize_method(method, &normalized_method)) {
    *kind = HttpErrorKind::InvalidMethod;
    *error = "invalid HTTP method: " + method;
    return false;
  }

  HttpUrl parsed;
  if (!http_parse_url(url, &parsed, kind, error)) {
    return false;
  }

  // The client owns request framing: a caller must not smuggle in a
  // Transfer-Encoding (§18.1). Content-Length is allowed only if it matches the
  // static body we are about to frame.
  if (user_headers.contains("transfer-encoding")) {
    *kind = HttpErrorKind::InvalidHeader;
    *error = "caller-supplied Transfer-Encoding is not allowed in v1";
    return false;
  }

  HttpHeaders headers = user_headers;

  // Host: synthesize from the URL authority when absent. A non-default port is
  // included; port 80 is omitted (§17.1 origin canonicalization).
  if (auto_host && !headers.contains("host")) {
    std::string host_value = parsed.host;
    if (parsed.port != 80) {
      host_value += ":" + std::to_string(parsed.port);
    }
    std::string host_error;
    if (!headers.set("host", host_value, &host_error)) {
      *kind = HttpErrorKind::InvalidHeader;
      *error = host_error;
      return false;
    }
  }

  if (has_body) {
    const std::string expected = std::to_string(body.size());
    if (headers.contains("content-length")) {
      const std::optional<std::string> supplied =
          headers.first("content-length");
      if (!supplied.has_value() || *supplied != expected) {
        *kind = HttpErrorKind::InvalidHeader;
        *error = "caller Content-Length does not match the request body length";
        return false;
      }
    } else {
      std::string cl_error;
      if (!headers.set("content-length", expected, &cl_error)) {
        *kind = HttpErrorKind::InvalidHeader;
        *error = cl_error;
        return false;
      }
    }
  }

  out->method = std::move(normalized_method);
  out->host = parsed.host;
  out->port = parsed.port;
  out->target = parsed.target;
  out->headers = std::move(headers);
  out->body = body;
  out->has_body = has_body;
  return true;
}

HttpExchangeResult http_perform(HttpTransport &transport,
                                const HttpRequest &request, bool head_request,
                                const HttpResponseParserLimits &limits) {
  HttpExchangeResult result;

  std::string wire = http_serialize_request_head(request.method, request.target,
                                                 request.headers);
  if (request.has_body) {
    wire += request.body;
  }

  std::string transport_error;
  if (!transport.write_all(wire, &transport_error)) {
    result.error_kind = HttpErrorKind::Connection;
    result.error_message =
        transport_error.empty() ? "failed to write request" : transport_error;
    return result;
  }

  HttpResponseParser parser(limits, head_request);
  std::string chunk;
  for (;;) {
    if (parser.message_complete()) {
      break;
    }
    chunk.clear();
    const long n = transport.read_some(&chunk, &transport_error);
    if (n < 0) {
      result.error_kind = HttpErrorKind::Connection;
      result.error_message =
          transport_error.empty() ? "failed to read response" : transport_error;
      return result;
    }
    if (n == 0) {
      // Peer closed: let the parser finalize a close-delimited body or latch
      // UnexpectedEof on a truncated message.
      parser.finish();
      break;
    }
    parser.feed(chunk);
    if (parser.has_error()) {
      break;
    }
  }

  if (parser.has_error()) {
    result.error_kind = parser.error_kind();
    result.error_message = parser.error_message();
    return result;
  }
  if (!parser.message_complete()) {
    result.error_kind = HttpErrorKind::UnexpectedEof;
    result.error_message = "response ended before it was complete";
    return result;
  }

  result.ok = true;
  result.status = parser.status();
  result.reason = parser.reason();
  result.minor_version = parser.minor_version();
  result.headers = parser.headers();
  result.body = parser.take_body();
  result.trailers = parser.trailers();
  return result;
}

} // namespace amber::runtime::http
