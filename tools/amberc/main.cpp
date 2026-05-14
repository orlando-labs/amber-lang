#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/ast/expr.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string read_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open input file: " + path);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void usage(std::ostream &out) {
  out << "usage:\n";
  out << "  amberc lex <file>\n";
  out << "  amberc parse <file>\n";
  out << "  amberc parse-expr <file>\n";
  out << "  amberc bind <file>\n";
  out << "  amberc hir <file>\n";
  out << "  amberc bc <file>\n";
  out << "  amberc bc-disasm <file>\n";
  out << "  amberc amberbc-dump <file>\n";
  out << "  amberc amberbc-verify <file>\n";
  out << "  amberc amberbc-disasm <file>\n";
  out << "  amberc --version\n";
}

amber::lexer::LexResult lex_source(const std::string &source,
                                   const std::string &path) {
  amber::lexer::Lexer lexer(source, path);
  return lexer.lex();
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--version") {
      std::cout << "amberc 0.1.0-dev\n";
      return 0;
    }
    if (argc != 3 ||
        (std::string(argv[1]) != "lex" && std::string(argv[1]) != "parse" &&
         std::string(argv[1]) != "bind" && std::string(argv[1]) != "hir" &&
         std::string(argv[1]) != "bc" && std::string(argv[1]) != "bc-disasm" &&
         std::string(argv[1]) != "parse-expr" &&
         std::string(argv[1]) != "amberbc-dump" &&
         std::string(argv[1]) != "amberbc-verify" &&
         std::string(argv[1]) != "amberbc-disasm")) {
      usage(std::cerr);
      return 2;
    }

    const std::string path = argv[2];
    const std::string command = argv[1];
    if (command == "amberbc-dump" || command == "amberbc-verify" ||
        command == "amberbc-disasm") {
      const std::string binary = read_file(path);
      const std::vector<std::uint8_t> bytes(binary.begin(), binary.end());
      amber::bytecode::DecodeResult decode_result =
          amber::bytecode::deserialize_module(bytes);
      if (!decode_result.ok()) {
        std::cerr << amber::bytecode::verify_errors_to_json(
            decode_result.errors);
        return 1;
      }
      if (command == "amberbc-verify") {
        std::cout << amber::bytecode::verify_errors_to_json({});
        return 0;
      }
      const std::string artifact_hash = amber::lexer::sha256_hex(binary);
      if (command == "amberbc-disasm") {
        std::cout << amber::bytecode::module_to_disasm(
            decode_result.module, decode_result.sections, artifact_hash);
        return 0;
      }
      std::cout << amber::bytecode::module_to_json(
          decode_result.module, decode_result.sections, artifact_hash);
      return 0;
    }

    const std::string source = read_file(path);
    amber::lexer::LexResult lex_result = lex_source(source, path);
    if (!lex_result.ok()) {
      std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
      return 1;
    }

    if (command == "lex") {
      std::cout << amber::lexer::tokens_to_json(
          lex_result.tokens, amber::lexer::sha256_hex(source));
      return 0;
    }

    amber::parser::Parser parser(lex_result.tokens);
    if (command == "parse" || command == "bind" || command == "hir" ||
        command == "bc" || command == "bc-disasm") {
      amber::parser::ParseModuleResult parse_result =
          parser.parse_module_unit();
      if (!parse_result.ok()) {
        std::cerr << amber::lexer::diagnostics_to_json(
            parse_result.diagnostics);
        return 1;
      }
      if (command == "bind" || command == "hir" || command == "bc" ||
          command == "bc-disasm") {
        amber::binder::BindResult bind_result = amber::binder::bind_module(
            parse_result.items, parse_result.module_name);
        if (!bind_result.diagnostics.empty()) {
          std::cerr << amber::lexer::diagnostics_to_json(
              bind_result.diagnostics);
        }
        if (!bind_result.ok()) {
          return 1;
        }
        if (command == "bind") {
          std::cout << amber::binder::bind_graph_to_json(
              bind_result.graph, parse_result.module_name,
              amber::lexer::sha256_hex(source));
          return 0;
        }
        amber::hir::Program program = amber::hir::lower_module(
            parse_result.items, parse_result.module_name, bind_result.graph);
        if (command == "bc" || command == "bc-disasm") {
          amber::bytecode::EmitResult emit_result =
              amber::bytecode::emit_program(program, parse_result.module_name);
          if (!emit_result.ok()) {
            std::cerr << amber::lexer::diagnostics_to_json(
                emit_result.diagnostics);
            return 1;
          }
          const std::vector<std::uint8_t> bytes =
              amber::bytecode::serialize_module(emit_result.module);
          amber::bytecode::DecodeResult decode_result =
              amber::bytecode::deserialize_module(bytes);
          if (!decode_result.ok()) {
            std::cerr << amber::bytecode::verify_errors_to_json(
                decode_result.errors);
            return 1;
          }
          const std::string artifact_hash =
              amber::lexer::sha256_hex(std::string(bytes.begin(), bytes.end()));
          if (command == "bc-disasm") {
            std::cout << amber::bytecode::module_to_disasm(
                decode_result.module, decode_result.sections, artifact_hash);
            return 0;
          }
          std::cout << amber::bytecode::module_to_json(
              decode_result.module, decode_result.sections, artifact_hash);
          return 0;
        }
        std::cout << amber::hir::program_to_json(
            program, parse_result.module_name,
            amber::lexer::sha256_hex(source));
        return 0;
      }
      std::cout << amber::ast::ast_module_to_json(
          parse_result.items, parse_result.module_name,
          amber::lexer::sha256_hex(source));
      return 0;
    }

    amber::parser::ParseResult parse_result = parser.parse_expression_unit();
    if (!parse_result.ok()) {
      std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
      return 1;
    }
    std::cout << amber::ast::ast_module_to_json(
        *parse_result.expr, amber::lexer::sha256_hex(source));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "amberc: " << error.what() << "\n";
    return 1;
  }
}
