#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/ast/expr.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct TestCase {
  std::string directory;
  std::string phase;
  std::string source;
  std::string expect;
};

std::string read_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open file: " + path);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool is_directory(const std::string &path) {
  struct stat info{};
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool is_regular_file(const std::string &path) {
  struct stat info{};
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

std::string join_path(const std::string &left, const std::string &right) {
  if (left.empty() || left == ".") {
    return right;
  }
  if (!left.empty() && left[left.size() - 1] == '/') {
    return left + right;
  }
  return left + "/" + right;
}

std::string find_json_string(const std::string &json, const std::string &key) {
  const std::string quoted_key = "\"" + key + "\"";
  const std::size_t key_pos = json.find(quoted_key);
  if (key_pos == std::string::npos) {
    return "";
  }
  std::size_t cursor = json.find(':', key_pos + quoted_key.size());
  if (cursor == std::string::npos) {
    return "";
  }
  ++cursor;
  while (cursor < json.size() &&
         (json[cursor] == ' ' || json[cursor] == '\n' || json[cursor] == '\r' ||
          json[cursor] == '\t')) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != '"') {
    return "";
  }
  ++cursor;

  std::string value;
  while (cursor < json.size()) {
    const char c = json[cursor++];
    if (c == '"') {
      return value;
    }
    if (c == '\\' && cursor < json.size()) {
      const char escaped = json[cursor++];
      switch (escaped) {
      case '"':
      case '\\':
      case '/':
        value.push_back(escaped);
        break;
      case 'n':
        value.push_back('\n');
        break;
      case 'r':
        value.push_back('\r');
        break;
      case 't':
        value.push_back('\t');
        break;
      default:
        value.push_back(escaped);
        break;
      }
      continue;
    }
    value.push_back(c);
  }
  return "";
}

void discover_cases(const std::string &root, std::vector<TestCase> *cases) {
  const std::string meta_path = join_path(root, "meta.json");
  if (is_regular_file(meta_path)) {
    const std::string meta = read_file(meta_path);
    TestCase test_case;
    test_case.directory = root;
    test_case.phase = find_json_string(meta, "phase");
    test_case.source = find_json_string(meta, "source");
    test_case.expect = find_json_string(meta, "expect");
    cases->push_back(test_case);
  }

  DIR *dir = opendir(root.c_str());
  if (dir == nullptr) {
    return;
  }
  while (dirent *entry = readdir(dir)) {
    const char *name = entry->d_name;
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
      continue;
    }
    const std::string child = join_path(root, name);
    if (is_directory(child)) {
      discover_cases(child, cases);
    }
  }
  closedir(dir);
}

bool run_lex_case(const TestCase &test_case) {
  const std::string source_path =
      join_path(test_case.directory, test_case.source);
  const std::string expect_path =
      join_path(test_case.directory, test_case.expect);
  const std::string source = read_file(source_path);

  amber::lexer::Lexer lexer(source, source_path);
  amber::lexer::LexResult result = lexer.lex();
  if (!result.ok()) {
    std::cerr << test_case.directory << ": lexer diagnostics:\n"
              << amber::lexer::diagnostics_to_json(result.diagnostics);
    return false;
  }

  const std::string actual = amber::lexer::tokens_to_json(
      result.tokens, amber::lexer::sha256_hex(source));
  const std::string expected = read_file(expect_path);
  if (actual != expected) {
    std::cerr << test_case.directory << ": token dump mismatch\n";
    return false;
  }
  return true;
}

bool run_parse_expr_case(const TestCase &test_case) {
  const std::string source_path =
      join_path(test_case.directory, test_case.source);
  const std::string expect_path =
      join_path(test_case.directory, test_case.expect);
  const std::string source = read_file(source_path);

  amber::lexer::Lexer lexer(source, source_path);
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << test_case.directory << ": lexer diagnostics:\n"
              << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    return false;
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseResult parse_result = parser.parse_expression_unit();
  if (!parse_result.ok()) {
    std::cerr << test_case.directory << ": parser diagnostics:\n"
              << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    return false;
  }

  const std::string actual = amber::ast::ast_module_to_json(
      *parse_result.expr, amber::lexer::sha256_hex(source));
  const std::string expected = read_file(expect_path);
  if (actual != expected) {
    std::cerr << test_case.directory << ": AST dump mismatch\n";
    return false;
  }
  return true;
}

