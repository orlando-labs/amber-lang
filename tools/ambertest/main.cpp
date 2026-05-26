#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/ast/expr.h"
#include "frontend/binder/binder.h"
#include "frontend/checker/checker.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/module_loader.h"
#include "runtime/vm.h"

#include <algorithm>
#include <cstdint>
#include <dirent.h>
#include <sys/stat.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestCase {
  std::string directory;
  std::string phase;
  std::string source;
  std::string expect;
  std::string entry;
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
    test_case.entry = find_json_string(meta, "entry");
    cases->push_back(test_case);
  }

  DIR *dir = opendir(root.c_str());
  if (dir == nullptr) {
    return;
  }
  std::vector<std::string> children;
  while (dirent *entry = readdir(dir)) {
    const char *name = entry->d_name;
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
      continue;
    }
    const std::string child = join_path(root, name);
    if (is_directory(child)) {
      children.push_back(child);
    }
  }
  closedir(dir);
  std::sort(children.begin(), children.end());
  for (const std::string &child : children) {
    discover_cases(child, cases);
  }
}

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (const char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      out << c;
      break;
    }
  }
  return out.str();
}

void print_mismatch(const TestCase &test_case, const std::string &label,
                    const std::string &expected, const std::string &actual) {
  std::cerr << test_case.directory << ": " << label << " mismatch\n";
  std::cerr << "--- expected\n" << expected;
  if (expected.empty() || expected[expected.size() - 1] != '\n') {
    std::cerr << "\n";
  }
  std::cerr << "--- actual\n" << actual;
  if (actual.empty() || actual[actual.size() - 1] != '\n') {
    std::cerr << "\n";
  }
}

std::string canonical_phase(const std::string &phase) {
  if (phase == "lower") {
    return "hir";
  }
  if (phase == "compile") {
    return "bc";
  }
  if (phase == "disasm") {
    return "bc-disasm";
  }
  return phase;
}

int phase_bundle_level(const std::string &phase) {
  const std::string canonical = canonical_phase(phase);
  if (canonical == "lex" || canonical == "parse-expr" || canonical == "parse" ||
      canonical == "bind" || canonical == "bind-diag" || canonical == "hir" ||
      canonical == "check") {
    return 1;
  }
  if (canonical == "bc" || canonical == "bc-disasm") {
    return 2;
  }
  if (canonical == "run") {
    return 3;
  }
  if (canonical == "load") {
    return 5;
  }
  if (canonical == "typed" || canonical == "typed-diag") {
    return 6;
  }
  return 99;
}

int bundle_level(const std::string &bundle) {
  if (bundle.empty() || bundle == "all" || bundle == "full" || bundle == "M5") {
    return 5;
  }
  if (bundle == "M6") {
    return 6;
  }
  if (bundle == "M11") {
    return 11;
  }
  if (bundle == "M1") {
    return 1;
  }
  if (bundle == "M2") {
    return 2;
  }
  if (bundle == "M3" || bundle == "M4") {
    return 3;
  }
  return -1;
}

bool bundle_allows_phase(const std::string &bundle, const std::string &phase) {
  if (bundle.empty()) {
    return true;
  }
  const int level = bundle_level(bundle);
  const int phase_level = phase_bundle_level(phase);
  return level >= 0 && (phase_level == 99 || phase_level <= level);
}

bool compile_case_to_module(const TestCase &test_case,
                            amber::bytecode::BcModule *module,
                            std::string *module_name) {
  const std::string source_path =
      join_path(test_case.directory, test_case.source);
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

  *module = std::move(decode_result.module);
  *module_name = parse_result.module_name.empty() ? "<anonymous>"
                                                  : parse_result.module_name;
  return true;
}

const amber::bytecode::BcMethod *
method_by_name(const amber::bytecode::BcModule &module,
               const std::string &name) {
  for (const amber::bytecode::BcMethod &method : module.methods) {
    if (method.selector_sym_id < module.symbols.size() &&
        module.symbols[method.selector_sym_id] == name) {
      return &method;
    }
  }
  return nullptr;
}

std::string check_result_to_json(const std::string &module_name,
                                 std::size_t diagnostic_count) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.check.v1\",\n";
  out << "  \"status\": \"ok\",\n";
  out << "  \"module\": \"" << json_escape(module_name) << "\",\n";
  out << "  \"diagnostic_count\": " << diagnostic_count << "\n";
  out << "}\n";
  return out.str();
}

std::string run_result_to_json(const std::string &entry,
                               const amber::bytecode::BcModule &module,
                               const amber::runtime::ExecutionResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.run.v1\",\n";
  out << "  \"entry\": \"" << json_escape(entry) << "\",\n";
  if (result.ok()) {
    out << "  \"status\": \"ok\",\n";
    out << "  \"value\": \""
        << json_escape(
               amber::runtime::value_to_debug_string(result.value, &module))
        << "\"\n";
  } else {
    out << "  \"status\": \"fault\",\n";
    out << "  \"error_name\": \"" << json_escape(result.fault->error_name)
        << "\",\n";
    out << "  \"message\": \"" << json_escape(result.fault->message) << "\"\n";
  }
  out << "}\n";
  return out.str();
}

