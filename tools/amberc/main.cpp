#include "build/build.h"
#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/ast/expr.h"
#include "frontend/binder/binder.h"
#include "frontend/checker/checker.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "frozen/image.h"
#include "optimizer/mir.h"
#include "optimizer/native.h"
#include "package/package.h"
#include "profile/data.h"
#include "profile/modern.h"
#include "profile/replay.h"
#include "profile/wasm_accel.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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
  out << "  amberc effects-check <file>\n";
  out << "  amberc hir <file>\n";
  out << "  amberc mir <file>\n";
  out << "  amberc mir-dump <file>\n";
  out << "  amberc mir-verify <file>\n";
  out << "  amberc native <file>\n";
  out << "  amberc native-dump <file>\n";
  out << "  amberc native-verify <file>\n";
  out << "  amberc bc <file>\n";
  out << "  amberc bc-disasm <file>\n";
  out << "  amberc build <amber.build.json> [--out-dir <dir>] "
         "[--cache-dir <dir>] [--no-cache]\n";
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
  out << "  amberc capabilities-check <amber.toml> [--grant "
         "<cap[=target]>...]\n";
  out << "  amberc replay-check <file.ambertrace>\n";
  out << "  amberc trace-inspect <file.ambertrace>\n";
  out << "  amberc schema-check <file.amberschema>\n";
  out << "  amberc table-explain <file.ambertable>\n";
  out << "  amberc wasm-build <file.amberwasm>\n";
  out << "  amberc accel-check <file.amberaccel>\n";
  out << "  amberc symbols <file>\n";
  out << "  amberc explain <file> --span <line>:<column>\n";
  out << "  amberc patch-check <file.ambermodern>\n";
  out << "  amberc provenance-audit <file.ambermodern>\n";
  out << "  amberc contract-check <file.ambermodern>\n";
  out << "  amberc privacy-check <file.ambermodern>\n";
  out << "  amberc workflow-check <file.ambermodern>\n";
  out << "  amberc image-build <amber.toml> <out.amberimg> "
         "[--sign-key <key>] [--key-id <id>]\n";
  out << "  amberc image-inspect <file.amberimg>\n";
  out << "  amberc image-verify <file.amberimg> [--sign-key <key>]\n";
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

void write_bytes(const std::string &path,
                 const std::vector<std::uint8_t> &bytes) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("failed to write output file: " + path);
  }
}

std::vector<std::uint8_t> read_bytes(const std::string &path) {
  const std::string data = read_file(path);
  return std::vector<std::uint8_t>(data.begin(), data.end());
}

std::string bytes_to_string(const std::vector<std::uint8_t> &bytes) {
  return std::string(bytes.begin(), bytes.end());
}

std::string bytes_to_hex(const std::array<std::uint8_t, 32> &bytes) {
  std::ostringstream out;
  const char *hex = "0123456789abcdef";
  for (const std::uint8_t byte : bytes) {
    out << hex[(byte >> 4U) & 0x0FU] << hex[byte & 0x0FU];
  }
  return out.str();
}

int from_hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + c - 'a';
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + c - 'A';
  }
  return -1;
}

std::array<std::uint8_t, 32> sha256_array(const std::string &value) {
  const std::string hex = amber::lexer::sha256_hex(value);
  std::array<std::uint8_t, 32> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = from_hex_digit(hex[i * 2U]);
    const int lo = from_hex_digit(hex[i * 2U + 1U]);
    if (hi < 0 || lo < 0) {
      throw std::runtime_error("internal sha256 hex conversion failed");
    }
    out[i] = static_cast<std::uint8_t>((hi << 4U) | lo);
  }
  return out;
}

std::array<std::uint8_t, 32> array_from_hex32(const std::string &hex) {
  if (hex.size() != 64U) {
    throw std::runtime_error("expected 32-byte hex digest");
  }
  std::array<std::uint8_t, 32> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = from_hex_digit(hex[i * 2U]);
    const int lo = from_hex_digit(hex[i * 2U + 1U]);
    if (hi < 0 || lo < 0) {
      throw std::runtime_error("invalid 32-byte hex digest");
    }
    out[i] = static_cast<std::uint8_t>((hi << 4U) | lo);
  }
  return out;
}

std::uint32_t ensure_string_id(amber::bytecode::BcModule *module,
                               const std::string &value) {
  for (std::size_t i = 0; i < module->strings.size(); ++i) {
    if (module->strings[i] == value) {
      return static_cast<std::uint32_t>(i);
    }
  }
  module->strings.push_back(value);
  return static_cast<std::uint32_t>(module->strings.size() - 1U);
}

