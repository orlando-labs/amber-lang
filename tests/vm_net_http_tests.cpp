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

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

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

// Run `source` (with %PORT% substituted) while a one-shot loopback server
// replies with `response`. Returns the execution result.
amber::runtime::ExecutionResult run_with_server(const std::string &source,
                                                const std::string &response,
                                                std::string *server_error) {
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
    while (request.find("\r\n\r\n") == std::string::npos) {
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

void test_get_headers_map() {
  std::string server_error;
  const amber::runtime::ExecutionResult result = run_with_server(
      "import net\n"
      "res = net.http.Client().get(\"http://127.0.0.1:%PORT%/\")\n"
      "res.headers()[\"x-test\"][0] == \"yes\"\n",
      kResponse, &server_error);
  expect(server_error.empty(), "server error: " + server_error);
  expect_ok_true(result, "get().headers() map");
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

} // namespace

int main() {
  test_from_import_client();
  test_from_import_request_send();
  test_get_status();
  test_get_body_text();
  test_get_ok_predicate();
  test_get_headers_map();
  test_scoped_block_form();
  test_post_body();
  test_send_request();
  test_send_post_request_body();
  test_request_method_normalized();
  test_request_content_length();
  test_request_invalid_method();
  test_request_invalid_url_scheme();
  test_unsupported_scheme_raises();
  test_connection_refused_raises();
  test_rescue_unsupported_scheme();
  std::cout << "vm net.http tests passed (" << g_checks << " checks)\n";
  return 0;
}