std::string
load_result_to_json(const std::string &root_module,
                    const amber::runtime::RuntimeModuleLoadResult &result) {
  std::vector<amber::runtime::RuntimeModuleSnapshot> modules = result.modules;
  std::sort(modules.begin(), modules.end(),
            [](const amber::runtime::RuntimeModuleSnapshot &left,
               const amber::runtime::RuntimeModuleSnapshot &right) {
              return left.name < right.name;
            });

  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.load.v1\",\n";
  out << "  \"root\": \"" << json_escape(root_module) << "\",\n";
  out << "  \"status\": \"" << (result.ok ? "ok" : "error") << "\",\n";
  if (!result.ok) {
    out << "  \"error_name\": \"" << json_escape(result.error_name) << "\",\n";
    out << "  \"message\": \"" << json_escape(result.message) << "\",\n";
  }
  out << "  \"init_order\": [";
  for (std::size_t i = 0; i < result.init_order.size(); ++i) {
    if (i != 0U) {
      out << ", ";
    }
    out << "\"" << json_escape(result.init_order[i]) << "\"";
  }
  out << "],\n";
  out << "  \"modules\": [";
  for (std::size_t i = 0; i < modules.size(); ++i) {
    const amber::runtime::RuntimeModuleSnapshot &module = modules[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(module.name) << "\",";
    out << "\"state\":\""
        << amber::runtime::runtime_module_state_name(module.state) << "\",";
    out << "\"init_runs\":" << module.init_runs << ",";
    out << "\"dependencies\":[";
    for (std::size_t dep = 0; dep < module.dependencies.size(); ++dep) {
      if (dep != 0U) {
        out << ", ";
      }
      out << "\"" << json_escape(module.dependencies[dep]) << "\"";
    }
    out << "],\"exports\":[";
    for (std::size_t exp = 0; exp < module.exports.size(); ++exp) {
      if (exp != 0U) {
        out << ", ";
      }
      out << "{\"name\":\"" << json_escape(module.exports[exp].public_name)
          << "\",\"state\":\""
          << amber::runtime::runtime_export_cell_state_name(
                 module.exports[exp].state)
          << "\",\"target_kind\":\""
          << json_escape(module.exports[exp].target_kind) << "\"}";
    }
    out << "]}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
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
    print_mismatch(test_case, "token dump", expected, actual);
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
    print_mismatch(test_case, "AST dump", expected, actual);
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
    print_mismatch(test_case, "AST dump", expected, actual);
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
    print_mismatch(test_case, "bind dump", expected, actual);
    return false;
  }
  return true;
}

bool run_check_case(const TestCase &test_case) {
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

  const std::string actual = check_result_to_json(
      parse_result.module_name, bind_result.diagnostics.size());
  const std::string expected = read_file(expect_path);
  if (actual != expected) {
    print_mismatch(test_case, "check result", expected, actual);
    return false;
  }
  return true;
}

bool run_typed_case(const TestCase &test_case) {
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

  amber::checker::CheckResult check_result = amber::checker::check_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  if (!check_result.ok()) {
    std::cerr << test_case.directory << ": typed diagnostics:\n"
              << amber::lexer::diagnostics_to_json(check_result.diagnostics);
    return false;
  }

  const std::string actual = amber::checker::check_result_to_json(
      check_result, parse_result.module_name);
  const std::string expected = read_file(expect_path);
  if (actual != expected) {
    print_mismatch(test_case, "typed result", expected, actual);
    return false;
  }
  return true;
}

bool run_typed_diag_case(const TestCase &test_case) {
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
      print_mismatch(test_case, "lexer diagnostic", expected, actual);
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
      print_mismatch(test_case, "parser diagnostic", expected, actual);
      return false;
    }
    return true;
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    const std::string actual =
        amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    const std::string expected = read_file(expect_path);
    if (actual != expected) {
      print_mismatch(test_case, "binder diagnostic", expected, actual);
      return false;
    }
    return true;
  }

  amber::checker::CheckResult check_result = amber::checker::check_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  if (check_result.diagnostics.empty()) {
    std::cerr << test_case.directory << ": expected diagnostics, got success\n";
    return false;
  }
  const std::string actual =
      amber::lexer::diagnostics_to_json(check_result.diagnostics);
  const std::string expected = read_file(expect_path);
  if (actual != expected) {
    print_mismatch(test_case, "typed diagnostic", expected, actual);
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
    print_mismatch(test_case, "HIR dump", expected, actual);
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
    print_mismatch(test_case, "bytecode dump", expected, actual);
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
    print_mismatch(test_case, "bytecode disasm", expected, actual);
    return false;
  }
  return true;
}

