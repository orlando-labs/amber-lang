// net.http Amber-surface VM tests (DESIGN-stdlib-net-http-io-2026-06-20 Phase
// 2b-2). Runs real Amber source through the full compile+execute pipeline
// against an in-process loopback HTTP server, exercising
// net.http.Client().get(...) -> Response end to end (construct, capability,
// connect, exchange, Response accessors, scoped-block form, error mapping).

#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/io.h"
#include "runtime/vm.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using amber::runtime::RuntimeByteBuffer;
using amber::runtime::RuntimeTcpListener;

int g_checks = 0;

void expect(bool condition, const std::string &message) {
  ++g_checks;
  if (!condition) {
    std::cerr << "vm net.http test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<vm-net-http-test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  expect(lex_result.ok(), "lex should succeed");

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  expect(parse_result.ok(), "parse should succeed");

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  expect(bind_result.ok(), "bind should succeed");

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  expect(emit_result.ok(), "emit should succeed");

  amber::bytecode::DecodeResult decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), "decode should succeed");
  return std::move(decoded.module);
}

amber::runtime::ExecutionResult execute_source(const std::string &source) {
  amber::bytecode::BcModule module = compile_source_or_die(source);
  expect(module.init.has_entry_code_id, "module should have init code");
  return amber::runtime::execute_code(module, module.init.entry_code_id);
}

std::string with_port(const std::string &templ, std::uint16_t port) {
  std::string out = templ;
  const std::string token = "%PORT%";
  for (std::size_t pos = out.find(token); pos != std::string::npos;
       pos = out.find(token, pos)) {
    out.replace(pos, token.size(), std::to_string(port));
  }
  return out;
}

std::string with_two_ports(const std::string &templ, std::uint16_t port1,
                           std::uint16_t port2) {
  std::string out = templ;
  const std::string token1 = "%PORT1%";
  for (std::size_t pos = out.find(token1); pos != std::string::npos;
       pos = out.find(token1, pos)) {
    out.replace(pos, token1.size(), std::to_string(port1));
  }
  const std::string token2 = "%PORT2%";
  for (std::size_t pos = out.find(token2); pos != std::string::npos;
       pos = out.find(token2, pos)) {
    out.replace(pos, token2.size(), std::to_string(port2));
  }
  return out;
}

std::string ascii_lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

bool request_complete(const std::string &request) {
  const std::size_t header_end = request.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return false;
  }
  const std::string headers = ascii_lower(request.substr(0, header_end + 2));
  const std::size_t body_start = header_end + 4U;
  if (headers.find("\r\ntransfer-encoding: chunked\r\n") != std::string::npos) {
    return request.find("\r\n0\r\n\r\n", body_start) != std::string::npos;
  }
  const std::size_t cl = headers.find("\r\ncontent-length:");
  if (cl == std::string::npos) {
    return true;
  }
  const std::size_t value_start =
      cl + std::string("\r\ncontent-length:").size();
  const std::size_t value_end = headers.find("\r\n", value_start);
  if (value_end == std::string::npos) {
    return false;
  }
  std::size_t length = 0;
  for (std::size_t i = value_start; i < value_end; ++i) {
    const char c = headers[i];
    if (c == ' ' || c == '\t') {
      continue;
    }
    if (c < '0' || c > '9') {
      return true;
    }
    length = length * 10U + static_cast<std::size_t>(c - '0');
  }
  return request.size() - body_start >= length;
}

// Run `source` (with %PORT% substituted) while a one-shot loopback server
// replies with `response`. Returns the execution result.
amber::runtime::ExecutionResult
run_with_server_capture(const std::string &source, const std::string &response,
                        std::string *server_error, std::string *got_request) {
  auto listening = RuntimeTcpListener::listen({"127.0.0.1", 0});
  expect(listening.ok, "loopback listen failed: " + listening.error_name);
  const std::uint16_t port = listening.listener->local_endpoint().port;
  listening.listener->allow_unchecked_sharing();

  std::thread server([&] {
    auto accepted = listening.listener->accept(2s);
    if (!accepted.ok) {
      *server_error = "accept:" + accepted.error_name;
      return;
    }
    std::string request;
    while (!request_complete(request)) {
      RuntimeByteBuffer buf(4096);
      auto read = accepted.stream->read(buf, 2s);
      if (read.eof) {
        break;
      }
      if (!read.ok) {
        *server_error = "read:" + read.error_name;
        return;
      }
      request += buf.bytes();
    }
    if (got_request != nullptr) {
      *got_request = request;
    }
    auto written = accepted.stream->write_all(response, 2s);
    if (!written.ok) {
      *server_error = "write:" + written.error_name;
    }
    accepted.stream->close();
  });

  amber::runtime::ExecutionResult result =
      execute_source(with_port(source, port));
  server.join();
  listening.listener->close();
  return result;
}

amber::runtime::ExecutionResult run_with_server(const std::string &source,
                                                const std::string &response,
                                                std::string *server_error) {
  return run_with_server_capture(source, response, server_error, nullptr);
}

amber::runtime::ExecutionResult run_with_server_until_request_contains(
    const std::string &source, const std::string &response,
    const std::string &needle, std::string *server_error,
    std::string *got_request) {
  auto listening = RuntimeTcpListener::listen({"127.0.0.1", 0});
  expect(listening.ok, "loopback listen failed: " + listening.error_name);
  const std::uint16_t port = listening.listener->local_endpoint().port;
  listening.listener->allow_unchecked_sharing();

  std::thread server([&] {
    auto accepted = listening.listener->accept(2s);
    if (!accepted.ok) {
      *server_error = "accept:" + accepted.error_name;
      return;
    }
    std::string request;
    while (request.find(needle) == std::string::npos) {
      RuntimeByteBuffer buf(4096);
      auto read = accepted.stream->read(buf, 2s);
      if (read.eof) {
        break;
      }
      if (!read.ok) {
        *server_error = "read:" + read.error_name;
        return;
      }
      request += buf.bytes();
    }
    if (got_request != nullptr) {
      *got_request = request;
    }
    if (request.find(needle) == std::string::npos) {
      *server_error = "read:marker not found";
      return;
    }
    auto written = accepted.stream->write_all(response, 2s);
    if (!written.ok) {
      *server_error = "write:" + written.error_name;
    }
    accepted.stream->close();
  });

  amber::runtime::ExecutionResult result =
      execute_source(with_port(source, port));
  server.join();
  listening.listener->close();
  return result;
}