void add_module_attr(amber::bytecode::BcModule *module, const std::string &key,
                     const std::string &value) {
  module->attrs.push_back(
      {ensure_string_id(module, key), ensure_string_id(module, value)});
}

std::string safe_artifact_name(const std::string &module_name) {
  std::string out;
  for (const unsigned char c : module_name) {
    if (std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('_');
    }
  }
  return out.empty() ? "module" : out;
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

std::string frozen_diagnostics_to_string(
    const std::vector<amber::frozen::FrozenImageDiagnostic> &diagnostics) {
  std::ostringstream out;
  for (const amber::frozen::FrozenImageDiagnostic &diagnostic : diagnostics) {
    out << diagnostic.error_name << ": " << diagnostic.message;
    if (!diagnostic.module_name.empty()) {
      out << " (" << diagnostic.module_name << ")";
    }
    out << "\n";
  }
  return out.str();
}

std::vector<amber::lexer::Diagnostic>
effect_diagnostics_only(const amber::checker::CheckResult &result) {
  std::vector<amber::lexer::Diagnostic> diagnostics;
  for (const amber::lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.phase == "effects") {
      diagnostics.push_back(diagnostic);
    }
  }
  return diagnostics;
}

amber::modern::SourceLocation
parse_cli_source_location(const std::string &path, const std::string &raw) {
  const std::size_t colon = raw.find(':');
  if (colon == std::string::npos) {
    throw std::runtime_error("span must be <line>:<column>");
  }
  amber::modern::SourceLocation source;
  source.file = path;
  source.line =
      static_cast<std::uint32_t>(std::stoul(raw.substr(0, colon), nullptr, 10));
  source.column = static_cast<std::uint32_t>(
      std::stoul(raw.substr(colon + 1U), nullptr, 10));
  return source;
}

std::vector<amber::modern::AgentSymbol>
agent_symbols_from_bind_graph(const amber::binder::BindGraph &graph,
                              const std::string &module_name) {
  std::vector<amber::modern::AgentSymbol> symbols;
  symbols.reserve(graph.bindings.size());
  for (const amber::binder::Binding &binding : graph.bindings) {
    amber::modern::AgentSymbol symbol;
    symbol.symbol_id = binding.id;
    symbol.name = binding.name;
    symbol.kind = binding.role == "param" ? "param" : binding.kind;
    if (symbol.kind == "import") {
      symbol.kind = "module";
    }
    symbol.module = module_name.empty() ? "module" : module_name;
    symbol.visibility = "internal";
    symbol.source = amber::modern::source_from_span(binding.span);
    symbol.defined_in = binding.span.file;
    symbol.doc_summary = binding.source;
    for (const amber::binder::Export &export_record : graph.exports) {
      if (export_record.binding_id == binding.id) {
        symbol.visibility = "public";
        break;
      }
    }
    for (const amber::binder::Reference &reference : graph.references) {
      if (reference.binding_id == binding.id) {
        symbol.references.push_back(
            amber::modern::source_from_span(reference.span));
      }
    }
    symbols.push_back(amber::modern::normalize_agent_symbol(std::move(symbol)));
  }
  return symbols;
}

int run_modern_document_command(int argc, char **argv) {
  if (argc != 3) {
    usage(std::cerr);
    return 2;
  }
  const std::string command = argv[1];
  const std::string source = read_file(argv[2]);
  const amber::modern::ModernDocumentParseResult parsed =
      amber::modern::parse_modern_document(source);
  if (command == "patch-check" || command == "provenance-audit") {
    amber::modern::AgentValidationResult result =
        amber::modern::validate_agent_metadata(parsed.document.symbols,
                                               parsed.document.patches,
                                               parsed.document.provenance);
    result.diagnostics.insert(result.diagnostics.begin(),
                              parsed.diagnostics.begin(),
                              parsed.diagnostics.end());
    result.ok = result.ok && parsed.ok();
    std::cout << amber::modern::agent_validation_to_json(result);
    return result.ok ? 0 : 1;
  }
  if (command == "contract-check") {
    amber::modern::ContractValidationResult result =
        amber::modern::validate_contract_metadata(parsed.document.contracts,
                                                  parsed.document.properties);
    result.diagnostics.insert(result.diagnostics.begin(),
                              parsed.diagnostics.begin(),
                              parsed.diagnostics.end());
    result.ok = result.ok && parsed.ok();
    std::cout << amber::modern::contract_validation_to_json(result);
    return result.ok ? 0 : 1;
  }
  if (command == "privacy-check") {
    amber::modern::PrivacyValidationResult result =
        amber::modern::validate_privacy_metadata(
            parsed.document.privacy_labels, parsed.document.privacy_policies,
            parsed.document.lineage);
    result.diagnostics.insert(result.diagnostics.begin(),
                              parsed.diagnostics.begin(),
                              parsed.diagnostics.end());
    result.ok = result.ok && parsed.ok();
    std::cout << amber::modern::privacy_validation_to_json(result);
    return result.ok ? 0 : 1;
  }
  if (command == "workflow-check") {
    amber::modern::WorkflowValidationResult result =
        amber::modern::validate_workflow_metadata(
            parsed.document.workflow_steps, parsed.document.workflow_history);
    result.diagnostics.insert(result.diagnostics.begin(),
                              parsed.diagnostics.begin(),
                              parsed.diagnostics.end());
    result.ok = result.ok && parsed.ok();
    std::cout << amber::modern::workflow_validation_to_json(result);
    return result.ok ? 0 : 1;
  }
  usage(std::cerr);
  return 2;
}

