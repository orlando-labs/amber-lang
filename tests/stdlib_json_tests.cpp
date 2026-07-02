#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "profile/capabilities.h"
#include "runtime/vm.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib json test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<stdlib-json-source-test>");
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

amber::runtime::ExecutionResult execute_source_or_die(
    const std::string &source) {
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
  const amber::runtime::ExecutionResult result = execute_source_or_die(source);
  expect(!result.ok() && result.fault.has_value(), message + " should fault");
  expect(result.fault->error_name == error_name,
         message + " should fault with " + error_name + ", got " +
             (result.fault.has_value() ? result.fault->error_name : ""));
}

amber::runtime::RuntimeIoProviderStatus provider_ok() {
  amber::runtime::RuntimeIoProviderStatus status;
  status.handled = true;
  status.ok = true;
  return status;
}

class TestIoProvider final : public amber::runtime::RuntimeIoProvider {
public:
  std::unordered_map<std::string, std::string> files;

  amber::runtime::RuntimeIoProviderStatus
  fs_read_bytes(const std::string &path,
                std::optional<std::size_t> limit) override {
    auto found = files.find(path);
    if (found == files.end()) {
      amber::runtime::RuntimeIoProviderStatus status = provider_ok();
      status.ok = false;
      status.error_name = "FileNotFoundError";
      status.message = "provider file not found";
      return status;
    }
    if (limit.has_value() && found->second.size() > *limit) {
      amber::runtime::RuntimeIoProviderStatus status = provider_ok();
      status.ok = false;
      status.error_name = "ArgumentError";
      status.message = "read_bytes limit exceeded";
      return status;
    }
    amber::runtime::RuntimeIoProviderStatus status = provider_ok();
    status.bytes = found->second;
    status.count = status.bytes.size();
    return status;
  }

  amber::runtime::RuntimeIoProviderStatus
  fs_write_bytes(const std::string &path, const std::string &bytes,
                 bool create, bool truncate, bool append = false) override {
    (void)create;
    amber::runtime::RuntimeIoProviderStatus status = provider_ok();
    if (append && !truncate) {
      files[path] += bytes;
    } else {
      files[path] = bytes;
    }
    status.count = bytes.size();
    return status;
  }
};