amber::runtime::ExecutionResult run_with_persistent_server(
    const std::string &source, const std::vector<std::string> &responses,
    std::string *server_error, std::vector<std::string> *got_requests,
    int *accept_count) {
  auto listening = RuntimeTcpListener::listen({"127.0.0.1", 0});
  expect(listening.ok, "loopback listen failed: " + listening.error_name);
  const std::uint16_t port = listening.listener->local_endpoint().port;
  listening.listener->allow_unchecked_sharing();

  std::thread server([&] {
    auto accepted = listening.listener->accept(2s);
    if (!accepted.ok) {
      *server_error = "accept:" + accepted.error_name;
      return;
    }
    ++*accept_count;
    for (const std::string &response : responses) {
      std::string request;
      while (!request_complete(request)) {
        RuntimeByteBuffer buf(4096);
        auto read = accepted.stream->read(buf, 2s);
        if (read.eof) {
          break;
        }
        if (!read.ok) {
          *server_error = "read:" + read.error_name;
          return;
        }
        request += buf.bytes();
      }
      if (!request_complete(request)) {
        *server_error = "read:request incomplete";
        return;
      }
      if (got_requests != nullptr) {
        got_requests->push_back(request);
      }
      auto written = accepted.stream->write_all(response, 2s);
      if (!written.ok) {
        *server_error = "write:" + written.error_name;
        return;
      }
    }
    accepted.stream->close();
  });

  amber::runtime::ExecutionResult result =
      execute_source(with_port(source, port));
  server.join();
  listening.listener->close();
  return result;
}

amber::runtime::ExecutionResult run_with_two_connection_server(
    const std::string &source, const std::string &first_response,
    const std::string &second_response, std::string *server_error,
    std::vector<std::string> *got_requests, int *accept_count) {
  auto listening = RuntimeTcpListener::listen({"127.0.0.1", 0});
  expect(listening.ok, "loopback listen failed: " + listening.error_name);
  const std::uint16_t port = listening.listener->local_endpoint().port;
  listening.listener->allow_unchecked_sharing();

  std::thread server([&] {
    const std::string responses[2] = {first_response, second_response};
    for (int i = 0; i < 2; ++i) {
      auto accepted = listening.listener->accept(2s);
      if (!accepted.ok) {
        *server_error = "accept:" + accepted.error_name;
        return;
      }
      ++*accept_count;
      std::string request;
      while (!request_complete(request)) {
        RuntimeByteBuffer buf(4096);
        auto read = accepted.stream->read(buf, 2s);
        if (read.eof) {
          break;
        }
        if (!read.ok) {
          *server_error = "read:" + read.error_name;
          return;
        }
        request += buf.bytes();
      }
      if (!request_complete(request)) {
        *server_error = "read:request incomplete";
        return;
      }
      if (got_requests != nullptr) {
        got_requests->push_back(request);
      }
      auto written = accepted.stream->write_all(responses[i], 2s);
      if (!written.ok) {
        *server_error = "write:" + written.error_name;
        return;
      }
      if (i == 0) {
        RuntimeByteBuffer buf(4096);
        auto read = accepted.stream->read(buf, 2s);
        if (!read.eof) {
          *server_error = read.ok ? "read:first connection reused"
                                  : "read:" + read.error_name;
          return;
        }
      }
      accepted.stream->close();
    }
  });

  amber::runtime::ExecutionResult result =
      execute_source(with_port(source, port));
  server.join();
  listening.listener->close();
  return result;
}

amber::runtime::ExecutionResult run_with_two_origin_server(
    const std::string &source, const std::string &first_response_template,
    const std::string &second_response, std::string *server_error,
    std::string *first_request, std::string *second_request) {
  auto first = RuntimeTcpListener::listen({"127.0.0.1", 0});
  expect(first.ok, "first loopback listen failed: " + first.error_name);
  auto second = RuntimeTcpListener::listen({"127.0.0.1", 0});
  expect(second.ok, "second loopback listen failed: " + second.error_name);
  const std::uint16_t port1 = first.listener->local_endpoint().port;
  const std::uint16_t port2 = second.listener->local_endpoint().port;
  first.listener->allow_unchecked_sharing();
  second.listener->allow_unchecked_sharing();

  std::thread first_server([&] {
    auto accepted = first.listener->accept(2s);
    if (!accepted.ok) {
      *server_error = "first accept:" + accepted.error_name;
      return;
    }
    std::string request;
    while (!request_complete(request)) {
      RuntimeByteBuffer buf(4096);
      auto read = accepted.stream->read(buf, 2s);
      if (read.eof) {
        break;
      }
      if (!read.ok) {
        *server_error = "first read:" + read.error_name;
        return;
      }
      request += buf.bytes();
    }
    if (first_request != nullptr) {
      *first_request = request;
    }
    const std::string response =
        with_two_ports(first_response_template, port1, port2);
    auto written = accepted.stream->write_all(response, 2s);
    if (!written.ok) {
      *server_error = "first write:" + written.error_name;
    }
    accepted.stream->close();
  });

  std::thread second_server([&] {
    auto accepted = second.listener->accept(2s);
    if (!accepted.ok) {
      *server_error = "second accept:" + accepted.error_name;
      return;
    }
    std::string request;
    while (!request_complete(request)) {
      RuntimeByteBuffer buf(4096);
      auto read = accepted.stream->read(buf, 2s);
      if (read.eof) {
        break;
      }
      if (!read.ok) {
        *server_error = "second read:" + read.error_name;
        return;
      }
      request += buf.bytes();
    }
    if (second_request != nullptr) {
      *second_request = request;
    }
    auto written = accepted.stream->write_all(second_response, 2s);
    if (!written.ok) {
      *server_error = "second write:" + written.error_name;
    }
    accepted.stream->close();
  });

  amber::runtime::ExecutionResult result =
      execute_source(with_two_ports(source, port1, port2));
  first_server.join();
  second_server.join();
  first.listener->close();
  second.listener->close();
  return result;
}

