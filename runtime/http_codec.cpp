#include "runtime/http_codec.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>

namespace amber::runtime::http {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

bool is_ows(char c) { return c == ' ' || c == '\t'; }

// RFC 9110 §5.6.2 tchar.
bool is_tchar(unsigned char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9')) {
    return true;
  }
  switch (c) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

std::string trim_ows(const std::string &text) {
  std::size_t start = 0;
  std::size_t end = text.size();
  while (start < end && is_ows(text[start])) {
    ++start;
  }
  while (end > start && is_ows(text[end - 1U])) {
    --end;
  }
  return text.substr(start, end - start);
}

// Split a header value on commas into OWS-trimmed tokens (used to normalize
// duplicate / list-form Content-Length values, §18.2).
std::vector<std::string> split_comma_list(const std::string &value) {
  std::vector<std::string> tokens;
  std::size_t pos = 0;
  while (pos <= value.size()) {
    const std::size_t comma = value.find(',', pos);
    const std::size_t end = comma == std::string::npos ? value.size() : comma;
    tokens.push_back(trim_ows(value.substr(pos, end - pos)));
    if (comma == std::string::npos) {
      break;
    }
    pos = comma + 1U;
  }
  return tokens;
}

bool parse_decimal_u64(const std::string &text, std::uint64_t *out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
  }
  *out = value;
  return true;
}

int hex_digit_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

bool is_valid_extension_value_byte(unsigned char c) {
  // quoted-string allows HTAB / SP / visible ASCII / obs-text. CR, LF, NUL and
  // the remaining controls are deliberately rejected at the public boundary.
  return c == '\t' || c == ' ' || (c >= 0x21U && c <= 0x7eU) || c >= 0x80U;
}

void append_chunk_size(std::size_t size, std::string *out) {
  std::array<char, 2U * sizeof(std::size_t)> digits{};
  std::size_t count = 0;
  do {
    digits[count++] = kHexDigits[size & 0xFU];
    size >>= 4U;
  } while (size > 0);
  for (std::size_t i = count; i > 0; --i) {
    out->push_back(digits[i - 1U]);
  }
}

bool append_chunk_extensions(
    const std::vector<HttpChunkExtension> &extensions, std::string *out,
    std::string *error) {
  for (const HttpChunkExtension &extension : extensions) {
    if (!http_valid_field_name(extension.name)) {
      if (error != nullptr) {
        *error = "invalid chunk extension name";
      }
      return false;
    }
    out->push_back(';');
    *out += extension.name;
    if (!extension.value.has_value()) {
      continue;
    }
    out->push_back('=');
    bool token = !extension.value->empty();
    for (const unsigned char c : *extension.value) {
      token = token && is_tchar(c);
      if (!is_valid_extension_value_byte(c)) {
        if (error != nullptr) {
          *error = "invalid byte in chunk extension value";
        }
        return false;
      }
    }
    if (token) {
      *out += *extension.value;
      continue;
    }
    out->push_back('"');
    for (const char c : *extension.value) {
      if (c == '"' || c == '\\') {
        out->push_back('\\');
      }
      out->push_back(c);
    }
    out->push_back('"');
  }
  return true;
}

} // namespace

const char *http_error_class_name(HttpErrorKind kind) {
  switch (kind) {
  case HttpErrorKind::None:
    return "";
  case HttpErrorKind::InvalidHeader:
    return "InvalidHeaderError";
  case HttpErrorKind::InvalidUrl:
    return "InvalidUrlError";
  case HttpErrorKind::InvalidMethod:
    return "InvalidMethodError";
  case HttpErrorKind::UnsupportedScheme:
    return "UnsupportedSchemeError";
  case HttpErrorKind::BodyLength:
    return "BodyLengthError";
  case HttpErrorKind::BodyLimit:
    return "BodyLimitError";
  case HttpErrorKind::Protocol:
    return "ProtocolError";
  case HttpErrorKind::HeaderLimit:
    return "HeaderLimitError";
  case HttpErrorKind::StatusLineLimit:
    return "StatusLineLimitError";
  case HttpErrorKind::Chunk:
    return "ChunkError";
  case HttpErrorKind::UnexpectedEof:
    return "UnexpectedEofError";
  case HttpErrorKind::Connection:
    return "ConnectionError";
  }
  return "";
}