struct CompiledModuleArtifact {
  std::vector<std::uint8_t> bytes;
  amber::native::NativeModule native_module;
};

CompiledModuleArtifact compile_source_to_module_artifact(
    const std::string &path, const std::string &expected_module_name,
    const std::vector<amber::capability::CapabilityRequest> &capabilities =
        {}) {
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
  amber::checker::CheckResult check_result = amber::checker::check_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  const std::vector<amber::lexer::Diagnostic> effect_diagnostics =
      effect_diagnostics_only(check_result);
  if (!effect_diagnostics.empty()) {
    throw std::runtime_error(
        amber::lexer::diagnostics_to_json(effect_diagnostics));
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::mir::Module mir_module =
      amber::mir::lower_program(program, parse_result.module_name);
  amber::mir::ValidationResult mir_validation =
      amber::mir::validate_module(mir_module);
  if (!mir_validation.ok()) {
    throw std::runtime_error(
        amber::mir::validation_errors_to_json(mir_validation.errors));
  }

  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    throw std::runtime_error(
        amber::lexer::diagnostics_to_json(emit_result.diagnostics));
  }
  emit_result.module.capabilities = capabilities;
  emit_result.module.effects = check_result.effect_summaries;
  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(emit_result.module);
  amber::bytecode::DecodeResult decode_result =
      amber::bytecode::deserialize_module(bytes);
  if (!decode_result.ok()) {
    throw std::runtime_error(
        amber::bytecode::verify_errors_to_json(decode_result.errors));
  }

  amber::native::NativeModule native_module =
      amber::native::compile_native_module(decode_result.module, mir_module);
  amber::native::NativeValidationResult native_validation =
      amber::native::validate_native_module(native_module,
                                            &decode_result.module);
  if (!native_validation.ok()) {
    throw std::runtime_error(
        amber::native::diagnostics_to_json(native_validation.diagnostics));
  }
  return {bytes, std::move(native_module)};
}

amber::bytecode::BcModule compile_source_text_to_module(
    const std::string &source, const std::string &source_path,
    const std::string &expected_module_name,
    const std::vector<amber::capability::CapabilityRequest> &capabilities =
        {}) {
  amber::lexer::LexResult lex_result = lex_source(source, source_path);
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
                             parse_result.module_name + "' in " + source_path);
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    throw std::runtime_error(
        amber::lexer::diagnostics_to_json(bind_result.diagnostics));
  }
  amber::checker::CheckResult check_result = amber::checker::check_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  const std::vector<amber::lexer::Diagnostic> effect_diagnostics =
      effect_diagnostics_only(check_result);
  if (!effect_diagnostics.empty()) {
    throw std::runtime_error(
        amber::lexer::diagnostics_to_json(effect_diagnostics));
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    throw std::runtime_error(
        amber::lexer::diagnostics_to_json(emit_result.diagnostics));
  }
  emit_result.module.capabilities = capabilities;
  emit_result.module.effects = check_result.effect_summaries;
  return std::move(emit_result.module);
}

std::vector<std::uint8_t> compile_source_to_bytecode(
    const std::string &path, const std::string &expected_module_name,
    const std::vector<amber::capability::CapabilityRequest> &capabilities =
        {}) {
  const std::string source = read_file(path);
  amber::bytecode::BcModule module = compile_source_text_to_module(
      source, path, expected_module_name, capabilities);
  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(module);
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
                                              module.name,
                                              manifest.manifest.capabilities);
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