const char *kResponse = "HTTP/1.1 200 OK\r\n"
                        "Content-Length: 13\r\n"
                        "X-Test: yes\r\n"
                        "\r\n"
                        "Hello, world!";

void expect_ok_int(const amber::runtime::ExecutionResult &result,
                   std::int64_t expected, const std::string &what) {
  if (!result.ok() && result.fault.has_value()) {
    std::cerr << "[fault] " << what << ": " << result.fault->error_name << " / "
              << result.fault->message << "\n";
  }
  expect(result.ok(), what + " should succeed");
  expect(result.value.is_integer(), what + " should return Int");
  expect(result.value.as_integer() == expected, what + " value mismatch");
}

void expect_ok_true(const amber::runtime::ExecutionResult &result,
                    const std::string &what) {
  if (!result.ok() && result.fault.has_value()) {
    std::cerr << "[fault] " << what << ": " << result.fault->error_name << " / "
              << result.fault->message << "\n";
  }
  expect(result.ok(), what + " should succeed");
  expect(result.value.is_bool() && result.value.as_bool(),
         what + " should return true");
}

void expect_ok_string(const amber::runtime::ExecutionResult &result,
                      const std::string &expected, const std::string &what) {
  if (!result.ok() && result.fault.has_value()) {
    std::cerr << "[fault] " << what << ": " << result.fault->error_name << " / "
              << result.fault->message << "\n";
  }
  expect(result.ok(), what + " should succeed");
  expect(result.value.is_string(), what + " should return Str");
  expect(result.value.as_string().string_id < result.runtime_strings.size(),
         what + " string id in range");
  expect(result.runtime_strings[result.value.as_string().string_id] == expected,
         what + " value mismatch");
}

void test_get_status() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "res = net.http.Client().get(\"http://127.0.0.1:%PORT%/\")\n"
      "res.status()\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 200, "get().status()");
}

void test_get_body_text() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "res = net.http.Client().get(\"http://127.0.0.1:%PORT%/\")\n"
      "res.body_text() == \"Hello, world!\"\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_true(result, "get().body_text()");
}

void test_get_ok_predicate() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "res = net.http.Client().get(\"http://127.0.0.1:%PORT%/\")\n"
      "res.ok?()\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_true(result, "get().ok?()");
}

void test_get_headers_value() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "res = net.http.Client().get(\"http://127.0.0.1:%PORT%/\")\n"
      "res.headers().first(\"X-Test\") == \"yes\"\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_true(result, "get().headers() Headers value, case-insensitive");
}

void test_response_headers_read_only() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "res = net.http.Client().get(\"http://127.0.0.1:%PORT%/\")\n"
      "res.headers().add!(\"x\", \"y\")\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect(!result.ok() && result.fault.has_value() &&
             result.fault->error_name == "TypeError",
         "mutating response headers -> TypeError (read-only)");
}

void test_headers_value_api() {
  // Duplicate preservation + case-insensitive all().
  const amber::runtime::ExecutionResult dup =
      execute_source("import net\n"
                     "h = net.http.Headers()\n"
                     "h.add!(\"X-Tag\", \"one\")\n"
                     "h.add!(\"x-tag\", \"two\")\n"
                     "h.all(\"X-TAG\").size()\n");
  expect_ok_int(dup, 2, "Headers add! duplicate + case-insensitive all()");

  // first() returns the earliest line, case-insensitively.
  const amber::runtime::ExecutionResult first =
      execute_source("import net\n"
                     "h = net.http.Headers()\n"
                     "h.add!(\"X-Tag\", \"one\")\n"
                     "h.add!(\"x-tag\", \"two\")\n"
                     "h.first(\"x-tag\") == \"one\"\n");
  expect_ok_true(first, "Headers first() earliest line");

  // set! replaces all lines for a name.
  const amber::runtime::ExecutionResult set =
      execute_source("import net\n"
                     "h = net.http.Headers()\n"
                     "h.add!(\"x-tag\", \"one\")\n"
                     "h.add!(\"x-tag\", \"two\")\n"
                     "h.set!(\"x-tag\", \"only\")\n"
                     "h.all(\"x-tag\").size()\n");
  expect_ok_int(set, 1, "Headers set! replaces all");
}

void test_headers_include_and_combined() {
  const amber::runtime::ExecutionResult result = execute_source(
      "import net\n"
      "h = net.http.Headers()\n"
      "h.add!(\"Accept\", \"text/html\")\n"
      "h.add!(\"accept\", \"application/json\")\n"
      "h.combined(\"accept\") == \"text/html, application/json\"\n");
  expect_ok_true(result, "Headers combined() joins list field");
}

void test_request_accepts_headers_value() {
  std::string server_error;
  const amber::runtime::ExecutionResult result =
      run_with_server("import net\n"
                      "h = net.http.Headers()\n"
                      "h.add!(\"accept\", \"application/json\")\n"
                      "req = net.http.Request(method: :get, "
                      "url: \"http://127.0.0.1:%PORT%/\", headers: h)\n"
                      "net.http.Client().send(req).status()\n",
                      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 200, "Request accepts a Headers value");
}

void test_from_import_headers() {
  const amber::runtime::ExecutionResult result =
      execute_source("from net.http import Headers\n"
                     "h = Headers()\n"
                     "h.add!(\"a\", \"b\")\n"
                     "h.include?(\"A\")\n");
  expect_ok_true(result, "from net.http import Headers; include?");
}

