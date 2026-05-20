#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/ast/expr.h"
#include "frontend/binder/binder.h"
#include "frontend/checker/checker.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "optimizer/mir.h"
#include "optimizer/native.h"
#include "package/package.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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
  out << "  amberc typed <file>\n";
  out << "  amberc hir <file>\n";
  out << "  amberc mir <file>\n";
  out << "  amberc mir-dump <file>\n";
  out << "  amberc mir-verify <file>\n";
  out << "  amberc native <file>\n";
  out << "  amberc native-dump <file>\n";
  out << "  amberc native-verify <file>\n";
  out << "  amberc bc <file>\n";
  out << "  amberc bc-disasm <file>\n";
  out << "  amberc amberbc-dump <file>\n";
  out << "  amberc amberbc-verify <file>\n";
  out << "  amberc amberbc-disasm <file>\n";
  out << "  amberc package-manifest <amber.toml>\n";
  out << "  amberc package-lock <amber.toml>\n";
  out << "  amberc package-build <amber.toml> <out.amberpkg> "
         "[--sign-key <key>] [--key-id <id>]\n";
  out << "  amberc package-inspect <file.amberpkg>\n";
  out << "  amberc package-verify <file.amberpkg> [--sign-key <key>]\n";
  out << "  amberc package-install <file.amberpkg> <registry-dir> "
         "[--sign-key <key>]\n";
  out << "  amberc package-publish <file.amberpkg> <registry-dir> "
         "[--sign-key <key>]\n";
  out << "  amberc --version\n";
}

amber::lexer::LexResult lex_source(const std::string &source,
                                   const std::string &path) {
  amber::lexer::Lexer lexer(source, path);
  return lexer.lex();
}

std::string dirname(const std::string &path) {
  const std::size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0U) {
    return path.substr(0, 1);
  }
  return path.substr(0, slash);
}

std::string join_path(const std::string &left, const std::string &right) {
  if (left.empty() || left == ".") {
    return right;
  }
  if (!right.empty() && (right[0] == '/' || right[0] == '\\')) {
    return right;
  }
  if (left[left.size() - 1U] == '/' || left[left.size() - 1U] == '\\') {
    return left + right;
  }
  return left + "/" + right;
}

void write_file(const std::string &path, const std::string &contents) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  output << contents;
  if (!output) {
    throw std::runtime_error("failed to write output file: " + path);
  }
}

std::string package_diagnostics_to_string(
    const std::vector<amber::pkg::PackageDiagnostic> &diagnostics) {
  std::ostringstream out;
  for (const amber::pkg::PackageDiagnostic &diagnostic : diagnostics) {
    out << diagnostic.error_name << ": " << diagnostic.message;
    if (!diagnostic.path.empty()) {
      out << " (" << diagnostic.path << ")";
    }
    out << "\n";
  }
  return out.str();
}

std::vector<std::uint8_t>
compile_source_to_bytecode(const std::string &path,
                           const std::string &expected_module_name) {
  const std::string source = read_file(path);
  amber::lexer::LexResult lex_result = lex_source(source, path);
  if (!lex_result.ok()) {
    throw std::runtime_error(
        amber::lexer::diagnostics_to_json(lex_result.diagnostics));
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    throw std::runtime_error(
        amber::lexer::diagnostics_to_json(parse_result.diagnostics));
  }
  if (!expected_module_name.empty() &&
      parse_result.module_name != expected_module_name) {
    throw std::runtime_error("manifest module '" + expected_module_name +
                             "' does not match source package '" +
                             parse_result.module_name + "' in " + path);
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    throw std::runtime_error(
        amber::lexer::diagnostics_to_json(bind_result.diagnostics));
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    throw std::runtime_error(
        amber::lexer::diagnostics_to_json(emit_result.diagnostics));
  }
  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(emit_result.module);
  amber::bytecode::DecodeResult decode_result =
      amber::bytecode::deserialize_module(bytes);
  if (!decode_result.ok()) {
    throw std::runtime_error(
        amber::bytecode::verify_errors_to_json(decode_result.errors));
  }
  return bytes;
}

