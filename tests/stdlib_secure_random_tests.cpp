#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "profile/capabilities.h"
#include "runtime/vm.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib secure random test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<stdlib-secure-random-source-test>");
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

struct SourceRun {
  amber::bytecode::BcModule module;
  amber::runtime::ExecutionResult result;
};

SourceRun execute_source(const std::string &source) {
  amber::bytecode::BcModule module = compile_source_or_die(source);
  expect(module.init.has_entry_code_id, "source module should have init code");
  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, module.init.entry_code_id);
  return {std::move(module), std::move(result)};
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

std::string string_result_text(const SourceRun &run) {
  expect(run.result.ok(), "string result should succeed");
  expect(run.result.value.is_string(), "result should be Str");
  const std::uint32_t id = run.result.value.as_string().string_id;
  const std::vector<std::string> &strings =
      run.result.runtime_strings.empty() ? run.module.strings
                                         : run.result.runtime_strings;
  expect(id < strings.size(), "string id should be in range");
  return strings[id];
}

bool all_hex(const std::string &text) {
  for (char c : text) {
    const bool digit = c >= '0' && c <= '9';
    const bool lower_hex = c >= 'a' && c <= 'f';
    if (!digit && !lower_hex) {
      return false;
    }
  }
  return true;
}

bool all_base64url_unpadded(const std::string &text) {
  for (char c : text) {
    const bool upper = c >= 'A' && c <= 'Z';
    const bool lower = c >= 'a' && c <= 'z';
    const bool digit = c >= '0' && c <= '9';
    if (!upper && !lower && !digit && c != '-' && c != '_') {
      return false;
    }
  }
  return text.find('=') == std::string::npos;
}

bool uuid_v4_shape(const std::string &uuid) {
  if (uuid.size() != 36U) {
    return false;
  }
  if (uuid[8] != '-' || uuid[13] != '-' || uuid[18] != '-' ||
      uuid[23] != '-') {
    return false;
  }
  if (uuid[14] != '4') {
    return false;
  }
  const char variant = uuid[19];
  if (variant != '8' && variant != '9' && variant != 'a' && variant != 'b') {
    return false;
  }
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    if (uuid[i] == '-') {
      continue;
    }
    const bool digit = uuid[i] >= '0' && uuid[i] <= '9';
    const bool lower_hex = uuid[i] >= 'a' && uuid[i] <= 'f';
    if (!digit && !lower_hex) {
      return false;
    }
  }
  return true;
}

void expect_fault(const std::string &source, const std::string &error_name,
                  const std::string &message) {
  const SourceRun run = execute_source(source);
  expect(!run.result.ok() && run.result.fault.has_value(),
         message + " should fault");
  expect(run.result.fault->error_name == error_name,
         message + " should fault with " + error_name + ", got " +
             (run.result.fault.has_value() ? run.result.fault->error_name
                                           : ""));
}

void test_bytes_and_formats() {
  expect_ok_integer(execute_source("SecureRandom.bytes(16).count()\n").result,
                    16, "bytes length");

  const std::string hex = string_result_text(execute_source(
      "SecureRandom.hex(8)\n"));
  expect(hex.size() == 16U && all_hex(hex), "hex token shape");

  const std::string b64 = string_result_text(execute_source(
      "SecureRandom.base64(3)\n"));
  expect(b64.size() == 4U, "base64 3 bytes yields 4 chars");

  const std::string b64_unpadded = string_result_text(execute_source(
      "SecureRandom.base64(1, padding: false)\n"));
  expect(b64_unpadded.size() == 2U &&
             b64_unpadded.find('=') == std::string::npos,
         "base64 unpadded shape");

  const std::string url = string_result_text(execute_source(
      "SecureRandom.base64url(32)\n"));
  expect(all_base64url_unpadded(url), "base64url default is url-safe");
}

void test_uuid_and_int_range() {
  const std::string uuid = string_result_text(execute_source(
      "SecureRandom.uuid\n"));
  expect(uuid_v4_shape(uuid), "uuid v4 shape");

  const SourceRun ranged = execute_source(
      "x = SecureRandom.int(3..7)\n"
      "if x >= 3 and x <= 7:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(ranged.result, 42, "inclusive range sample");

  const SourceRun stepped = execute_source(
      "x = SecureRandom.int(Range.new(10, 2, step: -2))\n"
      "if x == 10 or x == 8 or x == 6 or x == 4 or x == 2:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(stepped.result, 42, "stepped descending range sample");

  expect_ok_integer(
      execute_source(
          "SecureRandom.int(Range.new(1, 2, inclusive_end: false))\n")
          .result,
      1, "single-value exclusive range");
}

void test_faults_and_policy() {
  expect_fault("SecureRandom.bytes(-1)\n", "ArgumentError",
               "negative byte count");
  expect_fault("SecureRandom.int(1)\n", "TypeError", "non-range int arg");
  expect_fault("SecureRandom.int(Range.new(1, 1, inclusive_end: false))\n",
               "ArgumentError", "empty range");

  amber::bytecode::BcModule module =
      compile_source_or_die("SecureRandom.bytes(1).count()\n");
  module.capabilities.push_back(
      amber::capability::make_capability("random.secure"));
  expect(module.init.has_entry_code_id, "policy module should have init");

  amber::runtime::RuntimeWorldOptions denied_options;
  amber::runtime::RuntimeWorld denied_world(module, denied_options);
  amber::runtime::ExecutionResult denied =
      denied_world.execute(module.init.entry_code_id);
  expect(!denied.ok() && denied.fault.has_value() &&
             denied.fault->error_name == "CapabilityError",
         "missing random.secure grant should fail");

  amber::runtime::RuntimeWorldOptions allowed_options;
  allowed_options.capability_grants.push_back(
      amber::capability::make_capability("random.secure"));
  amber::runtime::RuntimeWorld allowed_world(module, allowed_options);
  amber::runtime::ExecutionResult allowed =
      allowed_world.execute(module.init.entry_code_id);
  expect_ok_integer(allowed, 1, "granted random.secure");

  amber::runtime::RuntimeWorldOptions replay_options = allowed_options;
  replay_options.enforce_replay = true;
  amber::runtime::RuntimeWorld replay_world(module, replay_options);
  amber::runtime::ExecutionResult replay =
      replay_world.execute(module.init.entry_code_id);
  expect(!replay.ok() && replay.fault.has_value() &&
             replay.fault->error_name == "DeterminismError",
         "replay mode without random provider should fail");
}

} // namespace

int main() {
  test_bytes_and_formats();
  test_uuid_and_int_range();
  test_faults_and_policy();

  std::cout << "stdlib_secure_random_tests: ok\n";
  return 0;
}