struct BuildCliOptions {
  std::string out_dir;
  std::string cache_dir;
  bool cache_enabled = true;
};

BuildCliOptions parse_build_options(int argc, char **argv, int start_index) {
  BuildCliOptions options;
  for (int i = start_index; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out-dir" && i + 1 < argc) {
      options.out_dir = argv[++i];
    } else if (arg == "--cache-dir" && i + 1 < argc) {
      options.cache_dir = argv[++i];
    } else if (arg == "--no-cache") {
      options.cache_enabled = false;
    } else {
      throw std::runtime_error("unknown build option: " + arg);
    }
  }
  return options;
}

std::string profile_material(const amber::build::BuildProfileSet &profiles) {
  std::ostringstream out;
  for (const std::string &feature : profiles.required_features) {
    out << "required=" << feature << "\n";
  }
  for (const std::string &feature : profiles.optional_features) {
    out << "optional=" << feature << "\n";
  }
  for (const std::string &feature : profiles.forbidden_features) {
    out << "forbidden=" << feature << "\n";
  }
  return out.str();
}

struct BuiltStdlibAbi {
  std::string name;
  std::array<std::uint8_t, 32> abi_hash{};
};

bool dependency_exists(const amber::bytecode::BcModule &module,
                       const std::string &name) {
  for (const amber::bytecode::DepEntry &dependency : module.dependencies) {
    if (dependency.module_name_str_id < module.strings.size() &&
        module.strings[dependency.module_name_str_id] == name) {
      return true;
    }
  }
  return false;
}

void add_stdlib_dependencies(amber::bytecode::BcModule *module,
                             const std::vector<BuiltStdlibAbi> &stdlib_abis) {
  for (const BuiltStdlibAbi &stdlib_abi : stdlib_abis) {
    if (dependency_exists(*module, stdlib_abi.name)) {
      continue;
    }
    amber::bytecode::DepEntry dependency;
    dependency.module_name_str_id = ensure_string_id(module, stdlib_abi.name);
    dependency.required_format = {1, 0};
    dependency.min_language_version = {1, 0};
    dependency.has_abi_requirement = true;
    dependency.abi_requirement = stdlib_abi.abi_hash;
    module->dependencies.push_back(dependency);
  }
}

std::string
stdlib_abi_material(const std::vector<BuiltStdlibAbi> &stdlib_abis) {
  std::ostringstream out;
  for (const BuiltStdlibAbi &stdlib_abi : stdlib_abis) {
    out << stdlib_abi.name << "=" << bytes_to_hex(stdlib_abi.abi_hash) << "\n";
  }
  return out.str();
}

std::array<std::uint8_t, 32>
module_abi_hash(const amber::build::BuildModule &module,
                const amber::build::BuildProfileSet &profiles,
                const std::string &source_hash) {
  return sha256_array("amber.abi.v1\n" + module.name + "\n" + module.path +
                      "\n" + source_hash + "\n" + profile_material(profiles));
}