void test_capability_denied() {
  // A world with no capability grants must deny net.connect *before* any
  // socket opens (§20.1 / §25.9). Port 9 is never contacted.
  amber::bytecode::BcModule module =
      compile_source_or_die("import net\n"
                            "net.http.Client().get(\"http://127.0.0.1:9/\")\n");
  amber::runtime::RuntimeWorldOptions options; // no net.connect grant
  amber::runtime::RuntimeWorld world(module, options);
  const amber::runtime::ExecutionResult result =
      world.execute(module.init.entry_code_id);
  expect(!result.ok() && result.fault.has_value(),
         "denied net.connect should fault");
  expect(result.fault->error_name == "CapabilityError",
         "denied net.connect -> CapabilityError, got " +
             (result.fault.has_value() ? result.fault->error_name : ""));
}

void test_scoped_block_form() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "net.http.Client().get(\"http://127.0.0.1:%PORT%/\") |res|:\n"
      "  res.status()\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 200, "scoped block get");
}

void test_pool_reuses_after_body_read() {
  std::string server_error;
  std::vector<std::string> requests;
  int accepts = 0;
  const amber::runtime::ExecutionResult result = run_with_persistent_server(
      "import net\n"
      "client = net.http.Client(max_idle_connections: 4, "
      "max_idle_per_origin: 2)\n"
      "a = client.get(\"http://127.0.0.1:%PORT%/one\").body_text()\n"
      "b = client.get(\"http://127.0.0.1:%PORT%/two\").body_text()\n"
      "\"#{a}|#{b}\"\n",
      {"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none",
       "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo"},
      &server_error, &requests, &accepts);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_string(result, "one|two", "pool reuse after body read");
  expect(accepts == 1, "pooled requests reuse one TCP connection");
  expect(requests.size() == 2, "persistent server saw two requests");
}

void test_close_idle_closes_pooled_connection() {
  std::string server_error;
  std::vector<std::string> requests;
  int accepts = 0;
  const amber::runtime::ExecutionResult result = run_with_two_connection_server(
      "import net\n"
      "client = net.http.Client(max_idle_connections: 4, "
      "max_idle_per_origin: 2)\n"
      "a = client.get(\"http://127.0.0.1:%PORT%/one\").body_text()\n"
      "client.close_idle!()\n"
      "b = client.get(\"http://127.0.0.1:%PORT%/two\").body_text()\n"
      "\"#{a}|#{b}\"\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none",
      "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo", &server_error,
      &requests, &accepts);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_string(result, "one|two", "close_idle! closes pooled connection");
  expect(accepts == 2, "close_idle! forces a second TCP connection");
  expect(requests.size() == 2, "two-connection server saw two requests");
}

void test_pool_does_not_reuse_after_early_close() {
  std::string server_error;
  std::vector<std::string> requests;
  int accepts = 0;
  const amber::runtime::ExecutionResult result = run_with_two_connection_server(
      "import net\n"
      "client = net.http.Client(max_idle_connections: 4, "
      "max_idle_per_origin: 2)\n"
      "res = client.get(\"http://127.0.0.1:%PORT%/one\")\n"
      "status = res.status()\n"
      "res.close!()\n"
      "b = client.get(\"http://127.0.0.1:%PORT%/two\").body_text()\n"
      "\"#{status}:#{b}\"\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
      "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo", &server_error,
      &requests, &accepts);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_string(result, "200:two",
                   "early response close prevents pool reuse");
  expect(accepts == 2, "early close forces a second TCP connection");
  expect(requests.size() == 2, "early-close server saw two requests");
}

void test_pool_timeout_when_active_slot_unavailable() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result =
      run_with_server_until_request_contains(
          "import net\n"
          "client = net.http.Client(max_active_per_origin: 1)\n"
          "h = client.begin(method: :post, "
          "url: \"http://127.0.0.1:%PORT%/hold\", length: null)\n"
          "caught = \"none\"\n"
          "try:\n"
          "  client.get(\"http://127.0.0.1:%PORT%/blocked\", "
          "pool_timeout: 0)\n"
          "rescue PoolTimeoutError:\n"
          "  caught = \"pool\"\n"
          "h.abort!()\n"
          "caught == \"pool\"\n",
          "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", "\r\n\r\n",
          &server_error, &request);
  expect(server_error.empty(), "server error: " + server_error);
  expect(request.find("/hold") != std::string::npos,
         "pool timeout test opened the held request");
  expect_ok_true(result, "max_active_per_origin observes pool_timeout");
}

void test_redirect_off_returns_3xx() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "net.http.Client().get(\"http://127.0.0.1:%PORT%/start\").status()\n",
      "HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\n\r\n",
      &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 302, "default redirects: :off returns 3xx");
}

