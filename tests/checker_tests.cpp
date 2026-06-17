#include "frontend/binder/binder.h"
#include "frontend/checker/checker.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

struct ParsedModule {
  std::vector<std::unique_ptr<amber::ast::Expr>> items;
  std::string module_name;
  amber::binder::BindGraph graph;
};

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "checker test failed: " << message << "\n";
    std::exit(1);
  }
}

ParsedModule parse_and_bind_ok(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<test>");
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

  ParsedModule parsed;
  parsed.module_name = parse_result.module_name;
  parsed.items = std::move(parse_result.items);
  parsed.graph = std::move(bind_result.graph);
  return parsed;
}

amber::checker::CheckResult check_source(const std::string &source) {
  ParsedModule parsed = parse_and_bind_ok(source);
  return amber::checker::check_module(parsed.items, parsed.module_name,
                                      parsed.graph);
}

bool has_diagnostic(const amber::checker::CheckResult &result,
                    const std::string &code) {
  for (const amber::lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

void test_type_term_parser() {
  amber::lexer::Span span;
  span.file = "<type>";
  amber::checker::TypeParseResult parsed =
      amber::checker::parse_type_term("Map[Str, Int?]", span);
  expect(parsed.ok(), "generic optional TypeTerm parses");
  expect(amber::checker::type_term_to_string(parsed.term) == "Map[Str, Int?]",
         "generic optional TypeTerm canonical form");

  parsed = amber::checker::parse_type_term("{id: Int, **Never}", span);
  expect(parsed.ok(), "exact record TypeTerm parses");
  expect(amber::checker::type_term_to_string(parsed.term) ==
             "{id: Int, **Never}",
         "exact record TypeTerm canonical form");
}

void test_exported_boundary_success() {
  amber::checker::CheckResult result =
      check_source("package typed.demo\n"
                   "export truthy\n"
                   "def truthy(flag as Bool) -> Bool:\n"
                   "  flag and true\n");
  expect(result.ok(), "exported typed boundary succeeds");
  expect(result.boundaries.size() == 1, "one exported boundary recorded");
  const amber::checker::CallableBoundary &boundary = result.boundaries[0];
  expect(boundary.exported, "boundary is exported");
  expect(boundary.return_type == "Bool", "return type normalized");
  expect(boundary.observed_return_type == "False | True",
         "and/or flow type is observed");
  expect(boundary.type_hooks.size() == 2, "parameter and return hooks");
}

void test_missing_export_annotations() {
  amber::checker::CheckResult result = check_source("export f\n"
                                                    "def f(x):\n"
                                                    "  x\n");
  expect(!result.ok(), "missing exported annotations fail typed check");
  expect(has_diagnostic(result, "T0001"), "missing parameter annotation");
  expect(has_diagnostic(result, "T0002"), "missing return annotation");
}

void test_boundary_mismatches() {
  amber::checker::CheckResult result =
      check_source("def f(x as Int = \"bad\") -> Int:\n"
                   "  x\n");
  expect(!result.ok(), "default mismatch fails typed check");
  expect(has_diagnostic(result, "T0004"), "default mismatch diagnostic");

  result = check_source("export f\n"
                        "def f() -> Int:\n"
                        "  \"bad\"\n");
  expect(!result.ok(), "return mismatch fails typed check");
  expect(has_diagnostic(result, "T0005"), "return mismatch diagnostic");
}

void test_case_bang_exhaustiveness() {
  amber::checker::CheckResult result =
      check_source("def classify(flag as Bool) -> Bool:\n"
                   "  case! flag:\n"
                   "    when true: true\n"
                   "    when false: false\n");
  expect(result.ok(), "exhaustive bool case! succeeds");

  result = check_source("def classify(flag as Bool) -> Bool:\n"
                        "  case! flag:\n"
                        "    when true: true\n");
  expect(!result.ok(), "non-exhaustive bool case! fails");
  expect(has_diagnostic(result, "T0006"), "non-exhaustive case diagnostic");
}

void test_effect_rows_and_call_validation() {
  amber::checker::CheckResult result =
      check_source("def clocky() -> Int !{time}:\n"
                   "  clock.now()\n"
                   "def caller() -> Int !{time}:\n"
                   "  clocky()\n");
  expect(result.ok(), "matching effect rows should pass");
  expect(result.effect_summaries.size() == 2, "effect summaries recorded");
  expect(result.effect_summaries[0].declared_effects.size() == 1,
         "declared effects canonicalized");

  result = check_source("def bad() -> Int !{}:\n"
                        "  clock.now()\n");
  expect(!result.ok(), "pure effect row should reject clock access");
  expect(has_diagnostic(result, "FX0003"), "effect mismatch diagnostic");

  result = check_source("def mutate(x as Int) -> Int !{}:\n"
                        "  y = x\n");
  expect(!result.ok(), "pure effect row should reject mutation");
  expect(has_diagnostic(result, "FX0003"), "mutation effect diagnostic");

  result = check_source("def token() -> Int !{random}:\n"
                        "  SecureRandom.bytes(1).count()\n");
  expect(result.ok(), "SecureRandom calls should be covered by random effect");

  result = check_source("def token() -> Int !{}:\n"
                        "  SecureRandom.bytes(1).count()\n");
  expect(!result.ok(), "pure effect row should reject SecureRandom access");
  expect(has_diagnostic(result, "FX0003"), "SecureRandom effect diagnostic");
}

} // namespace

int main() {
  test_type_term_parser();
  test_exported_boundary_success();
  test_missing_export_annotations();
  test_boundary_mismatches();
  test_case_bang_exhaustiveness();
  test_effect_rows_and_call_validation();
  return 0;
}
