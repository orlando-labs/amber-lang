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
    std::cerr << "stdlib yaml test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<stdlib-yaml-source-test>");
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

void expect_ok_integer(const std::string &source, std::int64_t expected,
                       const std::string &message) {
  expect_ok_integer(execute_source_or_die(source), expected, message);
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
  fs_write_bytes(const std::string &path, const std::string &bytes, bool create,
                 bool truncate, bool append = false) override {
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

void test_parse_block_structure() {
  expect_ok_integer(
      "cfg = Yaml.parse(\"name: Ada\\nage: 42\\nactive: true\\n"
      "scores:\\n  - 10\\n  - 20\\ntags: [a, b]\\nmeta:\\n  role: admin\\n\")\n"
      "if cfg[:name] == \"Ada\" and cfg[:age] == 42 and cfg[:active] == true "
      "and cfg[:scores][0] == 10 and cfg[:scores][1] == 20 and "
      "cfg[\"tags\"][1] == \"b\" and cfg[:meta][:role] == \"admin\":\n"
      "  42\n"
      "else:\n"
      "  0\n",
      42, "parse block mapping/sequence/nested");
}

void test_scalar_typing() {
  expect_ok_integer(
      "d = Yaml.parse(\"n: 7\\nf: 1.5\\ns: \\\"7\\\"\\nb: false\\n"
      "z: null\\ne: ~\\nq: 'a: b'\\n\")\n"
      "if d[:n] == 7 and d[:n] + 1 == 8 and d[:f] == 1.5 and d[:s] == \"7\" "
      "and d[:b] == false and d[:z] == null and d[:e] == null and "
      "d[:q] == \"a: b\":\n"
      "  42\n"
      "else:\n"
      "  0\n",
      42, "core-schema scalar typing");
}

void test_flow_collections() {
  expect_ok_integer(
      "d = Yaml.parse(\"matrix: [[1, 2], [3, 4]]\\n"
      "who: {name: Ada, age: 42}\\nempty_map: {}\\nempty_list: []\\n\")\n"
      "if d[:matrix][0][1] == 2 and d[:matrix][1][0] == 3 and "
      "d[:who][:name] == \"Ada\" and d[:who][:age] == 42 and "
      "d[:empty_map].count() == 0 and d[:empty_list].count() == 0:\n"
      "  42\n"
      "else:\n"
      "  0\n",
      42, "flow sequences and mappings");
}

void test_sequence_of_mappings() {
  expect_ok_integer(
      "d = Yaml.parse(\"users:\\n  - name: Ada\\n    id: 1\\n"
      "  - name: Bob\\n    id: 2\\n\")\n"
      "if d[:users][0][:name] == \"Ada\" and d[:users][0][:id] == 1 and "
      "d[:users][1][:name] == \"Bob\" and d[:users][1][:id] == 2:\n"
      "  42\n"
      "else:\n"
      "  0\n",
      42, "block sequence of inline mappings");
}

void test_generate_exact_and_roundtrip() {
  expect_ok_integer(
      "text = Yaml.generate({a: 1, b: [1, 2], c: {d: \"x\"}})\n"
      "back = Yaml.parse(text)\n"
      "if text == \"a: 1\\nb:\\n  - 1\\n  - 2\\nc:\\n  d: x\\n\" and "
      "back[:a] == 1 and back[:b][1] == 2 and back[:c][:d] == \"x\":\n"
      "  42\n"
      "else:\n"
      "  0\n",
      42, "generate block style + round-trip");
}

void test_generate_quotes_ambiguous_scalars() {
  expect_ok_integer(
      "text = Yaml.generate({a: \"7\", b: \"true\", c: \"null\", d: \"\", "
      "e: \"plain\"})\n"
      "round = Yaml.parse(text)\n"
      "if round[:a] == \"7\" and round[:b] == \"true\" and "
      "round[:c] == \"null\" and round[:d] == \"\" and round[:e] == \"plain\":\n"
      "  42\n"
      "else:\n"
      "  0\n",
      42, "generator quotes scalars that would re-type");
}

void test_parse_faults() {
  expect_fault("Yaml.parse(\"a: [1, 2\")\n", "YamlParseError",
               "unterminated flow sequence");
  expect_fault("Yaml.parse(\"a:\\n\\tb: 1\\n\")\n", "YamlParseError",
               "tab indentation rejected");
  expect_fault("Yaml.parse(42)\n", "TypeError", "non-string input");
}

void test_provider_file_roundtrip() {
  amber::bytecode::BcModule module = compile_source_or_die(
      "Yaml.save_to_file(\"cfg.yaml\", {port: 8080, hosts: [\"a\", \"b\"]})\n"
      "cfg = Yaml.load_from_file(\"cfg.yaml\")\n"
      "cfg[:port] * 10 + cfg[:hosts].count()\n");
  module.capabilities.push_back(
      amber::capability::make_capability("fs.read", "cfg.yaml"));
  module.capabilities.push_back(
      amber::capability::make_capability("fs.write", "cfg.yaml"));

  auto provider = std::make_shared<TestIoProvider>();
  amber::runtime::RuntimeWorldOptions options;
  options.enforce_replay = true;
  options.io_provider = provider;
  options.capability_grants.push_back(
      amber::capability::make_capability("fs.read", "cfg.yaml"));
  options.capability_grants.push_back(
      amber::capability::make_capability("fs.write", "cfg.yaml"));

  amber::runtime::RuntimeWorld world(module, std::move(options));
  const amber::runtime::ExecutionResult result =
      world.execute(module.init.entry_code_id);
  expect_ok_integer(result, 80802, "provider YAML save/load round-trip");
  expect(provider->files["cfg.yaml"] ==
             "port: 8080\nhosts:\n  - a\n  - b\n",
         "provider should receive block-style YAML bytes");
}

} // namespace

int main() {
  test_parse_block_structure();
  test_scalar_typing();
  test_flow_collections();
  test_sequence_of_mappings();
  test_generate_exact_and_roundtrip();
  test_generate_quotes_ambiguous_scalars();
  test_parse_faults();
  test_provider_file_roundtrip();

  std::cout << "stdlib_yaml_tests: ok\n";
  return 0;
}