void test_redirect_manual_exposes_location() {
  std::string server_error;
  const amber::runtime::ExecutionResult result =
      run_with_server("import net\n"
                      "res = net.http.Client(redirects: :manual).get("
                      "\"http://127.0.0.1:%PORT%/start\")\n"
                      "res.status() == 302 and res.redirect_location() == "
                      "\"http://127.0.0.1:%PORT%/final\"\n",
                      "HTTP/1.1 302 Found\r\nLocation: /final#frag\r\n"
                      "Content-Length: 0\r\n\r\n",
                      &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_true(result, "redirects: :manual exposes parsed Location");
}

void test_redirect_safe_follows_relative_and_records() {
  std::string server_error;
  std::vector<std::string> requests;
  int accepts = 0;
  const amber::runtime::ExecutionResult result = run_with_persistent_server(
      "import net\n"
      "client = net.http.Client(redirects: :safe)\n"
      "res = client.get(\"http://127.0.0.1:%PORT%/start\")\n"
      "rec = res.redirects()[0]\n"
      "\"#{res.status()}:#{res.body_text()}:#{res.redirects().size()}:"
      "#{rec.status()}:#{rec.method_before()}:#{rec.method_after()}:"
      "#{rec.to_url() == \"http://127.0.0.1:%PORT%/final\"}\"\n",
      {"HTTP/1.1 302 Found\r\nLocation: /final\r\n"
       "Content-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone"},
      &server_error, &requests, &accepts);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_string(result, "200:done:1:302:GET:GET:true",
                   "redirects: :safe follows GET and records hop");
  expect(accepts == 1, "safe redirect reuses drained same-origin connection");
  expect(requests.size() == 2, "safe redirect server saw two requests");
  expect(requests[0].find("GET /start HTTP/1.1\r\n") == 0,
         "first redirect request path");
  expect(requests[1].find("GET /final HTTP/1.1\r\n") == 0,
         "second redirect request path");
}

void test_redirect_303_rewrites_post_to_get() {
  std::string server_error;
  std::vector<std::string> requests;
  int accepts = 0;
  const amber::runtime::ExecutionResult result = run_with_persistent_server(
      "import net\n"
      "client = net.http.Client(redirects: :safe)\n"
      "res = client.post(\"http://127.0.0.1:%PORT%/submit\", "
      "body: \"payload\")\n"
      "rec = res.redirects()[0]\n"
      "\"#{res.body_text()}:#{rec.method_before()}:#{rec.method_after()}\"\n",
      {"HTTP/1.1 303 See Other\r\nLocation: /done\r\n"
       "Content-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"},
      &server_error, &requests, &accepts);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_string(result, "ok:POST:GET", "303 rewrites POST to GET");
  expect(requests.size() == 2, "303 server saw two requests");
  expect(requests[0].find("POST /submit HTTP/1.1\r\n") == 0,
         "first 303 request is POST");
  expect(requests[0].find("payload") != std::string::npos,
         "first 303 request carries body");
  expect(requests[1].find("GET /done HTTP/1.1\r\n") == 0,
         "second 303 request is GET");
  expect(requests[1].find("payload") == std::string::npos,
         "303 redirect drops request body");
  expect(requests[1].find("content-length:") == std::string::npos,
         "303 redirect drops Content-Length");
}

void test_redirect_cross_origin_strips_credentials_and_host() {
  std::string server_error;
  std::string first_request;
  std::string second_request;
  const amber::runtime::ExecutionResult result = run_with_two_origin_server(
      "import net\n"
      "headers = net.http.Headers()\n"
      "headers.add!(\"authorization\", \"Bearer secret\")\n"
      "headers.add!(\"cookie\", \"sid=secret\")\n"
      "headers.add!(\"host\", \"caller.example\")\n"
      "client = net.http.Client(redirects: :safe)\n"
      "res = client.get(\"http://127.0.0.1:%PORT1%/start\", "
      "headers: headers)\n"
      "rec = res.redirects()[0]\n"
      "\"#{res.body_text()}:#{rec.cross_origin?()}\"\n",
      "HTTP/1.1 302 Found\r\n"
      "Location: http://127.0.0.1:%PORT2%/landing\r\n"
      "Content-Length: 0\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nlanding", &server_error,
      &first_request, &second_request);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_string(result, "landing:true",
                   "cross-origin redirect follows and records origin change");
  expect(first_request.find("authorization: Bearer secret\r\n") !=
             std::string::npos,
         "first origin receives authorization");
  expect(first_request.find("cookie: sid=secret\r\n") != std::string::npos,
         "first origin receives cookie");
  expect(first_request.find("host: caller.example\r\n") != std::string::npos,
         "first origin receives caller Host");
  expect(second_request.find("authorization:") == std::string::npos,
         "redirect strips authorization across origin");
  expect(second_request.find("cookie:") == std::string::npos,
         "redirect strips cookie across origin");
  expect(second_request.find("host: caller.example\r\n") == std::string::npos,
         "redirect strips caller Host");
  expect(second_request.find("GET /landing HTTP/1.1\r\n") == 0,
         "second origin receives redirected path");
}

void test_redirect_unsupported_scheme_raises() {
  std::string server_error;
  const amber::runtime::ExecutionResult result =
      run_with_server("import net\n"
                      "client = net.http.Client(redirects: :safe)\n"
                      "client.get(\"http://127.0.0.1:%PORT%/start\")\n",
                      "HTTP/1.1 302 Found\r\nLocation: https://example.com/\r\n"
                      "Content-Length: 0\r\n\r\n",
                      &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect(!result.ok() && result.fault.has_value() &&
             result.fault->error_name == "UnsupportedSchemeError",
         "safe redirect to https -> UnsupportedSchemeError");
}

void test_redirect_max_redirects_raises() {
  std::string server_error;
  std::vector<std::string> requests;
  int accepts = 0;
  const amber::runtime::ExecutionResult result = run_with_persistent_server(
      "import net\n"
      "client = net.http.Client(redirects: :safe, max_redirects: 1)\n"
      "client.get(\"http://127.0.0.1:%PORT%/one\")\n",
      {"HTTP/1.1 302 Found\r\nLocation: /two\r\n"
       "Content-Length: 0\r\n\r\n",
       "HTTP/1.1 302 Found\r\nLocation: /three\r\n"
       "Content-Length: 0\r\n\r\n"},
      &server_error, &requests, &accepts);
  expect(server_error.empty(), "server error: " + server_error);
  expect(!result.ok() && result.fault.has_value() &&
             result.fault->error_name == "TooManyRedirectsError",
         "max_redirects -> TooManyRedirectsError");
  expect(requests.size() == 2, "max_redirects follows only allowed hop");
}

void test_redirect_nonreplayable_body_raises() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result = run_with_server_capture(
      "import net\n"
      "body = net.http.RequestBody.stream(length: null) |w|:\n"
      "  w.write_all!(\"abc\".bytes())\n"
      "req = net.http.Request(method: :get, "
      "url: \"http://127.0.0.1:%PORT%/start\", body: body)\n"
      "net.http.Client(redirects: :safe).send(req)\n",
      "HTTP/1.1 307 Temporary Redirect\r\nLocation: /again\r\n"
      "Content-Length: 0\r\n\r\n",
      &server_error, &request);
  expect(server_error.empty(), "server error: " + server_error);
  expect(!result.ok() && result.fault.has_value() &&
             result.fault->error_name == "NonReplayableRedirectError",
         "307 with non-replayable GET body -> NonReplayableRedirectError");
}

void test_post_body() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "res = net.http.Client().post(\"http://127.0.0.1:%PORT%/\", "
      "body: \"payload\")\n"
      "res.status()\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 200, "post with body");
}