bool run_parse_case(const TestCase &test_case) {
  const std::string source_path =
      join_path(test_case.directory, test_case.source);
  const std::string expect_path =
      join_path(test_case.directory, test_case.expect);
  const std::string source = read_file(source_path);

  amber::lexer::Lexer lexer(source, source_path);
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << test_case.directory << ": lexer diagnostics:\n"
              << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    return false;
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << test_case.directory << ": parser diagnostics:\n"
              << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    return false;
  }

  const std::string actual = amber::ast::ast_module_to_json(
      parse_result.items, parse_result.module_name,
      amber::lexer::sha256_hex(source));
  const std::string expected = read_file(expect_path);
  if (actual != expected) {
    std::cerr << test_case.directory << ": AST dump mismatch\n";
    return false;
  }
  return true;
}

bool run_bind_case(const TestCase &test_case) {
  const std::string source_path =
      join_path(test_case.directory, test_case.source);
  const std::string expect_path =
      join_path(test_case.directory, test_case.expect);
  const std::string source = read_file(source_path);

  amber::lexer::Lexer lexer(source, source_path);
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << test_case.directory << ": lexer diagnostics:\n"
              << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    return false;
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << test_case.directory << ": parser diagnostics:\n"
              << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    return false;
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << test_case.directory << ": binder diagnostics:\n"
              << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    return false;
  }

  const std::string actual = amber::binder::bind_graph_to_json(
      bind_result.graph, parse_result.module_name,
      amber::lexer::sha256_hex(source));
  const std::string expected = read_file(expect_path);
  if (actual != expected) {
    std::cerr << test_case.directory << ": bind dump mismatch\n";
    return false;
  }
  return true;
}

bool run_hir_case(const TestCase &test_case) {
  const std::string source_path =
      join_path(test_case.directory, test_case.source);
  const std::string expect_path =
      join_path(test_case.directory, test_case.expect);
  const std::string source = read_file(source_path);

  amber::lexer::Lexer lexer(source, source_path);
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << test_case.directory << ": lexer diagnostics:\n"
              << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    return false;
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << test_case.directory << ": parser diagnostics:\n"
              << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    return false;
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << test_case.directory << ": binder diagnostics:\n"
              << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    return false;
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  const std::string actual = amber::hir::program_to_json(
      program, parse_result.module_name, amber::lexer::sha256_hex(source));
  const std::string expected = read_file(expect_path);
  if (actual != expected) {
    std::cerr << test_case.directory << ": HIR dump mismatch\n";
    return false;
  }
  return true;
}

bool run_bc_case(const TestCase &test_case) {
  const std::string source_path =
      join_path(test_case.directory, test_case.source);
  const std::string expect_path =
      join_path(test_case.directory, test_case.expect);
  const std::string source = read_file(source_path);

  amber::lexer::Lexer lexer(source, source_path);
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << test_case.directory << ": lexer diagnostics:\n"
              << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    return false;
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << test_case.directory << ": parser diagnostics:\n"
              << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    return false;
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << test_case.directory << ": binder diagnostics:\n"
              << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    return false;
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << test_case.directory << ": emitter diagnostics:\n"
              << amber::lexer::diagnostics_to_json(emit_result.diagnostics);
    return false;
  }

  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(emit_result.module);
  amber::bytecode::DecodeResult decode_result =
      amber::bytecode::deserialize_module(bytes);
  if (!decode_result.ok()) {
    std::cerr << test_case.directory << ": verifier diagnostics:\n"
              << amber::bytecode::verify_errors_to_json(decode_result.errors);
    return false;
  }

  const std::string actual = amber::bytecode::module_to_json(
      decode_result.module, decode_result.sections,
      amber::lexer::sha256_hex(std::string(bytes.begin(), bytes.end())));
  const std::string expected = read_file(expect_path);
  if (actual != expected) {
    std::cerr << test_case.directory << ": bytecode dump mismatch\n";
    return false;
  }
  return true;
}

