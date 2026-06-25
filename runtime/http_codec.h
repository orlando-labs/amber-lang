#pragma once

// HTTP/1.1 wire codec (DESIGN-stdlib-net-http-io-2026-06-20 Phase 1, §18 wire
// rules + §9 Headers). This translation unit is deliberately VM-independent: it
// names no `Value`, `Frame`, or `Vm` type, only `std::string`. It is the
// incremental, bounded request serializer / response parser the spec calls for
// in §26.1, factored so it can be unit-tested directly (like runtime/io.cpp via
// tests/io_tests.cpp) and reused by the Phase 2 VM dispatch layer.
//
// Conformance coverage targeted here: §25.2 (headers) and §25.6 (protocol
// parser). Pooling, timeouts, redirects, and the Amber-facing value surface
// land in later phases on top of this codec.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace amber::runtime::http {

// Error categories produced by header validation and response parsing. The VM
// boundary (Phase 2) maps each to the §21 HTTP error class name returned by
// `http_error_class_name`; the codec itself stays free of the VM error
// registry.
enum class HttpErrorKind {
  None,
  // Request-construction / header validation (§21.1 RequestError subtree).
  InvalidHeader,
  InvalidUrl,
  InvalidMethod,
  UnsupportedScheme,
  BodyLength,
  BodyLimit,
  // Response parser (§21.1 ProtocolError subtree).
  Protocol,
  HeaderLimit,
  StatusLineLimit,
  Chunk,
  UnexpectedEof,
  // Transport failure surfaced during an exchange (§21.2 ConnectionError).
  Connection,
};

// The §21.1 error class name a kind maps to (e.g. "ChunkError"). "None" yields
// the empty string.
const char *http_error_class_name(HttpErrorKind kind);

std::string ascii_lower_copy(std::string text);

// A single field name is a valid HTTP token (RFC 9110 §5.6.2). Empty is
// invalid.
bool http_valid_field_name(const std::string &name);

// A field value rejects CR, LF, and NUL (§9.4). Leading/trailing optional
// whitespace is allowed here; callers trim where appropriate.
bool http_valid_field_value(const std::string &value);

// Normalize a request method to its uppercase token form (§8.2). Returns false
// if the method is empty or contains a non-token character.
bool http_normalize_method(const std::string &method, std::string *out);

// Ordered, case-insensitive multi-value header collection (§9). Field names are
// stored in lowercase canonical form; insertion order and duplicate field lines
// are preserved. This is neither a Map nor a StrictMap: comma folding is never
// implicit (§9.3).
class HttpHeaders {
public:
  // Validating append (§9.2 add!). Returns false and sets *error on an invalid
  // name or a value containing CR/LF/NUL (which also catches obsolete line
  // folding). The stored value is OWS-trimmed.
  bool add(const std::string &name, const std::string &value,
           std::string *error);

  // Non-validating append used by the parser, which has already validated the
  // field name token and value. `lower_name` must already be lowercased.
  void add_parsed(std::string lower_name, std::string value);

  // Validating set! (§9.2): remove every existing line for `name`, then append
  // one. Returns false and sets *error like add.
  bool set(const std::string &name, const std::string &value,
           std::string *error);

  // Remove all field lines whose name matches `name` (case-insensitively).
  void remove(const std::string &name);

  bool contains(const std::string &name) const;
  // First value for `name` in field-line order, or nullopt.
  std::optional<std::string> first(const std::string &name) const;
  // Every value for `name`, in field-line order.
  std::vector<std::string> all(const std::string &name) const;

  std::size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }

  // Field lines in order, names in lowercase canonical form (§9.2 to_pairs).
  const std::vector<std::pair<std::string, std::string>> &pairs() const {
    return entries_;
  }

  // §9.3 combined: comma-join the values of a list-compatible field. Returns
  // nullopt for an absent field. Callers must check is_list_combinable first;
  // Set-Cookie and other non-list fields must not be folded.
  static bool is_list_combinable(const std::string &lower_name);
  std::optional<std::string> combined(const std::string &name) const;

private:
  // Lowercased field names, value already OWS-trimmed; insertion order kept.
  std::vector<std::pair<std::string, std::string>> entries_;
};

// --- Request serialization (§18.1) -----------------------------------------

// Serialize the request line and header block in origin-form, terminated by the
// blank line. `target` is the already-resolved origin-form request target
// (path with optional ?query, fragment stripped). `method` is emitted verbatim;
// callers normalize via http_normalize_method first. Header names are emitted
// in their stored canonical (lowercase) form. Body bytes follow separately,
// framed by the chunk helpers below or by a Content-Length the caller added.
std::string http_serialize_request_head(const std::string &method,
                                        const std::string &target,
                                        const HttpHeaders &headers,
                                        int minor_version = 1);