void test_request_body_text_static() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result = run_with_server_capture(
      "import net\n"
      "body = net.http.RequestBody.text(\"payload\")\n"
      "res = net.http.Client().post(\"http://127.0.0.1:%PORT%/\", body: body)\n"
      "res.status()\n",
      kResponse, &server_error, &request);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 200, "RequestBody.text static body");
  expect(request.find("content-length: 7\r\n") != std::string::npos,
         "static RequestBody emits Content-Length");
  expect(request.rfind("payload") == request.size() - 7U,
         "static RequestBody payload sent");
}

void test_request_body_stream_chunked() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result = run_with_server_capture(
      "import net\n"
      "body = net.http.RequestBody.stream(length: null) |w|:\n"
      "  w.write_all!(\"abc\".bytes())\n"
      "  w.write_all!(\"def\".bytes())\n"
      "res = net.http.Client().post(\"http://127.0.0.1:%PORT%/\", body: body)\n"
      "res.body_text() == \"Hello, world!\"\n",
      kResponse, &server_error, &request);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_true(result, "RequestBody.stream chunked body");
  expect(request.find("transfer-encoding: chunked\r\n") != std::string::npos,
         "producer body emits chunked transfer");
  expect(request.find("3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n") != std::string::npos,
         "producer chunks sent");
}

void test_request_body_from_reader_fixed() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result = run_with_server_capture(
      "import io\n"
      "import net\n"
      "pair = io.Pipe.new(capacity: 16)\n"
      "pair[1].write_all!(\"reader\".bytes())\n"
      "pair[1].close!()\n"
      "body = net.http.RequestBody.from_reader(pair[0], length: 6)\n"
      "res = net.http.Client().post(\"http://127.0.0.1:%PORT%/\", body: body)\n"
      "res.status()\n",
      kResponse, &server_error, &request);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 200, "RequestBody.from_reader fixed body");
  expect(request.find("content-length: 6\r\n") != std::string::npos,
         "reader body emits Content-Length");
  expect(request.rfind("reader") == request.size() - 6U,
         "reader body payload sent");
}

void test_get_json_helper_and_response_json() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result = run_with_server_capture(
      "from net.http.json import get_json\n"
      "payload = get_json(\"http://127.0.0.1:%PORT%/data\").json()\n"
      "payload[:answer]\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\n{\"answer\":42}",
      &server_error, &request);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 42, "get_json helper returns response with json()");
  expect(request.find("accept: application/json\r\n") != std::string::npos,
         "get_json sends JSON Accept header");
}

void test_post_json_helper_sends_json_defaults() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result = run_with_server_capture(
      "from net.http.json import post_json\n"
      "res = post_json(\"http://127.0.0.1:%PORT%/users\", "
      "{name: \"Ada\", n: 2})\n"
      "res.expect_status!(201).status()\n",
      "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n", &server_error,
      &request);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 201, "post_json helper status");
  expect(request.find("content-type: application/json\r\n") !=
             std::string::npos,
         "post_json sends JSON Content-Type");
  expect(request.find("accept: application/json\r\n") != std::string::npos,
         "post_json sends JSON Accept");
  expect(request.find("\"name\":\"Ada\"") != std::string::npos &&
             request.find("\"n\":2") != std::string::npos,
         "post_json sends generated JSON body");
}

void test_form_body_encodes_and_sets_content_type() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result = run_with_server_capture(
      "from net.http import Client\n"
      "from net.http.form import FormBody\n"
      "body = FormBody({name: \"Ada Lovelace\", tags: [\"math\", \"code\"]})\n"
      "Client().post(\"http://127.0.0.1:%PORT%/form\", body: body).status()\n",
      kResponse, &server_error, &request);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 200, "FormBody post status");
  expect(request.find("content-type: application/x-www-form-urlencoded\r\n") !=
             std::string::npos,
         "FormBody sets form Content-Type");
  expect(request.find("name=Ada+Lovelace&tags[]=math&tags[]=code") !=
             std::string::npos,
         "FormBody percent-encodes query body");
}

void test_http_trace_hook_events() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "seen = \"\"\n"
      "client = net.http.Client(trace: net.http.trace |event|:\n"
      "  seen = seen + event[:name] + \",\"\n"
      ")\n"
      "res = client.get(\"http://127.0.0.1:%PORT%/trace\")\n"
      "res.body_text()\n"
      "seen.contains?(\"request.start\") and "
      "seen.contains?(\"read.headers.end\") and "
      "seen.contains?(\"read.body.eof\")\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_true(result, "Client(trace:) emits request/body events");
}

void test_manual_request_handle_chunked() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result = run_with_server_capture(
      "import net\n"
      "h = net.http.Client().begin(method: :post, "
      "url: \"http://127.0.0.1:%PORT%/\", length: null)\n"
      "h.write_all!(\"one\".bytes())\n"
      "h.write_all!(\"two\".bytes())\n"
      "h.finish!()\n"
      "res = h.response()\n"
      "res.status()\n",
      kResponse, &server_error, &request);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 200, "manual RequestHandle chunked body");
  expect(request.find("3\r\none\r\n3\r\ntwo\r\n0\r\n\r\n") != std::string::npos,
         "manual handle chunks sent");
}

void test_manual_request_handle_underwrite() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result =
      run_with_server_capture("import net\n"
                              "h = net.http.Client().begin(method: :post, "
                              "url: \"http://127.0.0.1:%PORT%/\", length: 4)\n"
                              "h.write_all!(\"ab\".bytes())\n"
                              "h.finish!()\n",
                              kResponse, &server_error, &request);
  (void)request;
  expect(!result.ok() && result.fault.has_value() &&
             result.fault->error_name == "BodyLengthError",
         "manual underwrite -> BodyLengthError");
}

void test_request_handle_write_after_response_state() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result = run_with_server_capture(
      "import net\n"
      "h = net.http.Client().begin(method: :post, "
      "url: \"http://127.0.0.1:%PORT%/\", length: null)\n"
      "h.write_all!(\"one\".bytes())\n"
      "h.finish!()\n"
      "h.response()\n"
      "h.write_all!(\"late\".bytes())\n",
      kResponse, &server_error, &request);
  expect(server_error.empty(), "server error: " + server_error);
  expect(!result.ok() && result.fault.has_value() &&
             result.fault->error_name == "RequestStateError" &&
             result.fault->message.find("already been returned") !=
                 std::string::npos,
         "write after response -> already returned RequestStateError");
}