bool run_bc_disasm_case(const TestCase &test_case) {
  const std::string source_path =
      join_path(test_case.directory, test_case.source);
  const std::string expect_path =
      join_path(test_case.directory, test_case.expect);
  const std::string source = read_file(source_path);

  amber::lexer::Lexer lexer(source, source_path);
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << test_case.directory << ": lexer diagnostics:\n"
              << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    return false;
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << test_case.directory << ": parser diagnostics:\n"
              << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    return false;
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << test_case.directory << ": binder diagnostics:\n"
              << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    return false;
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << test_case.directory << ": emitter diagnostics:\n"
              << amber::lexer::diagnostics_to_json(emit_result.diagnostics);
    return false;
  }

  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(emit_result.module);
  amber::bytecode::DecodeResult decode_result =
      amber::bytecode::deserialize_module(bytes);
  if (!decode_result.ok()) {
    std::cerr << test_case.directory << ": verifier diagnostics:\n"
              << amber::bytecode::verify_errors_to_json(decode_result.errors);
    return false;
  }

  const std::string actual = amber::bytecode::module_to_disasm(
      decode_result.module, decode_result.sections,
      amber::lexer::sha256_hex(std::string(bytes.begin(), bytes.end())));
  const std::string expected = read_file(expect_path);
  if (actual != expected) {
    std::cerr << test_case.directory << ": bytecode disasm mismatch\n";
    return false;
  }
  return true;
}

bool run_bind_diag_case(const TestCase &test_case) {
  const std::string source_path =
      join_path(test_case.directory, test_case.source);
  const std::string expect_path =
      join_path(test_case.directory, test_case.expect);
  const std::string source = read_file(source_path);

  amber::lexer::Lexer lexer(source, source_path);
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    const std::string actual =
        amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    const std::string expected = read_file(expect_path);
    if (actual != expected) {
      std::cerr << test_case.directory << ": lexer diagnostic mismatch\n";
      return false;
    }
    return true;
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    const std::string actual =
        amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    const std::string expected = read_file(expect_path);
    if (actual != expected) {
      std::cerr << test_case.directory << ": parser diagnostic mismatch\n";
      return false;
    }
    return true;
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (bind_result.diagnostics.empty()) {
    std::cerr << test_case.directory << ": expected diagnostics, got success\n";
    return false;
  }

  const std::string actual =
      amber::lexer::diagnostics_to_json(bind_result.diagnostics);
  const std::string expected = read_file(expect_path);
  if (actual != expected) {
    std::cerr << test_case.directory << ": binder diagnostic mismatch\n";
    return false;
  }
  return true;
}

void usage(std::ostream &out) { out << "usage: ambertest run <path>\n"; }

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 3 || std::string(argv[1]) != "run") {
      usage(std::cerr);
      return 2;
    }

    std::vector<TestCase> cases;
    discover_cases(argv[2], &cases);
    if (cases.empty()) {
      std::cerr << "ambertest: no meta.json cases found under " << argv[2]
                << "\n";
      return 1;
    }

    int passed = 0;
    int failed = 0;
    for (const TestCase &test_case : cases) {
      if (test_case.phase == "lex") {
        if (run_lex_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (test_case.phase == "parse-expr") {
        if (run_parse_expr_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (test_case.phase == "parse") {
        if (run_parse_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (test_case.phase == "bind") {
        if (run_bind_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (test_case.phase == "hir") {
        if (run_hir_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (test_case.phase == "bc") {
        if (run_bc_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (test_case.phase == "bc-disasm") {
        if (run_bc_disasm_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (test_case.phase == "bind-diag") {
        if (run_bind_diag_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else {
        std::cerr << test_case.directory << ": unsupported phase '"
                  << test_case.phase << "'\n";
        ++failed;
      }
    }

    std::cout << "ambertest: " << passed << " passed, " << failed
              << " failed\n";
    return failed == 0 ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << "ambertest: " << error.what() << "\n";
    return 1;
  }
}