// chunked transfer-coding (§18.1). `http_encode_chunk` frames one non-empty
// data chunk; an empty `data` produces no output (a zero-length data chunk is
// reserved for the terminator). `http_encode_last_chunk` emits the terminating
// zero-length chunk; v1 sends no request trailers.
std::string http_encode_chunk(const std::string &data);
std::string http_encode_last_chunk();

// --- Response parsing (§18.2) ----------------------------------------------

struct HttpResponseParserLimits {
  std::size_t max_status_line_bytes = 8192;
  std::size_t max_header_bytes = 65536;
  std::size_t max_chunk_header_bytes = 8192;
};

// Incremental, bounded HTTP/1.1 response parser. Feed transport bytes with
// `feed` (in any chunking); call `finish` when the transport reaches EOF.
// Informational 1xx responses are consumed transparently until the final
// response (§18.2); 101 is rejected as unsupported. HEAD/204/304 responses
// carry no body regardless of headers.
class HttpResponseParser {
public:
  explicit HttpResponseParser(HttpResponseParserLimits limits = {},
                              bool head_request = false);

  // Feed raw transport bytes. Returns false once a protocol error is latched;
  // after that the parser is terminal and further feeds are no-ops.
  bool feed(const char *data, std::size_t len);
  bool feed(const std::string &data) { return feed(data.data(), data.size()); }

  // Signal that the transport peer closed. Completes a close-delimited body;
  // for a body still expecting bytes this latches UnexpectedEof. Returns false
  // if it produced (or the parser already had) an error.
  bool finish();

  bool headers_complete() const { return headers_complete_; }
  bool message_complete() const { return state_ == State::Complete; }
  bool has_error() const { return error_ != HttpErrorKind::None; }
  HttpErrorKind error_kind() const { return error_; }
  const std::string &error_message() const { return error_message_; }

  // Valid once headers_complete(): the final status, reason, and version.
  int status() const { return status_; }
  const std::string &reason() const { return reason_; }
  int minor_version() const { return minor_version_; }
  const HttpHeaders &headers() const { return headers_; }
  // Trailers parsed after a chunked body's terminator (§18.2); empty otherwise.
  const HttpHeaders &trailers() const { return trailers_; }

  // Decoded body bytes accumulated so far (transfer-coding already removed).
  const std::string &body() const { return body_; }
  // Drain and return the decoded body accumulated so far.
  std::string take_body();

  // Raw chunk-extension text (everything after the ';' on each chunk-size
  // line), in order, when chunked framing is used (§18.3).
  const std::vector<std::string> &chunk_extensions() const {
    return chunk_extensions_;
  }

private:
  enum class State {
    StatusLine,
    HeaderLines,
    BodyContentLength,
    BodyChunkSize,
    BodyChunkData,
    BodyChunkDataCrlf,
    BodyTrailers,
    BodyCloseDelimited,
    Complete,
    Error,
  };

  bool process();
  bool parse_status_line();
  bool parse_header_lines();
  bool decide_body_framing();
  bool parse_chunk_size();
  bool parse_chunk_data();
  bool parse_chunk_data_crlf();
  bool parse_trailers();

  // Extract the next CRLF-terminated line from buf_ starting at cursor_, with a
  // strict-CRLF check. On success sets *line (without CRLF), advances cursor_
  // past the CRLF, and returns Line::Found. Returns Line::NeedMore when no CRLF
  // is buffered yet, or Line::Malformed on a bare CR/LF.
  enum class Line { Found, NeedMore, Malformed };
  Line next_line(std::string *line);

  bool set_error(HttpErrorKind kind, std::string message);
  // Reset per-message header/body state to parse a fresh response (used to skip
  // an informational 1xx response and continue to the final one).
  void reset_for_next_response();
  // Drop the already-consumed prefix of buf_ to bound memory.
  void compact_buffer();

  HttpResponseParserLimits limits_;
  bool head_request_ = false;

  State state_ = State::StatusLine;
  std::string buf_;
  std::size_t cursor_ = 0;

  bool headers_complete_ = false;
  // True while consuming the header block of an informational 1xx response, so
  // the blank line restarts at StatusLine for the final response (§18.2).
  bool interim_ = false;
  int status_ = 0;
  std::string reason_;
  int minor_version_ = 1;
  HttpHeaders headers_;
  HttpHeaders trailers_;

  std::size_t header_bytes_ = 0;
  std::uint64_t content_remaining_ = 0;
  std::uint64_t chunk_remaining_ = 0;

  std::string body_;
  std::vector<std::string> chunk_extensions_;

  HttpErrorKind error_ = HttpErrorKind::None;
  std::string error_message_;
};

} // namespace amber::runtime::http