void test_parse_strict_map_and_to_json() {
  const amber::runtime::ExecutionResult result = execute_source_or_die(
      "s = Json.parse(\"{\\\"name\\\":1,\\\"other\\\":2}\", map: StrictMap)\n"
      "plain = Json.parse(\"{\\\"name\\\":1}\")\n"
      "encoded = [1, \"x\"].to_json\n"
      "payload = {id: 1, name: \"Ada\", password: \"no\", token: \"secret\"}\n"
      "only_json = payload.to_json(only: [\"id\", :name])\n"
      "except_json = payload.to_json(except: :password)\n"
      "both_json = payload.to_json(only: [:id, :name, :token], "
      "except: \"token\")\n"
      "strict_payload = StrictMap{name: 1, \"name\": 2, keep: 3}\n"
      "strict_json = strict_payload.to_json(only: [\"name\", :keep])\n"
      "if StrictMap === s and Map === s and "
      "s.has_key?(:name) == false and s[\"name\"] == 1 and "
      "plain[:name] == 1 and encoded == \"[1,\\\"x\\\"]\" and "
      "only_json == \"{\\\"id\\\":1,\\\"name\\\":\\\"Ada\\\"}\" and "
      "except_json == \"{\\\"id\\\":1,\\\"name\\\":\\\"Ada\\\","
      "\\\"token\\\":\\\"secret\\\"}\" and "
      "both_json == \"{\\\"id\\\":1,\\\"name\\\":\\\"Ada\\\"}\" and "
      "strict_json == \"{\\\"name\\\":2,\\\"keep\\\":3}\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "StrictMap parse and value to_json");
}

void test_to_json_keyword_restrictions() {
  expect_fault("[1, 2].to_json(only: [:id])\n", "TypeError",
               "to_json only/except on non-map");
  expect_fault("{id: 1}.to_json(extra: :id)\n", "TypeError",
               "to_json rejects unknown map keyword");
}

void test_stream_parse_stop() {
  const amber::runtime::ExecutionResult result = execute_source_or_die(
      "seen = []\n"
      "n = Json.stream_parse(\"[1,2,3,4]\", depth: 1) |value|:\n"
      "  seen.push!(value)\n"
      "  if value == 3:\n"
      "    Json.stop\n"
      "n * 10 + seen.count()\n");
  expect_ok_integer(result, 33, "Json.stream_parse stop");
}

void test_path_and_paths() {
  const amber::runtime::ExecutionResult result = execute_source_or_die(
      "p = Json.parse(\"{\\\"items\\\":[{\\\"id\\\":1},{\\\"id\\\":2},{\\\"id\\\":3}],\\\"weird-key\\\":9}\")\n"
      "first = Json.path(p, \"$.items[*].id\")\n"
      "all = Json.paths(p, \"$.items[*].id\")\n"
      "short_first = p.path(\"$.items[*].id\")\n"
      "short_all = p.paths(\"$.items[*].id\")\n"
      "quoted = p.path(\"$[\\\"weird-key\\\"]\")\n"
      "kept = p.paths(\"$.items[*]\", []) |el, acc|:\n"
      "  if el[:id] > 1:\n"
      "    acc.push!(el[:id])\n"
      "  null\n"
      "stopped = p.paths(\"$.items[*]\", []) |el, acc|:\n"
      "  acc.push!(el[:id])\n"
      "  if el[:id] == 2:\n"
      "    Json.stop\n"
      "  null\n"
      "payload = p.paths(\"$.items[*]\", 0) |el, acc|:\n"
      "  if el[:id] == 2:\n"
      "    Json.stop(acc + 20)\n"
      "  acc + el[:id]\n"
      "strict = StrictMap{name: 1, \"name\": 2}\n"
      "plain = {name: 7}\n"
      "if first == 1 and all.count() == 3 and all[2] == 3 and "
      "short_first == 1 and short_all[1] == 2 and quoted == 9 and "
      "kept.count() == 2 and kept[0] == 2 and stopped.count() == 2 and "
      "stopped[1] == 2 and payload == 21 and strict.path(\"$.name\") == 2 "
      "and plain.path(\"$.name\") == 7:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "Json.path/paths and shorthands");
}

void test_stop_payload_rejected_by_streaming() {
  expect_fault(
      "Json.stream_parse(\"[1]\", depth: 1) |value|:\n"
      "  Json.stop(42)\n",
      "ArgumentError", "Json.stop payload in stream_parse");
}

void test_provider_file_jsonl_and_stream_file() {
  amber::bytecode::BcModule module = compile_source_or_die(
      "Json.save_to_file(\"events.jsonl\", [{id: 1}, {id: 2}], jsonl: true)\n"
      "rows = Json.load_from_file(\"events.jsonl\", jsonl: true)\n"
      "sum = 0\n"
      "n = Json.stream_parse_file(\"events.jsonl\", jsonl: true) |row|:\n"
      "  sum = sum + row[:id]\n"
      "rows[0][:id] * 100 + rows[1][:id] * 10 + n + sum\n");
  module.capabilities.push_back(
      amber::capability::make_capability("fs.read", "events.jsonl"));
  module.capabilities.push_back(
      amber::capability::make_capability("fs.write", "events.jsonl"));

  auto provider = std::make_shared<TestIoProvider>();
  amber::runtime::RuntimeWorldOptions options;
  options.enforce_replay = true;
  options.io_provider = provider;
  options.capability_grants.push_back(
      amber::capability::make_capability("fs.read", "events.jsonl"));
  options.capability_grants.push_back(
      amber::capability::make_capability("fs.write", "events.jsonl"));

  amber::runtime::RuntimeWorld world(module, std::move(options));
  const amber::runtime::ExecutionResult result =
      world.execute(module.init.entry_code_id);
  expect_ok_integer(result, 125, "provider JSONL load/save/stream");
  expect(provider->files["events.jsonl"] == "{\"id\":1}\n{\"id\":2}\n",
         "provider should receive compact JSONL bytes");
}

} // namespace

int main() {
  test_parse_strict_map_and_to_json();
  test_to_json_keyword_restrictions();
  test_stream_parse_stop();
  test_path_and_paths();
  test_stop_payload_rejected_by_streaming();
  test_provider_file_jsonl_and_stream_file();

  std::cout << "stdlib_json_tests: ok\n";
  return 0;
}