amber::build::BuildArtifactRecord build_one_module(
    const amber::build::BuildModule &module,
    const std::filesystem::path &root_dir, const std::filesystem::path &out_dir,
    const std::filesystem::path &cache_dir,
    const amber::build::BuildProfileSet &profiles,
    const std::vector<BuiltStdlibAbi> &stdlib_abis, bool cache_enabled) {
  const std::filesystem::path source_path = root_dir / module.path;
  const std::string source = read_file(source_path.string());
  const std::string source_hash = amber::lexer::sha256_hex(source);
  const std::string cache_key = amber::lexer::sha256_hex(
      "amber.build.cache.v1\n" + module.name + "\n" + module.path + "\n" +
      source_hash + "\n" + profile_material(profiles) +
      stdlib_abi_material(stdlib_abis));
  const std::filesystem::path output_path =
      out_dir / (safe_artifact_name(module.name) + ".amberbc");
  const std::filesystem::path cache_path =
      cache_dir /
      (safe_artifact_name(module.name) + "-" + cache_key + ".amberbc");

  amber::build::BuildArtifactRecord record;
  record.name = module.name;
  record.path = module.path;
  record.output_path = output_path.string();
  record.cache_path = cache_path.string();
  record.cache_key = cache_key;
  record.source_hash = source_hash;
  record.stdlib = module.stdlib;
  record.bootstrap_layer = module.bootstrap_layer;

  std::vector<std::uint8_t> bytes;
  if (cache_enabled && std::filesystem::exists(cache_path)) {
    bytes = read_bytes(cache_path.string());
    record.cached = true;
  } else {
    amber::bytecode::BcModule bc_module =
        compile_source_text_to_module(source, module.path, module.name);
    bc_module.required_features = profiles.required_features;
    bc_module.optional_features = profiles.optional_features;
    bc_module.forbidden_features = profiles.forbidden_features;
    bc_module.profile_flags = amber::build::profile_flags_for(profiles);
    if (!module.stdlib) {
      add_stdlib_dependencies(&bc_module, stdlib_abis);
    }
    add_module_attr(&bc_module, "amber.build.module", module.name);
    add_module_attr(&bc_module, "amber.build.source_hash", source_hash);
    add_module_attr(&bc_module, "amber.build.cache_key", cache_key);
    if (module.stdlib) {
      add_module_attr(&bc_module, "amber.bootstrap.layer",
                      module.bootstrap_layer.empty() ? "B2"
                                                     : module.bootstrap_layer);
    }
    bc_module.abi_hash = module_abi_hash(module, profiles, source_hash);
    bytes = amber::bytecode::serialize_module(bc_module);
    if (cache_enabled) {
      std::filesystem::create_directories(cache_dir);
      write_bytes(cache_path.string(), bytes);
    }
  }

  amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(bytes);
  if (!decoded.ok()) {
    throw std::runtime_error(
        amber::bytecode::verify_errors_to_json(decoded.errors));
  }
  record.abi_hash = bytes_to_hex(decoded.module.abi_hash);
  record.artifact_hash = amber::lexer::sha256_hex(bytes_to_string(bytes));
  record.byte_size = static_cast<std::uint64_t>(bytes.size());
  std::filesystem::create_directories(out_dir);
  write_bytes(output_path.string(), bytes);
  return record;
}

int run_build_command(int argc, char **argv) {
  if (argc < 3 || std::string(argv[1]) != "build") {
    usage(std::cerr);
    return 2;
  }

  const std::string manifest_path = argv[2];
  const BuildCliOptions options = parse_build_options(argc, argv, 3);
  const std::filesystem::path manifest_dir =
      std::filesystem::path(dirname(manifest_path));
  const std::filesystem::path out_dir =
      options.out_dir.empty() ? (manifest_dir / "build" / "amber")
                              : std::filesystem::path(options.out_dir);
  const std::filesystem::path cache_dir =
      options.cache_dir.empty() ? (out_dir / ".cache")
                                : std::filesystem::path(options.cache_dir);

  amber::build::BuildSummary summary;
  summary.out_dir = out_dir.string();
  summary.cache_dir = cache_dir.string();

  const amber::build::BuildManifestResult parsed =
      amber::build::parse_build_manifest_json(read_file(manifest_path),
                                              manifest_path);
  if (!parsed.ok()) {
    summary.diagnostics = parsed.diagnostics;
    std::cout << amber::build::summary_to_json(summary);
    return 1;
  }

  summary.name = parsed.manifest.name;
  summary.root_module = parsed.manifest.root_module;
  summary.profiles = parsed.manifest.profiles;

  try {
    std::vector<BuiltStdlibAbi> stdlib_abis;
    for (const amber::build::BuildModule &module :
         parsed.manifest.stdlib_modules) {
      amber::build::BuildArtifactRecord artifact =
          build_one_module(module, manifest_dir, out_dir, cache_dir,
                           parsed.manifest.profiles, {}, options.cache_enabled);
      BuiltStdlibAbi abi;
      abi.name = module.name;
      abi.abi_hash = array_from_hex32(artifact.abi_hash);
      stdlib_abis.push_back(std::move(abi));
      summary.artifacts.push_back(std::move(artifact));
    }
    for (const amber::build::BuildModule &module : parsed.manifest.modules) {
      summary.artifacts.push_back(build_one_module(
          module, manifest_dir, out_dir, cache_dir, parsed.manifest.profiles,
          stdlib_abis, options.cache_enabled));
    }
    summary.ok = true;
    std::cout << amber::build::summary_to_json(summary);
    return 0;
  } catch (const std::exception &error) {
    summary.ok = false;
    summary.diagnostics.push_back({"BuildError", error.what(), manifest_path});
    std::cout << amber::build::summary_to_json(summary);
    return 1;
  }
}

