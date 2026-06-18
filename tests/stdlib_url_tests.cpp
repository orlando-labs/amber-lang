#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/vm.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib url test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<stdlib-url-source-test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    std::exit(1);
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(emit_result.diagnostics);
    std::exit(1);
  }
  amber::bytecode::DecodeResult decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(emit_result.module));
  if (!decoded.ok()) {
    std::cerr << amber::bytecode::verify_errors_to_json(decoded.errors);
    std::exit(1);
  }
  return std::move(decoded.module);
}

amber::runtime::ExecutionResult execute_source(const std::string &source) {
  amber::bytecode::BcModule module = compile_source_or_die(source);
  expect(module.init.has_entry_code_id, "source module should have init code");
  return amber::runtime::execute_code(module, module.init.entry_code_id);
}

void expect_ok_integer(const amber::runtime::ExecutionResult &result,
                       std::int64_t expected, const std::string &message) {
  if (!result.ok() && result.fault.has_value()) {
    std::cerr << "[fault] " << message << ": " << result.fault->error_name
              << " / " << result.fault->message << "\n";
  }
  expect(result.ok(), message + " should succeed");
  expect(result.value.is_integer(), message + " should return Int");
  expect(result.value.as_integer() == expected, message + " value");
}

void expect_fault(const std::string &source, const std::string &error_name,
                  const std::string &message) {
  const amber::runtime::ExecutionResult result = execute_source(source);
  expect(!result.ok() && result.fault.has_value(), message + " should fault");
  expect(result.fault->error_name == error_name,
         message + " should fault with " + error_name + ", got " +
             (result.fault.has_value() ? result.fault->error_name : ""));
}

void test_parse_and_build_roundtrip() {
  const amber::runtime::ExecutionResult result = execute_source(
      "u = Url.parse(\"HTTP://User@Example.COM:8080/a/b?x=1&x=2&space=a+b#frag\")\n"
      "q = u[\"query_map\"]\n"
      "round = Url.build(u)\n"
      "if u[\"scheme\"] == \"http\" and u[\"host\"] == \"example.com\" and "
      "u[\"port\"] == 8080 and u[\"path\"] == \"/a/b\" and "
      "u[\"query\"] == \"x=1&x=2&space=a+b\" and "
      "u[\"fragment\"] == \"frag\" and q[\"x\"][0] == \"1\" and "
      "q[\"x\"][1] == \"2\" and q[\"space\"] == \"a b\" and "
      "round == \"http://User@example.com:8080/a/b?x=1&x=2&space=a+b#frag\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "URL parse/build roundtrip");
}

void test_percent_and_query_helpers() {
  const amber::runtime::ExecutionResult result = execute_source(
      "encoded = Url.percent_encode(\"a b/!\")\n"
      "decoded = Url.percent_decode(encoded)\n"
      "query = Url.build_query({\"q\": [\"a b\", \"x/y\"], \"empty\": \"\"})\n"
      "parsed = Url.parse_query(query)\n"
      "nested = Url.parse_query(\"?map[a]=1&map[b][x]=2&c[]=1&c[]=2\")\n"
      "built = Url.build({\"scheme\": \"http\", \"host\": \"example.com\", "
      "\"path\": \"api\", \"query_map\": {\"q\": [\"a b\"]}})\n"
      "if encoded == \"a%20b%2F%21\" and decoded == \"a b/!\" and "
      "query == \"q[]=a+b&q[]=x%2Fy&empty=\" and parsed[\"q\"][1] == \"x/y\" "
      "and nested[\"map\"][\"a\"] == \"1\" and "
      "nested[\"map\"][\"b\"][\"x\"] == \"2\" and "
      "nested[\"c\"][0] == \"1\" and nested[\"c\"][1] == \"2\" and "
      "built == \"http://example.com/api?q[]=a+b\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "URL percent and query helpers");
}

void test_faults() {
  expect_fault("Url.parse(\"http://example.com:bad\")\n", "UrlParseError",
               "invalid URL port");
  expect_fault("Url.percent_decode(\"%zz\")\n", "UrlDecodeError",
               "invalid percent escape");
  expect_fault("Url.parse(\"http://example.com/?q=%zz\")\n", "UrlDecodeError",
               "invalid query escape");
  expect_fault("Url.build({\"scheme\": \"1bad\", \"path\": \"/\"})\n",
               "UrlBuildError", "invalid build scheme");
}

} // namespace

int main() {
  test_parse_and_build_roundtrip();
  test_percent_and_query_helpers();
  test_faults();

  std::cout << "stdlib_url_tests: ok\n";
  return 0;
}