bool run_vm_case(const TestCase &test_case) {
  amber::bytecode::BcModule module;
  std::string module_name;
  if (!compile_case_to_module(test_case, &module, &module_name)) {
    return false;
  }

  const std::string entry = test_case.entry.empty() ? "main" : test_case.entry;
  const amber::bytecode::BcMethod *method = method_by_name(module, entry);
  if (method == nullptr) {
    std::cerr << test_case.directory << ": missing run entry '" << entry
              << "'\n";
    return false;
  }

  const amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, method->entry_code_id);
  const std::string actual = run_result_to_json(entry, module, result);
  const std::string expected =
      read_file(join_path(test_case.directory, test_case.expect));
  if (actual != expected) {
    print_mismatch(test_case, "run result", expected, actual);
    return false;
  }
  return true;
}

bool run_load_case(const TestCase &test_case) {
  amber::bytecode::BcModule module;
  std::string module_name;
  if (!compile_case_to_module(test_case, &module, &module_name)) {
    return false;
  }

  amber::runtime::RuntimeModuleLoader loader;
  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(module);
  const amber::runtime::RuntimeModuleLoadResult added =
      loader.add_serialized_module(module_name, bytes);
  amber::runtime::RuntimeModuleLoadResult result = added;
  if (added.ok) {
    result = loader.initialize_all();
  }

  const std::string actual = load_result_to_json(module_name, result);
  const std::string expected =
      read_file(join_path(test_case.directory, test_case.expect));
  if (actual != expected) {
    print_mismatch(test_case, "load result", expected, actual);
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
      print_mismatch(test_case, "lexer diagnostic", expected, actual);
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
      print_mismatch(test_case, "parser diagnostic", expected, actual);
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
    print_mismatch(test_case, "binder diagnostic", expected, actual);
    return false;
  }
  return true;
}

bool compile_all_candidate(const TestCase &test_case) {
  const std::string phase = canonical_phase(test_case.phase);
  return phase == "bc" || phase == "bc-disasm" || phase == "run" ||
         phase == "load";
}

bool run_compile_all_bundle(const std::vector<TestCase> &cases) {
  for (const TestCase &test_case : cases) {
    if (!compile_all_candidate(test_case)) {
      continue;
    }
    amber::bytecode::BcModule module;
    std::string module_name;
    if (!compile_case_to_module(test_case, &module, &module_name)) {
      return false;
    }
    const std::vector<std::uint8_t> bytes =
        amber::bytecode::serialize_module(module);
    amber::bytecode::DecodeResult decoded =
        amber::bytecode::deserialize_module(bytes);
    if (!decoded.ok()) {
      std::cerr << test_case.directory
                << ": compile-all verifier diagnostics:\n"
                << amber::bytecode::verify_errors_to_json(decoded.errors);
      return false;
    }
    (void)amber::bytecode::module_to_disasm(
        decoded.module, decoded.sections,
        amber::lexer::sha256_hex(std::string(bytes.begin(), bytes.end())));
    const std::string phase = canonical_phase(test_case.phase);
    if (phase == "run" && !run_vm_case(test_case)) {
      return false;
    }
    if (phase == "load" && !run_load_case(test_case)) {
      return false;
    }
  }
  return true;
}

void usage(std::ostream &out) {
  out << "usage: ambertest run <path> [--bundle M1|M2|M3|M4|M5|M6|M11]\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    if ((argc != 3 && argc != 5) || std::string(argv[1]) != "run") {
      usage(std::cerr);
      return 2;
    }
    std::string bundle;
    if (argc == 5) {
      if (std::string(argv[3]) != "--bundle") {
        usage(std::cerr);
        return 2;
      }
      bundle = argv[4];
      if (bundle_level(bundle) < 0) {
        std::cerr << "ambertest: unknown bundle '" << bundle << "'\n";
        return 2;
      }
    }

    std::vector<TestCase> cases;
    discover_cases(argv[2], &cases);
    std::sort(cases.begin(), cases.end(),
              [](const TestCase &left, const TestCase &right) {
                return left.directory < right.directory;
              });
    if (cases.empty()) {
      std::cerr << "ambertest: no meta.json cases found under " << argv[2]
                << "\n";
      return 1;
    }

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    for (const TestCase &test_case : cases) {
      if (!bundle_allows_phase(bundle, test_case.phase)) {
        ++skipped;
        continue;
      }

      const std::string phase = canonical_phase(test_case.phase);
      if (phase == "lex") {
        if (run_lex_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "parse-expr") {
        if (run_parse_expr_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "parse") {
        if (run_parse_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "bind") {
        if (run_bind_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "check") {
        if (run_check_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "typed") {
        if (run_typed_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "typed-diag") {
        if (run_typed_diag_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "hir") {
        if (run_hir_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "bc") {
        if (run_bc_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "bc-disasm") {
        if (run_bc_disasm_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "run") {
        if (run_vm_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "load") {
        if (run_load_case(test_case)) {
          ++passed;
        } else {
          ++failed;
        }
      } else if (phase == "bind-diag") {
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

    if (bundle == "M11") {
      if (run_compile_all_bundle(cases)) {
        ++passed;
      } else {
        ++failed;
      }
    }

    std::cout << "ambertest: " << passed << " passed, " << failed << " failed";
    if (!bundle.empty()) {
      std::cout << ", " << skipped << " skipped for " << bundle;
    }
    std::cout << "\n";
    return failed == 0 ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << "ambertest: " << error.what() << "\n";
    return 1;
  }
}
