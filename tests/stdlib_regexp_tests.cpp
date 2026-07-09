#include "bytecode/emitter.h"
#include "frontend/ast/expr.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/macro_expander.h"
#include "runtime/vm.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib regexp test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<stdlib-regexp-source-test>");
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
  const amber::macros::ExpandResult macro_result = amber::macros::expand_macros(
      parse_result.items, parse_result.module_name, source);
  if (!macro_result.ok) {
    std::cerr << "macro expansion error: " << macro_result.error << "\n";
    std::exit(1);
  }
  amber::ast::expand_quotes(parse_result.items);
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
  expect(decoded.ok(), "compiled module should verify");
  return std::move(decoded.module);
}

amber::runtime::ExecutionResult execute_source(const std::string &source) {
  amber::bytecode::BcModule module = compile_source_or_die(source);
  amber::runtime::RuntimeWorld world(module);
  return world.execute(module.init.entry_code_id);
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
         message + " should fault with " + error_name);
}

void test_compile_match_groups_and_operators() {
  expect_ok_integer(
      execute_source(
          "pat = Regexp.compile(\"(\\\\d+)\")\n"
          "m = pat.match(\"id=123\")\n"
          "left = \"id=123\" =~ pat\n"
          "right = pat =~ \"id=123\"\n"
          "if m and left and right and m[0] == \"123\" and m[1] == \"123\" "
          "and m.count() == 1 and m.captures()[0] == \"123\" and "
          "m.start == 3 and m.finish == 6 and pat.match?(\"x9\") and "
          "pat.full_match?(\"123\") and (\"abc\" !~ pat):\n"
          "  42\n"
          "else:\n"
          "  0\n"),
      42, "compile, match, captures, and operators");
}

void test_regexp_tag_and_escape() {
  expect_ok_integer(
      execute_source("pat = r\"\\d+\"\n"
                     "same = Regexp.new(\"\\\\d+\")\n"
                     "escaped = Regexp.escape(\"a+b[c]\")\n"
                     "if pat == same and pat.source == \"\\\\d+\" and "
                     "pat.to_str == \"/\\\\d+/\" and pat.match?(\"x42\") and "
                     "escaped == \"a\\\\+b\\\\[c\\\\]\":\n"
                     "  42\n"
                     "else:\n"
                     "  0\n"),
      42, "regexp string tag, Regexp.new, source, display, and escape");
}

void test_replacement_variants() {
  expect_ok_integer(
      execute_source("pat = Regexp.compile(\"(\\\\d+)\")\n"
                     "plain = \"a12b34\".replace(pat, \"[$1]\")\n"
                     "limited = \"1 2 3\".replace(pat, \"x\", count: 2)\n"
                     "blocky = \"a12b3\".replace(pat) |m|:\n"
                     "  \"<\" + m[1] + \">\"\n"
                     "if plain == \"a[12]b[34]\" and limited == \"x x 3\" and "
                     "blocky == \"a<12>b<3>\":\n"
                     "  42\n"
                     "else:\n"
                     "  0\n"),
      42, "regexp replacement overloads");
}

void test_faults() {
  expect_fault("Regexp.compile(\"[\")\n", "RegexpCompileError",
               "invalid regexp");
  expect_fault("Regexp.compile(\"x\", flags: \"m\")\n", "ArgumentError",
               "unsupported flags");
}

} // namespace

int main() {
  test_compile_match_groups_and_operators();
  test_regexp_tag_and_escape();
  test_replacement_variants();
  test_faults();
  std::cout << "stdlib_regexp_tests: ok\n";
  return 0;
}