int run_capabilities_command(int argc, char **argv) {
  if (argc < 3 || std::string(argv[1]) != "capabilities-check") {
    usage(std::cerr);
    return 2;
  }
  const std::string manifest_source = read_file(argv[2]);
  const amber::pkg::PackageManifestResult manifest =
      amber::pkg::parse_manifest_toml(manifest_source, argv[2]);
  if (!manifest.ok()) {
    std::cerr << package_diagnostics_to_string(manifest.diagnostics);
    return 1;
  }

  std::vector<amber::capability::CapabilityRequest> grants;
  std::vector<amber::capability::CapabilityDiagnostic> diagnostics;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg != "--grant" || i + 1 >= argc) {
      throw std::runtime_error("unknown capabilities option: " + arg);
    }
    amber::capability::CapabilityRequest grant;
    amber::capability::CapabilityDiagnostic diagnostic;
    if (!amber::capability::parse_cli_grant(argv[++i], &grant, &diagnostic)) {
      diagnostics.push_back(std::move(diagnostic));
      continue;
    }
    grants.push_back(std::move(grant));
  }

  amber::capability::CapabilityResolutionResult resolved =
      amber::capability::resolve_capabilities(manifest.manifest.capabilities,
                                              grants);
  resolved.diagnostics.insert(resolved.diagnostics.begin(), diagnostics.begin(),
                              diagnostics.end());
  resolved.ok = resolved.ok && diagnostics.empty();
  std::cout << amber::capability::resolution_to_json(resolved);
  return resolved.ok ? 0 : 1;
}

int run_replay_command(int argc, char **argv) {
  if (argc != 3) {
    usage(std::cerr);
    return 2;
  }
  const std::string command = argv[1];
  const std::string trace_source = read_file(argv[2]);
  amber::replay::ReplayTraceParseResult parsed =
      amber::replay::parse_trace(trace_source);
  if (!parsed.ok()) {
    amber::replay::ReplayValidationResult result;
    result.ok = false;
    result.diagnostics = parsed.diagnostics;
    std::cout << amber::replay::validation_to_json(result);
    return 1;
  }
  if (command == "trace-inspect") {
    std::cout << amber::replay::trace_to_json(parsed.trace);
    return 0;
  }
  if (command == "replay-check") {
    const amber::replay::ReplayValidationResult result =
        amber::replay::validate_trace(parsed.trace);
    std::cout << amber::replay::validation_to_json(result);
    return result.ok ? 0 : 1;
  }
  usage(std::cerr);
  return 2;
}

int run_data_command(int argc, char **argv) {
  if (argc != 3) {
    usage(std::cerr);
    return 2;
  }
  const std::string command = argv[1];
  const std::string source = read_file(argv[2]);
  if (command == "schema-check") {
    const amber::data::SchemaDocumentParseResult parsed =
        amber::data::parse_schema_document(source);
    if (!parsed.ok()) {
      amber::data::SchemaValidationResult result;
      result.ok = false;
      result.diagnostics = parsed.diagnostics;
      std::cout << amber::data::schema_validation_to_json(result);
      return 1;
    }
    const amber::data::SchemaValidationResult result =
        amber::data::validate_schemas(parsed.document.schemas,
                                      parsed.document.migrations,
                                      parsed.document.records);
    std::cout << amber::data::schema_validation_to_json(result);
    return result.ok ? 0 : 1;
  }
  if (command == "table-explain") {
    const amber::data::TablePlanParseResult parsed =
        amber::data::parse_table_plan_document(source);
    if (!parsed.ok()) {
      amber::data::TablePlanValidationResult result;
      result.ok = false;
      result.diagnostics = parsed.diagnostics;
      std::cout << amber::data::table_plan_validation_to_json(result);
      return 1;
    }
    const amber::data::TablePlanValidationResult result =
        amber::data::validate_table_plans(parsed.plans);
    std::cout << amber::data::table_plan_validation_to_json(result);
    return result.ok ? 0 : 1;
  }
  usage(std::cerr);
  return 2;
}

int run_wasm_accel_command(int argc, char **argv) {
  if (argc != 3) {
    usage(std::cerr);
    return 2;
  }
  const std::string command = argv[1];
  const std::string source = read_file(argv[2]);
  const amber::wasm_accel::WasmAccelDocumentParseResult parsed =
      amber::wasm_accel::parse_wasm_accel_document(source);
  if (command == "wasm-build") {
    amber::wasm_accel::WasmComponentValidationResult result =
        amber::wasm_accel::validate_wasm_components(parsed.document.components);
    result.diagnostics.insert(result.diagnostics.begin(),
                              parsed.diagnostics.begin(),
                              parsed.diagnostics.end());
    result.ok = result.ok && parsed.ok();
    std::cout << amber::wasm_accel::wasm_component_validation_to_json(result);
    return result.ok ? 0 : 1;
  }
  if (command == "accel-check") {
    amber::wasm_accel::AcceleratorValidationResult result =
        amber::wasm_accel::validate_accelerator_kernels(
            parsed.document.kernels);
    result.diagnostics.insert(result.diagnostics.begin(),
                              parsed.diagnostics.begin(),
                              parsed.diagnostics.end());
    result.ok = result.ok && parsed.ok();
    std::cout << amber::wasm_accel::accelerator_validation_to_json(result);
    return result.ok ? 0 : 1;
  }
  usage(std::cerr);
  return 2;
}

