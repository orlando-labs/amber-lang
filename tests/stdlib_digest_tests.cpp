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
    std::cerr << "stdlib digest test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<stdlib-digest-source-test>");
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

void test_standard_vectors() {
  const amber::runtime::ExecutionResult result = execute_source(
      "data = Bytes.new(\"abc\")\n"
      "key = Hex.decode(\"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b\")\n"
      "if Digest.crc32(data).hex() == \"352441c2\" and "
      "Digest.md5(data).hex() == \"900150983cd24fb0d6963f7d28e17f72\" and "
      "Digest.sha1(data).hex() == "
      "\"a9993e364706816aba3e25717850c26c9cd0d89d\" and "
      "Digest.sha256(data).hex() == "
      "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\" "
      "and Digest.hmac_sha256(key, Bytes.new(\"Hi There\")).hex() == "
      "\"b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "CRC/MD5/SHA/HMAC vectors");
}

void test_streebog_vectors_and_aliases() {
  const amber::runtime::ExecutionResult result = execute_source(
      "data = Bytes.new(\"abc\")\n"
      "s256 = "
      "\"4e2919cf137ed41ec4fb6270c61826cc4fffb660341e0af3688cd0626d23b481\"\n"
      "s512 = "
      "\"28156e28317da7c98f4fe2bed6b542d0dab85bb224445fcedaf75d46e26d7eb8d5997f"
      "3e0915dd6b7f0aab08d9c8beb0d8c64bae2ab8b3c8c6bc53b3bf0db728\"\n"
      "if Digest.streebog256(data).hex() == s256 and "
      "Digest.gost256(data).hex() == s256 and "
      "Digest.гост256(data).hex() == s256 and "
      "Digest.стрибог256(data).hex() == s256 and "
      "Digest.streebog512(data).hex() == s512 and "
      "Digest.gost512(data).hex() == s512 and "
      "Digest.гост512(data).hex() == s512 and "
      "Digest.стрибог512(data).hex() == s512:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "Streebog vectors and aliases");
}

void test_binary_inputs_and_faults() {
  const amber::runtime::ExecutionResult result = execute_source(
      "buf = io.ByteBuffer(3)\n"
      "buf.put!(97)\n"
      "buf.put!(98)\n"
      "buf.put!(99)\n"
      "buf.flip!()\n"
      "full = Digest.sha256(buf).hex()\n"
      "part = Digest.sha1(buf.slice(1, 2)).hex()\n"
      "if full == "
      "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\" "
      "and part == \"5b2505039ac5af9e197f5dad04113906a9cf9a2a\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "ByteBuffer and ByteSlice inputs");

  expect_fault("Digest.sha256(\"abc\")\n", "TypeError",
               "digest rejects implicit Str bytes");
  expect_fault("Digest.hmac_sha256(Bytes.new(\"key\"), \"abc\")\n", "TypeError",
               "HMAC rejects implicit Str bytes");
}

} // namespace

int main() {
  test_standard_vectors();
  test_streebog_vectors_and_aliases();
  test_binary_inputs_and_faults();

  std::cout << "stdlib_digest_tests: ok\n";
  return 0;
}