std::string ascii_lower_copy(std::string text) {
  for (char &c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

bool http_valid_field_name(const std::string &name) {
  if (name.empty()) {
    return false;
  }
  for (const unsigned char c : name) {
    if (!is_tchar(c)) {
      return false;
    }
  }
  return true;
}

bool http_valid_field_value(const std::string &value) {
  for (const unsigned char c : value) {
    if (c == '\r' || c == '\n' || c == '\0') {
      return false;
    }
  }
  return true;
}

bool http_normalize_method(const std::string &method, std::string *out) {
  if (method.empty()) {
    return false;
  }
  std::string normalized;
  normalized.reserve(method.size());
  for (const unsigned char c : method) {
    const char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A')
                                              : static_cast<char>(c);
    if (!is_tchar(static_cast<unsigned char>(upper))) {
      return false;
    }
    normalized.push_back(upper);
  }
  *out = std::move(normalized);
  return true;
}

// --- HttpHeaders -----------------------------------------------------------

bool HttpHeaders::add(const std::string &name, const std::string &value,
                      std::string *error) {
  if (!http_valid_field_name(name)) {
    if (error != nullptr) {
      *error = "invalid HTTP header name: " + name;
    }
    return false;
  }
  if (!http_valid_field_value(value)) {
    if (error != nullptr) {
      *error = "HTTP header value contains CR, LF, or NUL";
    }
    return false;
  }
  entries_.push_back({ascii_lower_copy(name), trim_ows(value)});
  return true;
}

void HttpHeaders::add_parsed(std::string lower_name, std::string value) {
  entries_.push_back({std::move(lower_name), std::move(value)});
}

bool HttpHeaders::set(const std::string &name, const std::string &value,
                      std::string *error) {
  if (!http_valid_field_name(name)) {
    if (error != nullptr) {
      *error = "invalid HTTP header name: " + name;
    }
    return false;
  }
  if (!http_valid_field_value(value)) {
    if (error != nullptr) {
      *error = "HTTP header value contains CR, LF, or NUL";
    }
    return false;
  }
  remove(name);
  entries_.push_back({ascii_lower_copy(name), trim_ows(value)});
  return true;
}

void HttpHeaders::remove(const std::string &name) {
  const std::string lower = ascii_lower_copy(name);
  std::vector<std::pair<std::string, std::string>> kept;
  kept.reserve(entries_.size());
  for (auto &entry : entries_) {
    if (entry.first != lower) {
      kept.push_back(std::move(entry));
    }
  }
  entries_.swap(kept);
}

bool HttpHeaders::contains(const std::string &name) const {
  const std::string lower = ascii_lower_copy(name);
  for (const auto &entry : entries_) {
    if (entry.first == lower) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> HttpHeaders::first(const std::string &name) const {
  const std::string lower = ascii_lower_copy(name);
  for (const auto &entry : entries_) {
    if (entry.first == lower) {
      return entry.second;
    }
  }
  return std::nullopt;
}

std::vector<std::string> HttpHeaders::all(const std::string &name) const {
  const std::string lower = ascii_lower_copy(name);
  std::vector<std::string> values;
  for (const auto &entry : entries_) {
    if (entry.first == lower) {
      values.push_back(entry.second);
    }
  }
  return values;
}

bool HttpHeaders::is_list_combinable(const std::string &lower_name) {
  // Set-Cookie is the canonical non-combinable field (§9.3). Be conservative:
  // only fold a small allow-list of known comma-list fields.
  static const std::array<const char *, 6> kCombinable = {
      "accept", "accept-encoding", "accept-language", "cache-control", "via",
      "vary"};
  for (const char *candidate : kCombinable) {
    if (lower_name == candidate) {
      return true;
    }
  }
  return false;
}

std::optional<std::string>
HttpHeaders::combined(const std::string &name) const {
  const std::string lower = ascii_lower_copy(name);
  std::string out;
  bool found = false;
  for (const auto &entry : entries_) {
    if (entry.first != lower) {
      continue;
    }
    if (found) {
      out += ", ";
    }
    out += entry.second;
    found = true;
  }
  if (!found) {
    return std::nullopt;
  }
  return out;
}

// --- Request serialization -------------------------------------------------

std::string http_serialize_request_head(const std::string &method,
                                        const std::string &target,
                                        const HttpHeaders &headers,
                                        int minor_version) {
  std::string out;
  out += method;
  out.push_back(' ');
  out += target;
  out += " HTTP/1.";
  out += (minor_version == 0) ? '0' : '1';
  out += "\r\n";
  for (const auto &entry : headers.pairs()) {
    out += entry.first;
    out += ": ";
    out += entry.second;
    out += "\r\n";
  }
  out += "\r\n";
  return out;
}

std::string http_encode_chunk(const std::string &data) {
  if (data.empty()) {
    return std::string{};
  }
  std::string out;
  std::size_t size = data.size();
  // Hex chunk size, no leading zeros.
  std::array<char, 16> digits{};
  std::size_t count = 0;
  if (size == 0) {
    digits[count++] = '0';
  } else {
    while (size > 0) {
      digits[count++] = kHexDigits[size & 0xFU];
      size >>= 4U;
    }
  }
  for (std::size_t i = count; i > 0; --i) {
    out.push_back(digits[i - 1U]);
  }
  out += "\r\n";
  out += data;
  out += "\r\n";
  return out;
}

bool http_parse_chunk_extensions(const std::string &text,
                                 std::vector<HttpChunkExtension> *out,
                                 std::string *error, std::size_t max_count,
                                 std::size_t max_bytes) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (text.size() > max_bytes) {
    if (error != nullptr) {
      *error = "chunk extensions exceed limit";
    }
    return false;
  }
  std::size_t i = 0;
  auto skip_bws = [&]() {
    while (i < text.size() && is_ows(text[i])) {
      ++i;
    }
  };
  while (i < text.size()) {
    skip_bws();
    if (i >= text.size() || text[i] != ';') {
      if (error != nullptr) {
        *error = "malformed chunk extension";
      }
      return false;
    }
    ++i;
    skip_bws();
    const std::size_t name_start = i;
    while (i < text.size() && is_tchar(static_cast<unsigned char>(text[i]))) {
      ++i;
    }
    if (i == name_start) {
      if (error != nullptr) {
        *error = "missing chunk extension name";
      }
      return false;
    }
    if (out->size() >= max_count) {
      if (error != nullptr) {
        *error = "too many chunk extensions";
      }
      return false;
    }
    HttpChunkExtension extension;
    extension.name = text.substr(name_start, i - name_start);
    skip_bws();
    if (i < text.size() && text[i] == '=') {
      ++i;
      skip_bws();
      if (i >= text.size()) {
        if (error != nullptr) {
          *error = "missing chunk extension value";
        }
        return false;
      }
      if (text[i] == '"') {
        ++i;
        std::string value;
        bool closed = false;
        while (i < text.size()) {
          unsigned char c = static_cast<unsigned char>(text[i++]);
          if (c == '"') {
            closed = true;
            break;
          }
          if (c == '\\') {
            if (i >= text.size()) {
              if (error != nullptr) {
                *error = "unterminated quoted chunk extension";
              }
              return false;
            }
            c = static_cast<unsigned char>(text[i++]);
            if (c != '\t' && c != ' ' && c < 0x21U) {
              if (error != nullptr) {
                *error = "invalid quoted-pair in chunk extension";
              }
              return false;
            }
          }
          if (!is_valid_extension_value_byte(c)) {
            if (error != nullptr) {
              *error = "invalid byte in chunk extension value";
            }
            return false;
          }
          value.push_back(static_cast<char>(c));
        }
        if (!closed) {
          if (error != nullptr) {
            *error = "unterminated quoted chunk extension";
          }
          return false;
        }
        extension.value = std::move(value);
      } else {
        const std::size_t value_start = i;
        while (i < text.size() &&
               is_tchar(static_cast<unsigned char>(text[i]))) {
          ++i;
        }
        if (i == value_start) {
          if (error != nullptr) {
            *error = "invalid chunk extension value";
          }
          return false;
        }
        extension.value = text.substr(value_start, i - value_start);
      }
      skip_bws();
    }
    if (i < text.size() && text[i] != ';') {
      if (error != nullptr) {
        *error = "malformed chunk extension separator";
      }
      return false;
    }
    out->push_back(std::move(extension));
  }
  return true;
}

bool http_encode_chunk(const std::string &data,
                       const std::vector<HttpChunkExtension> &extensions,
                       std::string *out, std::string *error) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (data.empty()) {
    if (error != nullptr) {
      *error = "stream chunk data must not be empty";
    }
    return false;
  }
  append_chunk_size(data.size(), out);
  if (!append_chunk_extensions(extensions, out, error)) {
    out->clear();
    return false;
  }
  *out += "\r\n";
  *out += data;
  *out += "\r\n";
  return true;
}

bool http_encode_last_chunk(const HttpHeaders &trailers, std::string *out,
                            std::string *error) {
  if (out == nullptr) {
    return false;
  }
  *out = "0\r\n";
  for (const auto &entry : trailers.pairs()) {
    if (!http_valid_field_name(entry.first) ||
        !http_valid_field_value(entry.second)) {
      if (error != nullptr) {
        *error = "invalid HTTP trailer";
      }
      out->clear();
      return false;
    }
    *out += entry.first;
    *out += ": ";
    *out += entry.second;
    *out += "\r\n";
  }
  *out += "\r\n";
  return true;
}

std::string http_encode_last_chunk() { return "0\r\n\r\n"; }

// --- HttpResponseParser ----------------------------------------------------

HttpResponseParser::HttpResponseParser(HttpResponseParserLimits limits,
                                       bool head_request)
    : limits_(limits), head_request_(head_request) {}

bool HttpResponseParser::set_error(HttpErrorKind kind, std::string message) {
  error_ = kind;
  error_message_ = std::move(message);
  state_ = State::Error;
  return false;
}

void HttpResponseParser::compact_buffer() {
  if (cursor_ == 0) {
    return;
  }
  buf_.erase(0, cursor_);
  cursor_ = 0;
}

void HttpResponseParser::reset_for_next_response() {
  headers_complete_ = false;
  status_ = 0;
  reason_.clear();
  minor_version_ = 1;
  headers_ = HttpHeaders{};
  header_bytes_ = 0;
  state_ = State::StatusLine;
}

std::string HttpResponseParser::take_body() {
  std::string out;
  out.swap(body_);
  return out;
}

HttpResponseParser::Line HttpResponseParser::next_line(std::string *line) {
  const std::size_t lf = buf_.find('\n', cursor_);
  if (lf == std::string::npos) {
    return Line::NeedMore;
  }
  if (lf == cursor_ || buf_[lf - 1U] != '\r') {
    // Bare LF (or CR without LF): strict CRLF parsing rejects it (§18.2).
    return Line::Malformed;
  }
  *line = buf_.substr(cursor_, (lf - 1U) - cursor_);
  cursor_ = lf + 1U;
  return Line::Found;
}

bool HttpResponseParser::feed(const char *data, std::size_t len) {
  if (state_ == State::Error) {
    return false;
  }
  if (state_ == State::Complete) {
    // Extra bytes after a complete message are ignored at the codec level;
    // connection reuse / framing is the pool's concern (Phase 4).
    return true;
  }
  buf_.append(data, len);
  return process();
}

bool HttpResponseParser::process() {
  for (;;) {
    switch (state_) {
    case State::StatusLine:
      if (!parse_status_line()) {
        return error_ == HttpErrorKind::None;
      }
      break;
    case State::HeaderLines:
      if (!parse_header_lines()) {
        return error_ == HttpErrorKind::None;
      }
      break;
    case State::BodyContentLength: {
      const std::size_t available = buf_.size() - cursor_;
      const std::size_t take = static_cast<std::size_t>(
          std::min<std::uint64_t>(content_remaining_, available));
      body_.append(buf_, cursor_, take);
      cursor_ += take;
      content_remaining_ -= take;
      compact_buffer();
      if (content_remaining_ == 0) {
        state_ = State::Complete;
        return true;
      }
      return true; // need more bytes
    }
    case State::BodyChunkSize:
      if (!parse_chunk_size()) {
        return error_ == HttpErrorKind::None;
      }
      break;
    case State::BodyChunkData:
      if (!parse_chunk_data()) {
        return error_ == HttpErrorKind::None;
      }
      break;
    case State::BodyChunkDataCrlf:
      if (!parse_chunk_data_crlf()) {
        return error_ == HttpErrorKind::None;
      }
      break;
    case State::BodyTrailers:
      if (!parse_trailers()) {
        return error_ == HttpErrorKind::None;
      }
      break;
    case State::BodyCloseDelimited: {
      const std::size_t available = buf_.size() - cursor_;
      body_.append(buf_, cursor_, available);
      cursor_ += available;
      compact_buffer();
      return true; // completes only on finish()
    }
    case State::Complete:
      return true;
    case State::Error:
      return false;
    }
  }
}

bool HttpResponseParser::parse_status_line() {
  std::string line;
  const Line outcome = next_line(&line);
  if (outcome == Line::NeedMore) {
    if (buf_.size() - cursor_ > limits_.max_status_line_bytes) {
      return set_error(HttpErrorKind::StatusLineLimit,
                       "HTTP status line exceeds limit");
    }
    return false;
  }
  if (outcome == Line::Malformed) {
    return set_error(HttpErrorKind::Protocol,
                     "malformed CRLF in HTTP status line");
  }
  if (line.size() > limits_.max_status_line_bytes) {
    return set_error(HttpErrorKind::StatusLineLimit,
                     "HTTP status line exceeds limit");
  }

  // status-line = HTTP-version SP status-code SP [ reason-phrase ]
  if (line.rfind("HTTP/1.", 0) != 0 || line.size() < 12U) {
    return set_error(HttpErrorKind::Protocol, "invalid HTTP status line");
  }
  const char minor = line[7];
  if (minor != '0' && minor != '1') {
    return set_error(HttpErrorKind::Protocol,
                     "unsupported HTTP version in status line");
  }
  if (line[8] != ' ') {
    return set_error(HttpErrorKind::Protocol, "invalid HTTP status line");
  }
  const std::string code_text = line.substr(9, 3);
  if (code_text.size() != 3U || !std::isdigit((unsigned char)code_text[0]) ||
      !std::isdigit((unsigned char)code_text[1]) ||
      !std::isdigit((unsigned char)code_text[2])) {
    return set_error(HttpErrorKind::Protocol, "invalid HTTP status code");
  }
  // After the 3-digit code there must be SP then an (optional) reason, or the
  // line may end exactly at the code.
  std::string reason;
  if (line.size() > 12U) {
    if (line[12] != ' ') {
      return set_error(HttpErrorKind::Protocol, "invalid HTTP status line");
    }
    reason = line.substr(13);
  }

  const int code = (code_text[0] - '0') * 100 + (code_text[1] - '0') * 10 +
                   (code_text[2] - '0');
  minor_version_ = (minor == '0') ? 0 : 1;

  if (code == 101) {
    return set_error(HttpErrorKind::Protocol,
                     "101 Switching Protocols is unsupported");
  }
  if (code >= 100 && code < 200) {
    // Informational 1xx: consume its (typically empty) header block, then parse
    // the next response. We stay in a lightweight header-skip by reusing the
    // header parser but discarding the result on completion.
    status_ = code;
    reason_ = reason;
    header_bytes_ = 0;
    headers_ = HttpHeaders{};
    state_ = State::HeaderLines;
    // Mark that the current header block belongs to an interim response: when
    // it finishes, parse_header_lines restarts at StatusLine instead of a body.
    headers_complete_ = false;
    interim_ = true;
    compact_buffer();
    return true;
  }

  status_ = code;
  reason_ = reason;
  interim_ = false;
  state_ = State::HeaderLines;
  compact_buffer();
  return true;
}

bool HttpResponseParser::parse_header_lines() {
  for (;;) {
    std::string line;
    const Line outcome = next_line(&line);
    if (outcome == Line::NeedMore) {
      if (buf_.size() - cursor_ + header_bytes_ > limits_.max_header_bytes) {
        return set_error(HttpErrorKind::HeaderLimit,
                         "HTTP header section exceeds limit");
      }
      return false;
    }
    if (outcome == Line::Malformed) {
      return set_error(HttpErrorKind::Protocol,
                       "malformed CRLF in HTTP header");
    }
    header_bytes_ += line.size() + 2U;
    if (header_bytes_ > limits_.max_header_bytes) {
      return set_error(HttpErrorKind::HeaderLimit,
                       "HTTP header section exceeds limit");
    }

    if (line.empty()) {
      // Blank line: end of header block.
      compact_buffer();
      if (interim_) {
        reset_for_next_response();
        return true; // re-enter process(): back to StatusLine
      }
      return decide_body_framing();
    }

    if (line[0] == ' ' || line[0] == '\t') {
      return set_error(HttpErrorKind::Protocol,
                       "obsolete HTTP header line folding is rejected");
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0U) {
      return set_error(HttpErrorKind::Protocol, "malformed HTTP header line");
    }
    const std::string name = line.substr(0, colon);
    if (!http_valid_field_name(name)) {
      return set_error(HttpErrorKind::Protocol, "invalid HTTP header name");
    }
    std::string value = trim_ows(line.substr(colon + 1U));
    if (!http_valid_field_value(value)) {
      return set_error(HttpErrorKind::Protocol, "invalid HTTP header value");
    }
    headers_.add_parsed(ascii_lower_copy(name), std::move(value));
  }
}

bool HttpResponseParser::decide_body_framing() {
  headers_complete_ = true;

  const bool no_body_status =
      (status_ >= 100 && status_ < 200) || status_ == 204 || status_ == 304;
  if (head_request_ || no_body_status) {
    state_ = State::Complete;
    return true;
  }

  const bool has_te = headers_.contains("transfer-encoding");
  const bool has_cl = headers_.contains("content-length");

  if (has_te) {
    if (has_cl) {
      return set_error(
          HttpErrorKind::Protocol,
          "Transfer-Encoding and Content-Length must not both be present");
    }
    // The final transfer coding must be chunked (§18.2). We accept exactly
    // "chunked" (case-insensitive); anything else is an unsupported coding.
    const std::vector<std::string> codings =
        split_comma_list(*headers_.first("transfer-encoding"));
    bool chunked = false;
    for (std::size_t i = 0; i < codings.size(); ++i) {
      const std::string coding = ascii_lower_copy(codings[i]);
      if (coding == "chunked") {
        // chunked must be the final coding.
        if (i + 1U != codings.size()) {
          return set_error(HttpErrorKind::Protocol,
                           "chunked must be the final transfer coding");
        }
        chunked = true;
      } else if (coding == "identity" || coding.empty()) {
        // identity is a no-op coding; tolerate it.
      } else {
        return set_error(HttpErrorKind::Protocol,
                         "unsupported transfer coding: " + codings[i]);
      }
    }
    if (!chunked) {
      return set_error(HttpErrorKind::Protocol,
                       "unsupported transfer coding (chunked required)");
    }
    state_ = State::BodyChunkSize;
    return true;
  }

  if (has_cl) {
    // Duplicate / list Content-Length is valid only when every value is the
    // same normalized number (§18.2).
    std::vector<std::string> tokens;
    for (const std::string &raw : headers_.all("content-length")) {
      for (std::string &token : split_comma_list(raw)) {
        tokens.push_back(std::move(token));
      }
    }
    std::uint64_t length = 0;
    bool first = true;
    for (const std::string &token : tokens) {
      std::uint64_t parsed = 0;
      if (!parse_decimal_u64(token, &parsed)) {
        return set_error(HttpErrorKind::Protocol,
                         "invalid Content-Length value");
      }
      if (first) {
        length = parsed;
        first = false;
      } else if (parsed != length) {
        return set_error(HttpErrorKind::Protocol,
                         "conflicting Content-Length values");
      }
    }
    content_remaining_ = length;
    if (content_remaining_ == 0) {
      state_ = State::Complete;
      return true;
    }
    state_ = State::BodyContentLength;
    return true;
  }

  // No framing headers: the body is delimited by connection close (§18.2).
  state_ = State::BodyCloseDelimited;
  return true;
}

bool HttpResponseParser::parse_chunk_size() {
  std::string line;
  const Line outcome = next_line(&line);
  if (outcome == Line::NeedMore) {
    if (buf_.size() - cursor_ > limits_.max_chunk_header_bytes) {
      return set_error(HttpErrorKind::Chunk, "chunk header exceeds limit");
    }
    return false;
  }
  if (outcome == Line::Malformed) {
    return set_error(HttpErrorKind::Chunk, "malformed CRLF in chunk header");
  }
  if (line.size() > limits_.max_chunk_header_bytes) {
    return set_error(HttpErrorKind::Chunk, "chunk header exceeds limit");
  }

  // chunk = chunk-size [ chunk-ext ] CRLF
  std::size_t i = 0;
  std::uint64_t size = 0;
  std::size_t hex_digits = 0;
  while (i < line.size() && line[i] != ';') {
    const int digit = hex_digit_value(line[i]);
    if (digit < 0) {
      return set_error(HttpErrorKind::Chunk, "invalid chunk size");
    }
    if (size > (std::numeric_limits<std::uint64_t>::max() >> 4U)) {
      return set_error(HttpErrorKind::Chunk, "chunk size overflow");
    }
    size = (size << 4U) | static_cast<std::uint64_t>(digit);
    ++hex_digits;
    ++i;
  }
  if (hex_digits == 0) {
    return set_error(HttpErrorKind::Chunk, "missing chunk size");
  }
  if (i < line.size() && line[i] == ';') {
    chunk_extensions_.push_back(line.substr(i + 1U));
  }
  compact_buffer();

  if (size == 0) {
    state_ = State::BodyTrailers;
    return true;
  }
  chunk_remaining_ = size;
  state_ = State::BodyChunkData;
  return true;
}

bool HttpResponseParser::parse_chunk_data() {
  const std::size_t available = buf_.size() - cursor_;
  const std::size_t take = static_cast<std::size_t>(
      std::min<std::uint64_t>(chunk_remaining_, available));
  body_.append(buf_, cursor_, take);
  cursor_ += take;
  chunk_remaining_ -= take;
  compact_buffer();
  if (chunk_remaining_ == 0) {
    state_ = State::BodyChunkDataCrlf;
    return true;
  }
  return false; // need more bytes
}

bool HttpResponseParser::parse_chunk_data_crlf() {
  std::string line;
  const Line outcome = next_line(&line);
  if (outcome == Line::NeedMore) {
    return false;
  }
  if (outcome == Line::Malformed || !line.empty()) {
    return set_error(HttpErrorKind::Chunk, "malformed chunk data terminator");
  }
  compact_buffer();
  state_ = State::BodyChunkSize;
  return true;
}

bool HttpResponseParser::parse_trailers() {
  for (;;) {
    std::string line;
    const Line outcome = next_line(&line);
    if (outcome == Line::NeedMore) {
      if (buf_.size() - cursor_ + header_bytes_ > limits_.max_header_bytes) {
        return set_error(HttpErrorKind::HeaderLimit,
                         "HTTP trailer section exceeds limit");
      }
      return false;
    }
    if (outcome == Line::Malformed) {
      return set_error(HttpErrorKind::Chunk, "malformed CRLF in trailer");
    }
    header_bytes_ += line.size() + 2U;
    if (header_bytes_ > limits_.max_header_bytes) {
      return set_error(HttpErrorKind::HeaderLimit,
                       "HTTP trailer section exceeds limit");
    }
    if (line.empty()) {
      compact_buffer();
      state_ = State::Complete;
      return true;
    }
    if (line[0] == ' ' || line[0] == '\t') {
      return set_error(HttpErrorKind::Protocol,
                       "obsolete trailer line folding is rejected");
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0U) {
      return set_error(HttpErrorKind::Protocol, "malformed HTTP trailer line");
    }
    const std::string name = line.substr(0, colon);
    if (!http_valid_field_name(name)) {
      return set_error(HttpErrorKind::Protocol, "invalid HTTP trailer name");
    }
    std::string value = trim_ows(line.substr(colon + 1U));
    if (!http_valid_field_value(value)) {
      return set_error(HttpErrorKind::Protocol, "invalid HTTP trailer value");
    }
    trailers_.add_parsed(ascii_lower_copy(name), std::move(value));
  }
}

bool HttpResponseParser::finish() {
  if (state_ == State::Error) {
    return false;
  }
  if (state_ == State::Complete) {
    return true;
  }
  if (state_ == State::BodyCloseDelimited) {
    state_ = State::Complete;
    return true;
  }
  return set_error(HttpErrorKind::UnexpectedEof,
                   "connection closed before HTTP message was complete");
}

} // namespace amber::runtime::http