int run_image_command(int argc, char **argv) {
  const std::string command = argv[1];
  if (command == "image-build" && argc >= 4) {
    const std::string manifest_path = argv[2];
    const std::string out_path = argv[3];
    const amber::pkg::PackageBuildOptions package_options =
        parse_package_build_options(argc, argv, 4);
    const std::string manifest_source = read_file(manifest_path);
    const amber::pkg::PackageManifestResult manifest =
        amber::pkg::parse_manifest_toml(manifest_source, manifest_path);
    if (!manifest.ok()) {
      std::cerr << package_diagnostics_to_string(manifest.diagnostics);
      return 1;
    }

    std::vector<amber::pkg::PackageModuleBlob> modules;
    std::vector<amber::native::NativeModule> native_modules;
    const std::string root_dir = dirname(manifest_path);
    for (const amber::pkg::PackageModule &module : manifest.manifest.modules) {
      CompiledModuleArtifact compiled = compile_source_to_module_artifact(
          join_path(root_dir, module.path), module.name,
          manifest.manifest.capabilities);
      amber::pkg::PackageModuleBlob blob;
      blob.name = module.name;
      blob.path = module.path;
      blob.bytes = std::move(compiled.bytes);
      modules.push_back(std::move(blob));
      native_modules.push_back(std::move(compiled.native_module));
    }

    const amber::pkg::PackageBuildResult package =
        amber::pkg::build_package_artifact(manifest.manifest, modules,
                                           package_options);
    if (!package.ok) {
      std::cerr << package_diagnostics_to_string(package.diagnostics);
      return 1;
    }

    const amber::frozen::FrozenImageBuildResult image =
        amber::frozen::build_frozen_image_artifact(
            package.artifact, package.serialized, native_modules);
    if (!image.ok) {
      std::cerr << frozen_diagnostics_to_string(image.diagnostics);
      return 1;
    }
    write_file(out_path, image.serialized);
    std::cout << amber::frozen::artifact_to_json(image.artifact);
    return 0;
  }

  if (command == "image-inspect" && argc == 3) {
    const std::string serialized = read_file(argv[2]);
    const amber::frozen::FrozenImageParseResult parsed =
        amber::frozen::parse_frozen_image_artifact(serialized, argv[2]);
    if (!parsed.ok()) {
      std::cerr << frozen_diagnostics_to_string(parsed.diagnostics);
      return 1;
    }
    std::cout << amber::frozen::artifact_to_json(parsed.artifact);
    return 0;
  }

  if (command == "image-verify" && argc >= 3) {
    const std::string serialized = read_file(argv[2]);
    const std::string signing_key = parse_package_signing_key(argc, argv, 3);
    const amber::frozen::FrozenImageVerifyResult verified =
        amber::frozen::verify_frozen_image_artifact(serialized, signing_key,
                                                    argv[2]);
    std::cout << amber::frozen::verify_result_to_json(verified);
    return verified.ok ? 0 : 1;
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
    if (argc >= 2 && std::string(argv[1]) == "build") {
      return run_build_command(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]).find("package-") == 0U) {
      return run_package_command(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]).find("capabilities-") == 0U) {
      return run_capabilities_command(argc, argv);
    }
    if (argc >= 2 && (std::string(argv[1]) == "replay-check" ||
                      std::string(argv[1]) == "trace-inspect")) {
      return run_replay_command(argc, argv);
    }
    if (argc >= 2 && (std::string(argv[1]) == "schema-check" ||
                      std::string(argv[1]) == "table-explain")) {
      return run_data_command(argc, argv);
    }
    if (argc >= 2 && (std::string(argv[1]) == "wasm-build" ||
                      std::string(argv[1]) == "accel-check")) {
      return run_wasm_accel_command(argc, argv);
    }
    if (argc >= 2 && (std::string(argv[1]) == "patch-check" ||
                      std::string(argv[1]) == "provenance-audit" ||
                      std::string(argv[1]) == "contract-check" ||
                      std::string(argv[1]) == "privacy-check" ||
                      std::string(argv[1]) == "workflow-check")) {
      return run_modern_document_command(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]).find("image-") == 0U) {
      return run_image_command(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "explain") {
      if (argc != 5 || std::string(argv[3]) != "--span") {
        usage(std::cerr);
        return 2;
      }
      const std::string path = argv[2];
      const std::string source = read_file(path);
      amber::lexer::LexResult lex_result = lex_source(source, path);
      if (!lex_result.ok()) {
        std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
        return 1;
      }
      amber::parser::Parser parser(lex_result.tokens);
      amber::parser::ParseModuleResult parse_result =
          parser.parse_module_unit();
      if (!parse_result.ok()) {
        std::cerr << amber::lexer::diagnostics_to_json(
            parse_result.diagnostics);
        return 1;
      }
      amber::binder::BindResult bind_result = amber::binder::bind_module(
          parse_result.items, parse_result.module_name);
      if (!bind_result.ok()) {
        std::cerr << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
        return 1;
      }
      const std::vector<amber::modern::AgentSymbol> symbols =
          agent_symbols_from_bind_graph(bind_result.graph,
                                        parse_result.module_name);
      const amber::modern::AgentValidationResult validated =
          amber::modern::validate_agent_metadata(symbols);
      std::cout << amber::modern::explain_result_to_json(
          validated, parse_cli_source_location(path, argv[4]));
      return validated.ok ? 0 : 1;
    }
    if (argc != 3 ||
        (std::string(argv[1]) != "lex" && std::string(argv[1]) != "parse" &&
         std::string(argv[1]) != "bind" && std::string(argv[1]) != "typed" &&
         std::string(argv[1]) != "effects-check" &&
         std::string(argv[1]) != "hir" && std::string(argv[1]) != "mir" &&
         std::string(argv[1]) != "mir-dump" &&
         std::string(argv[1]) != "mir-verify" && std::string(argv[1]) != "bc" &&
         std::string(argv[1]) != "native" &&
         std::string(argv[1]) != "native-dump" &&
         std::string(argv[1]) != "native-verify" &&
         std::string(argv[1]) != "bc-disasm" &&
         std::string(argv[1]) != "parse-expr" &&
         std::string(argv[1]) != "symbols" &&
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
        command == "effects-check" || command == "hir" || command == "mir" ||
        command == "mir-dump" || command == "mir-verify" ||
        command == "native" || command == "native-dump" ||
        command == "native-verify" || command == "bc" ||
        command == "bc-disasm" || command == "symbols") {
      amber::parser::ParseModuleResult parse_result =
          parser.parse_module_unit();
      if (!parse_result.ok()) {
        std::cerr << amber::lexer::diagnostics_to_json(
            parse_result.diagnostics);
        return 1;
      }
      if (command == "bind" || command == "typed" ||
          command == "effects-check" || command == "hir" || command == "mir" ||
          command == "mir-dump" || command == "mir-verify" ||
          command == "native" || command == "native-dump" ||
          command == "native-verify" || command == "bc" ||
          command == "bc-disasm" || command == "symbols") {
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
        if (command == "symbols") {
          const std::vector<amber::modern::AgentSymbol> symbols =
              agent_symbols_from_bind_graph(bind_result.graph,
                                            parse_result.module_name);
          const amber::modern::AgentValidationResult validated =
              amber::modern::validate_agent_metadata(symbols);
          std::cout << amber::modern::agent_validation_to_json(validated);
          return validated.ok ? 0 : 1;
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
        if (command == "effects-check") {
          amber::checker::CheckResult check_result =
              amber::checker::check_module(parse_result.items,
                                           parse_result.module_name,
                                           bind_result.graph);
          std::cout << amber::checker::effects_result_to_json(
              check_result, parse_result.module_name);
          return effect_diagnostics_only(check_result).empty() ? 0 : 1;
        }
        amber::checker::CheckResult check_result = amber::checker::check_module(
            parse_result.items, parse_result.module_name, bind_result.graph);
        const std::vector<amber::lexer::Diagnostic> effect_diagnostics =
            effect_diagnostics_only(check_result);
        if (!effect_diagnostics.empty()) {
          std::cerr << amber::lexer::diagnostics_to_json(effect_diagnostics);
          return 1;
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
            emit_result.module.effects = check_result.effect_summaries;
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
          emit_result.module.effects = check_result.effect_summaries;
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