void test_request_handle_early_response_is_terminal() {
  std::string server_error;
  std::string request;
  const amber::runtime::ExecutionResult result =
      run_with_server_until_request_contains(
          "import net\n"
          "h = net.http.Client().begin(method: :post, "
          "url: \"http://127.0.0.1:%PORT%/\", length: null)\n"
          "h.write_all!(\"one\".bytes())\n"
          "res = h.response()\n"
          "caught = \"none\"\n"
          "try:\n"
          "  h.write_all!(\"late\".bytes())\n"
          "rescue RequestStateError |e|:\n"
          "  caught = \"#{res.status()}:#{e.message()}\"\n"
          "caught\n",
          kResponse, "\r\none\r\n", &server_error, &request);
  expect(server_error.empty(), "server error: " + server_error);
  expect(request.find("transfer-encoding: chunked\r\n") != std::string::npos,
         "early-response request is chunked");
  expect_ok_string(result, "200:HTTP response has already been returned",
                   "early response before finish is terminal");
}

void test_response_body_read_chunks() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import io\n"
      "import net\n"
      "res = net.http.Client().get(\"http://127.0.0.1:%PORT%/\")\n"
      "body = res.body()\n"
      "a = io.ByteBuffer(5)\n"
      "b = io.ByteBuffer(32)\n"
      "n1 = body.read!(a)\n"
      "n2 = body.read!(b)\n"
      "\"#{n1}:#{a.bytes().to_str()}:#{n2}:#{b.bytes().to_str()}\"\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect(result.ok(), "ResponseBody read! should succeed");
  expect(result.value.is_string(), "ResponseBody read! result Str");
  expect(result.value.as_string().string_id < result.runtime_strings.size(),
         "ResponseBody read! string id in range");
  expect(result.runtime_strings[result.value.as_string().string_id] ==
             "5:Hello:8:, world!",
         "ResponseBody read! chunks match");
}

void test_response_body_single_consumer() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "res = net.http.Client().get(\"http://127.0.0.1:%PORT%/\")\n"
      "res.body().read(max_bytes: 5)\n"
      "res.body_text()\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect(!result.ok() && result.fault.has_value() &&
             result.fault->error_name == "BodyConsumedError",
         "mixed body consumption -> BodyConsumedError");
}

void test_response_body_each_chunk_propagates_block_failure() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "res = net.http.Client().get(\"http://127.0.0.1:%PORT%/\")\n"
      "body = res.body()\n"
      "caught = \"none\"\n"
      "try:\n"
      "  body.each_chunk(size: 5) |chunk|:\n"
      "    raise ValueError(\"stop\")\n"
      "  caught = \"no error\"\n"
      "rescue ValueError |e|:\n"
      "  caught = \"#{e.message()}:#{body.closed?()}\"\n"
      "caught\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_string(result, "stop:true",
                   "ResponseBody.each_chunk block rescue and cleanup");
}

void test_unsupported_scheme_raises() {
  // No server needed; the URL is rejected before any connection.
  const amber::runtime::ExecutionResult result =
      execute_source("import net\n"
                     "net.http.Client().get(\"https://127.0.0.1/\")\n");
  expect(!result.ok() && result.fault.has_value(), "https get should fault");
  expect(result.fault->error_name == "UnsupportedSchemeError",
         "https -> UnsupportedSchemeError, got " +
             (result.fault.has_value() ? result.fault->error_name : ""));
}

void test_connection_refused_raises() {
  // Bind then close a listener to get a refused port, then GET it.
  auto listening = RuntimeTcpListener::listen({"127.0.0.1", 0});
  expect(listening.ok, "listen failed");
  const std::uint16_t port = listening.listener->local_endpoint().port;
  listening.listener->close();

  const amber::runtime::ExecutionResult result = execute_source(
      with_port("import net\n"
                "net.http.Client().get(\"http://127.0.0.1:%PORT%/\")\n",
                port));
  expect(!result.ok() && result.fault.has_value(), "refused get should fault");
  expect(result.fault->error_name == "ConnectionError",
         "refused -> ConnectionError, got " +
             (result.fault.has_value() ? result.fault->error_name : ""));
}

void test_rescue_unsupported_scheme() {
  // The mapped error is a rescuable HttpError subclass.
  const amber::runtime::ExecutionResult result =
      execute_source("import net\n"
                     "caught = 0\n"
                     "try:\n"
                     "  net.http.Client().get(\"https://127.0.0.1/\")\n"
                     "rescue HttpError:\n"
                     "  caught = 1\n"
                     "caught\n");
  expect_ok_int(result, 1, "UnsupportedSchemeError rescued as HttpError");
}

void test_send_request() {
  std::string server_error;
  const amber::runtime::ExecutionResult result =
      run_with_server("import net\n"
                      "req = net.http.Request(method: :get, "
                      "url: \"http://127.0.0.1:%PORT%/\")\n"
                      "net.http.Client().send(req).status()\n",
                      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 200, "client.send(request).status()");
}