amber::pkg::PackageBuildOptions
parse_package_build_options(int argc, char **argv, int start_index) {
  amber::pkg::PackageBuildOptions options;
  for (int i = start_index; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--sign-key" && i + 1 < argc) {
      options.signing_key = argv[++i];
    } else if (arg == "--key-id" && i + 1 < argc) {
      options.key_id = argv[++i];
    } else {
      throw std::runtime_error("unknown package option: " + arg);
    }
  }
  return options;
}

std::string parse_package_signing_key(int argc, char **argv, int start_index) {
  std::string signing_key;
  for (int i = start_index; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--sign-key" && i + 1 < argc) {
      signing_key = argv[++i];
    } else {
      throw std::runtime_error("unknown package option: " + arg);
    }
  }
  return signing_key;
}

int run_package_command(int argc, char **argv) {
  const std::string command = argv[1];
  if ((command == "package-manifest" || command == "package-lock") &&
      argc == 3) {
    const std::string manifest_source = read_file(argv[2]);
    const amber::pkg::PackageManifestResult manifest =
        amber::pkg::parse_manifest_toml(manifest_source, argv[2]);
    if (!manifest.ok()) {
      std::cerr << package_diagnostics_to_string(manifest.diagnostics);
      return 1;
    }
    if (command == "package-manifest") {
      std::cout << amber::pkg::manifest_to_json(manifest.manifest);
    } else {
      std::cout << amber::pkg::render_lockfile(manifest.manifest);
    }
    return 0;
  }

  if (command == "package-build" && argc >= 4) {
    const std::string manifest_path = argv[2];
    const std::string out_path = argv[3];
    const amber::pkg::PackageBuildOptions options =
        parse_package_build_options(argc, argv, 4);
    const std::string manifest_source = read_file(manifest_path);
    const amber::pkg::PackageManifestResult manifest =
        amber::pkg::parse_manifest_toml(manifest_source, manifest_path);
    if (!manifest.ok()) {
      std::cerr << package_diagnostics_to_string(manifest.diagnostics);
      return 1;
    }

    std::vector<amber::pkg::PackageModuleBlob> modules;
    const std::string root_dir = dirname(manifest_path);
    for (const amber::pkg::PackageModule &module : manifest.manifest.modules) {
      amber::pkg::PackageModuleBlob blob;
      blob.name = module.name;
      blob.path = module.path;
      blob.bytes = compile_source_to_bytecode(join_path(root_dir, module.path),
                                              module.name);
      modules.push_back(std::move(blob));
    }
    const amber::pkg::PackageBuildResult built =
        amber::pkg::build_package_artifact(manifest.manifest, modules, options);
    if (!built.ok) {
      std::cerr << package_diagnostics_to_string(built.diagnostics);
      return 1;
    }
    write_file(out_path, built.serialized);
    std::cout << amber::pkg::artifact_to_json(built.artifact);
    return 0;
  }

  if (command == "package-inspect" && argc == 3) {
    const std::string serialized = read_file(argv[2]);
    const amber::pkg::PackageParseResult parsed =
        amber::pkg::parse_package_artifact(serialized, argv[2]);
    if (!parsed.ok()) {
      std::cerr << package_diagnostics_to_string(parsed.diagnostics);
      return 1;
    }
    std::cout << amber::pkg::artifact_to_json(parsed.artifact);
    return 0;
  }

  if (command == "package-verify" && argc >= 3) {
    const std::string serialized = read_file(argv[2]);
    const std::string signing_key = parse_package_signing_key(argc, argv, 3);
    const amber::pkg::PackageVerifyResult verified =
        amber::pkg::verify_package_artifact(serialized, signing_key, argv[2]);
    std::cout << amber::pkg::verify_result_to_json(verified);
    return verified.ok ? 0 : 1;
  }

  if ((command == "package-install" || command == "package-publish") &&
      argc >= 4) {
    const std::string serialized = read_file(argv[2]);
    const std::string signing_key = parse_package_signing_key(argc, argv, 4);
    const amber::pkg::PackageRegistryResult result =
        command == "package-install"
            ? amber::pkg::install_package_artifact(serialized, argv[3],
                                                   signing_key)
            : amber::pkg::publish_package_artifact(serialized, argv[3],
                                                   signing_key);
    std::cout << amber::pkg::registry_result_to_json(result);
    return result.ok ? 0 : 1;
  }

  usage(std::cerr);
  return 2;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--version") {
      std::cout << "amberc 0.1.0-dev\n";
      return 0;
    }
    if (argc >= 2 && std::string(argv[1]).find("package-") == 0U) {
      return run_package_command(argc, argv);
    }
    if (argc != 3 ||
        (std::string(argv[1]) != "lex" && std::string(argv[1]) != "parse" &&
         std::string(argv[1]) != "bind" && std::string(argv[1]) != "typed" &&
         std::string(argv[1]) != "hir" && std::string(argv[1]) != "mir" &&
         std::string(argv[1]) != "mir-dump" &&
         std::string(argv[1]) != "mir-verify" && std::string(argv[1]) != "bc" &&
         std::string(argv[1]) != "native" &&
         std::string(argv[1]) != "native-dump" &&
         std::string(argv[1]) != "native-verify" &&
         std::string(argv[1]) != "bc-disasm" &&
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
    if (command == "parse" || command == "bind" || command == "typed" ||
        command == "hir" || command == "mir" || command == "mir-dump" ||
        command == "mir-verify" || command == "native" ||
        command == "native-dump" || command == "native-verify" ||
        command == "bc" || command == "bc-disasm") {
      amber::parser::ParseModuleResult parse_result =
          parser.parse_module_unit();
      if (!parse_result.ok()) {
        std::cerr << amber::lexer::diagnostics_to_json(
            parse_result.diagnostics);
        return 1;
      }
      if (command == "bind" || command == "typed" || command == "hir" ||
          command == "mir" || command == "mir-dump" ||
          command == "mir-verify" || command == "native" ||
          command == "native-dump" || command == "native-verify" ||
          command == "bc" || command == "bc-disasm") {
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
        if (command == "typed") {
          amber::checker::CheckResult check_result =
              amber::checker::check_module(parse_result.items,
                                           parse_result.module_name,
                                           bind_result.graph);
          if (!check_result.ok()) {
            std::cerr << amber::lexer::diagnostics_to_json(
                check_result.diagnostics);
            return 1;
          }
          std::cout << amber::checker::check_result_to_json(
              check_result, parse_result.module_name);
          return 0;
        }
        amber::hir::Program program = amber::hir::lower_module(
            parse_result.items, parse_result.module_name, bind_result.graph);
        if (command == "mir" || command == "mir-dump" ||
            command == "mir-verify" || command == "native" ||
            command == "native-dump" || command == "native-verify") {
          amber::mir::Module mir_module =
              amber::mir::lower_program(program, parse_result.module_name);
          amber::mir::ValidationResult validation =
              amber::mir::validate_module(mir_module);
          if (command == "mir-verify") {
            std::cout << amber::mir::validation_errors_to_json(
                validation.errors);
            return validation.ok() ? 0 : 1;
          }
          if (!validation.ok()) {
            std::cerr << amber::mir::validation_errors_to_json(
                validation.errors);
            return 1;
          }
          const std::string source_hash = amber::lexer::sha256_hex(source);
          if (command == "mir-dump") {
            std::cout << amber::mir::module_to_dump(mir_module, source_hash);
            return 0;
          }
          if (command == "native" || command == "native-dump" ||
              command == "native-verify") {
            amber::bytecode::EmitResult emit_result =
                amber::bytecode::emit_program(program,
                                              parse_result.module_name);
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
            amber::native::NativeModule native_module =
                amber::native::compile_native_module(decode_result.module,
                                                     mir_module);
            amber::native::NativeValidationResult native_validation =
                amber::native::validate_native_module(native_module,
                                                      &decode_result.module);
            if (command == "native-verify") {
              std::cout << amber::native::diagnostics_to_json(
                  native_validation.diagnostics);
              return native_validation.ok() ? 0 : 1;
            }
            if (!native_validation.ok()) {
              std::cerr << amber::native::diagnostics_to_json(
                  native_validation.diagnostics);
              return 1;
            }
            if (command == "native-dump") {
              std::cout << amber::native::module_to_dump(native_module,
                                                         source_hash);
              return 0;
            }
            std::cout << amber::native::module_to_json(native_module,
                                                       source_hash);
            return 0;
          }
          std::cout << amber::mir::module_to_json(mir_module, source_hash);
          return 0;
        }
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
