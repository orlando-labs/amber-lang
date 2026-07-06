#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/vm.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib benchmark test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<stdlib-benchmark-source-test>");
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

void test_measure_chain_exports() {
  const amber::runtime::ExecutionResult result = execute_source(
      "r = Benchmark.measure(\"calc\"):\n"
      "  21 * 2\n"
      "m = r.map\n"
      "m2 = r.to_map\n"
      "json = r.to_json(pretty: true)\n"
      "copy = Benchmark.from_json(json)\n"
      "text = r.pretty(unit: :ns)\n"
      "if m[\"schema\"] == \"amber.benchmark.v1\" and "
      "m[\"kind\"] == \"measurement\" and m2[\"kind\"] == \"measurement\" and "
      "m.has_key?(\"value\") == false and r[\"value\"] == 42 and "
      "copy[\"data\"][\"elapsed_ns\"] >= 0 and "
      "json.contains?(\"amber.benchmark.v1\") and text.contains?(\"calc\"):\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "measure chain exports");
}

void test_run_compare_and_ansi_table() {
  const amber::runtime::ExecutionResult result = execute_source(
      "a = Benchmark.run(\"a\", iterations: 2, samples: 2) |i|:\n"
      "  i + 1\n"
      "b = Benchmark.run(\"b\", iterations: 2, samples: 2) |i|:\n"
      "  i + 2\n"
      "cmp = Benchmark.compare(a, b)\n"
      "table = cmp.table(style: :ansi, highlight: :best)\n"
      "plain = cmp.table\n"
      "round = Benchmark.from_map(cmp.map)\n"
      "if a.map[\"kind\"] == \"report\" and a.to_json.contains?(\"sample_ns\") "
      "and cmp.map[\"kind\"] == \"compare_report\" and "
      "round[\"data\"][\"cases\"].count() == 2 and "
      "table.contains?(\"[1m\") and plain.contains?(\"relative\"):\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "run compare and ANSI table");
}

void test_profile_sections() {
  const amber::runtime::ExecutionResult result = execute_source(
      "profile = Benchmark.profile(\"checkout\") |p|:\n"
      "  loaded = p.section(\"load\"):\n"
      "    2\n"
      "  p.section(\"persist\"):\n"
      "    loaded + 40\n"
      "m = profile.map\n"
      "json = profile.to_json\n"
      "text = profile.pretty(unit: :ns)\n"
      "if profile[\"value\"] == 42 and m[\"kind\"] == \"profile\" and "
      "m[\"data\"][\"spans\"].count() == 2 and "
      "m[\"data\"][\"summary\"].count() == 2 and "
      "m[\"data\"][\"spans\"][0][\"label\"] == \"load\" and "
      "json.contains?(\"persist\") and text.contains?(\"section\"):\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "profile sections");
}

void test_result_accessors() {
  const amber::runtime::ExecutionResult result = execute_source(
      "measurement = Benchmark.measure(\"calc\"):\n"
      "  21 * 2\n"
      "report = Benchmark.run(\"loop\", iterations: 2, samples: 2) |i|:\n"
      "  i + 1\n"
      "cmp = Benchmark.compare(report)\n"
      "profile = Benchmark.profile(\"checkout\") |p|:\n"
      "  p.section(\"load\"):\n"
      "    7\n"
      "found = profile.find(\"load\")\n"
      "tree = profile.pretty(layout: :tree, unit: :ns)\n"
      "if measurement.label == \"calc\" and measurement.value == 42 and "
      "measurement.elapsed_ns >= 0 and "
      "measurement.elapsed.total_nanoseconds >= 0 and "
      "measurement.iterations == 1 and "
      "measurement.to_str.contains?(\"calc\") and "
      "report.iterations == 4 and report.samples == 2 and "
      "report.sample_ns.count() == 2 and report.sample_times.count() == 2 and "
      "report.mean.total_nanoseconds >= 0 and report.p95_ns >= 0 and "
      "cmp.cases.count() == 1 and cmp.fastest[\"kind\"] == \"report\" and "
      "cmp.slowest[\"kind\"] == \"report\" and cmp.relative.count() == 1 and "
      "profile.value == 7 and profile.total_ns >= 0 and "
      "profile.total.total_nanoseconds >= 0 and profile.spans.count() == 1 and "
      "profile.summary.count() == 1 and found[\"label\"] == \"load\" and "
      "tree.contains?(\"load\"):\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "result accessors");
}

void test_faults() {
  expect_fault("Benchmark.run(iterations: 0) |i|:\n  i\n", "ArgumentError",
               "zero iterations without min_time");
  expect_fault("Benchmark.from_json(\"{}\")\n", "BenchmarkImportError",
               "invalid import");
  expect_fault(
      "Benchmark.from_map({\"schema\": \"amber.benchmark.v1\", "
      "\"kind\": \"report\", \"label\": null, \"data\": {}})\n",
      "BenchmarkImportError", "invalid report import");
  expect_fault("r = Benchmark.measure(\"x\"):\n  1\nr.pretty(sort: :bogus)\n",
               "ArgumentError", "invalid formatter sort");
}

} // namespace

int main() {
  test_measure_chain_exports();
  test_run_compare_and_ansi_table();
  test_profile_sections();
  test_result_accessors();
  test_faults();

  std::cout << "stdlib_benchmark_tests: ok\n";
  return 0;
}