void test_send_post_request_body() {
  std::string server_error;
  const amber::runtime::ExecutionResult result =
      run_with_server("import net\n"
                      "req = net.http.Request(method: :post, "
                      "url: \"http://127.0.0.1:%PORT%/\", body: \"hi\")\n"
                      "net.http.Client().send(req).ok?()\n",
                      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_true(result, "send POST request with body");
}

void test_request_method_normalized() {
  const amber::runtime::ExecutionResult result =
      execute_source("import net\n"
                     "req = net.http.Request(method: :post, url: "
                     "\"http://h/x\")\n"
                     "req.method() == \"POST\"\n");
  expect_ok_true(result, "request.method() normalized to POST");
}

void test_request_content_length() {
  const amber::runtime::ExecutionResult result =
      execute_source("import net\n"
                     "req = net.http.Request(method: \"post\", url: "
                     "\"http://h/x\", body: \"abcd\")\n"
                     "req.content_length()\n");
  expect_ok_int(result, 4, "request.content_length() from body");
}

void test_request_invalid_method() {
  const amber::runtime::ExecutionResult result =
      execute_source("import net\n"
                     "net.http.Request(method: \"bad method\", url: "
                     "\"http://h/x\")\n");
  expect(!result.ok() && result.fault.has_value() &&
             result.fault->error_name == "InvalidMethodError",
         "bad method -> InvalidMethodError, got " +
             (result.fault.has_value() ? result.fault->error_name : ""));
}

void test_request_invalid_url_scheme() {
  const amber::runtime::ExecutionResult result =
      execute_source("import net\n"
                     "net.http.Request(method: :get, url: \"https://h/x\")\n");
  expect(!result.ok() && result.fault.has_value() &&
             result.fault->error_name == "UnsupportedSchemeError",
         "https Request -> UnsupportedSchemeError, got " +
             (result.fault.has_value() ? result.fault->error_name : ""));
}

void test_from_import_client() {
  std::string server_error;
  const amber::runtime::ExecutionResult result =
      run_with_server("from net.http import Client\n"
                      "Client().get(\"http://127.0.0.1:%PORT%/\").status()\n",
                      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 200, "from net.http import Client; Client().get");
}

void test_from_import_request_send() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "from net.http import Client, Request\n"
      "req = Request(method: :get, url: \"http://127.0.0.1:%PORT%/\")\n"
      "Client().send(req).status()\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_int(result, 200, "from net.http import Request + send");
}

void test_http_server_serves_request_hook() {
  const amber::runtime::ExecutionResult result = execute_source(
      "import task\n"
      "from net.http import Client, Server, ServerResponse\n"
      "\n"
      "server = Server(host: \"127.0.0.1\", port: 0, workers: 2, "
      "max_concurrent_per_worker: 2)\n"
      "port = server.port()\n"
      "runner = task.spawn:\n"
      "  server.serve(max_requests: 1) |req|:\n"
      "    ServerResponse.text("
      "\"#{req.method()}:#{req.path()}:#{req.query()}:#{req.body_text()}\", "
      "headers: {\"x-seen\": \"yes\"})\n"
      "\n"
      "res = Client().post(\"http://127.0.0.1:#{port}/submit?x=1\", "
      "body: \"hello\")\n"
      "body = res.body_text()\n"
      "runner.wait()\n"
      "res.status() == 200 and body == \"POST:/submit:x=1:hello\" and "
      "res.headers().first(\"x-seen\") == \"yes\"\n");
  expect_ok_true(result, "net.http.Server serve hook responds");
}

void test_http_server_allows_cooperative_concurrency_per_worker() {
  const amber::runtime::ExecutionResult result = execute_source(
      "import task\n"
      "from net.http import Client, Server, ServerResponse\n"
      "\n"
      "server = Server(host: \"127.0.0.1\", port: 0, workers: 1, "
      "max_concurrent_per_worker: 2)\n"
      "port = server.port()\n"
      "runner = task.spawn:\n"
      "  server.serve(max_requests: 2) |req|:\n"
      "    if req.path() == \"/slow\":\n"
      "      task.sleep(20)\n"
      "    ServerResponse.text(req.path())\n"
      "\n"
      "a = task.spawn:\n"
      "  Client().get(\"http://127.0.0.1:#{port}/slow\").body_text()\n"
      "b = task.spawn:\n"
      "  Client().get(\"http://127.0.0.1:#{port}/fast\").body_text()\n"
      "ra = a.wait()\n"
      "rb = b.wait()\n"
      "runner.wait()\n"
      "server.workers() == 1 and server.max_concurrent_per_worker() == 2 and "
      "((ra == \"/slow\" and rb == \"/fast\") or "
      "(ra == \"/fast\" and rb == \"/slow\"))\n");
  expect_ok_true(result,
                 "net.http.Server workers/concurrency handles parked hooks");
}

} // namespace

int main() {
  test_from_import_client();
  test_from_import_request_send();
  test_http_server_serves_request_hook();
  test_http_server_allows_cooperative_concurrency_per_worker();
  test_get_status();
  test_get_body_text();
  test_get_ok_predicate();
  test_get_headers_value();
  test_response_headers_read_only();
  test_headers_value_api();
  test_headers_include_and_combined();
  test_request_accepts_headers_value();
  test_from_import_headers();
  test_scoped_block_form();
  test_pool_reuses_after_body_read();
  test_close_idle_closes_pooled_connection();
  test_pool_does_not_reuse_after_early_close();
  test_pool_timeout_when_active_slot_unavailable();
  test_redirect_off_returns_3xx();
  test_redirect_manual_exposes_location();
  test_redirect_safe_follows_relative_and_records();
  test_redirect_303_rewrites_post_to_get();
  test_redirect_cross_origin_strips_credentials_and_host();
  test_redirect_unsupported_scheme_raises();
  test_redirect_max_redirects_raises();
  test_redirect_nonreplayable_body_raises();
  test_post_body();
  test_request_body_text_static();
  test_request_body_stream_chunked();
  test_request_body_from_reader_fixed();
  test_get_json_helper_and_response_json();
  test_post_json_helper_sends_json_defaults();
  test_form_body_encodes_and_sets_content_type();
  test_http_trace_hook_events();
  test_manual_request_handle_chunked();
  test_manual_request_handle_underwrite();
  test_request_handle_write_after_response_state();
  test_request_handle_early_response_is_terminal();
  test_response_body_read_chunks();
  test_response_body_single_consumer();
  test_response_body_each_chunk_propagates_block_failure();
  test_send_request();
  test_send_post_request_body();
  test_request_method_normalized();
  test_request_content_length();
  test_request_invalid_method();
  test_request_invalid_url_scheme();
  test_unsupported_scheme_raises();
  test_connection_refused_raises();
  test_rescue_unsupported_scheme();
  test_capability_denied();
  std::cout << "vm net.http tests passed (" << g_checks << " checks)\n";
  return 0;
}
