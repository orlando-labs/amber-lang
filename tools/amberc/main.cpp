#include "buildsys/build.h"
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
#include "runtime/macro_expander.h"
#include "runtime/vm.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

// Identifies the allocator this binary was linked against. The Makefile MALLOC=
// flag defines this; default to the system allocator when built without it.
#ifndef AMBER_ALLOCATOR
#define AMBER_ALLOCATOR "system"
#endif

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
  out << "  amberc <file.am>\n";
  out << "  amberc build <file.am> [-o <path>] [--out-dir <dir>] "
         "[--target native|native-debug|bytecode-wrapper] "
         "[--entry auto|init|main|main-only] [--grant <cap[=target]>...]\n";
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
  out << "  amberc build <amber.build.yaml|amber.build.json> [--out-dir <dir>] "
         "[--cache-dir <dir>] [--target both|native|bytecode] [--no-cache]\n";
  out << "  amberc metadata <file.amberbc> --json\n";
  out << "  amberc verify <file.amberbc> --json\n";
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

std::string native_target_triple() {
#if defined(__x86_64__) || defined(_M_X64)
  const std::string arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  const std::string arch = "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  const std::string arch = "arm";
#elif defined(__i386__) || defined(_M_IX86)
  const std::string arch = "i386";
#else
  const std::string arch = "unknown";
#endif

#if defined(__APPLE__)
  const std::string os = "apple-darwin";
#elif defined(__linux__)
  const std::string os = "unknown-linux";
#elif defined(_WIN32)
  const std::string os = "pc-windows";
#else
  const std::string os = "unknown-unknown";
#endif
  return arch + "-" + os;
}

std::vector<amber::pkg::PackageNativeBlob> collect_native_blobs(
    const std::vector<amber::pkg::PackageNativeExtension> &extensions,
    const std::string &root_dir) {
  std::vector<amber::pkg::PackageNativeBlob> blobs;
  std::set<std::string> seen;
  const auto add_blob = [&](const amber::pkg::PackageNativeExtension &extension,
                            const std::string &kind, const std::string &path) {
    const std::string key = extension.name + "\n" + kind + "\n" + path;
    if (!seen.insert(key).second) {
      return;
    }
    amber::pkg::PackageNativeBlob blob;
    blob.extension_name = extension.name;
    blob.kind = kind;
    blob.path = path;
    blob.bytes = read_bytes(join_path(root_dir, path));
    blobs.push_back(std::move(blob));
  };
  for (const amber::pkg::PackageNativeExtension &extension : extensions) {
    for (const std::string &source : extension.sources) {
      add_blob(extension, "source", source);
    }
    for (const std::string &header : extension.headers) {
      add_blob(extension, "header", header);
    }
  }
  return blobs;
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

// Temporary declaration for the macro provider pre-pass; the real macro driver
// cleanup can move this once the staging code settles.
std::vector<amber::macros::MacroExport>
harvest_macro_exports(const std::string &source, const std::string &path);

CompiledModuleArtifact compile_source_to_module_artifact(
    const std::string &path, const std::string &expected_module_name,
    const std::vector<amber::capability::CapabilityRequest> &capabilities = {},
    const amber::macros::MacroProviderMap *macro_providers = nullptr) {
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

  // F1.5: expand macro calls, then quote/unquote, before binding. Package /
  // image builds pass the staged provider table (§11 cross-module macros).
  {
    const amber::macros::ExpandResult macro_result =
        macro_providers != nullptr
            ? amber::macros::expand_macros(parse_result.items,
                                           parse_result.module_name, source,
                                           *macro_providers)
            : amber::macros::expand_macros(parse_result.items,
                                           parse_result.module_name, source);
    if (!macro_result.ok) {
      throw std::runtime_error("macro expansion error: " + macro_result.error);
    }
  }
  amber::ast::expand_quotes(parse_result.items);

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
    const std::vector<amber::capability::CapabilityRequest> &capabilities = {},
    const amber::macros::MacroProviderMap *macro_providers = nullptr) {
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

  // F1.5: expand macro calls, then quote/unquote, before binding. Manifest
  // builds pass the staged provider table (§11 cross-module macros).
  {
    const amber::macros::ExpandResult macro_result =
        macro_providers != nullptr
            ? amber::macros::expand_macros(parse_result.items,
                                           parse_result.module_name, source,
                                           *macro_providers)
            : amber::macros::expand_macros(parse_result.items,
                                           parse_result.module_name, source);
    if (!macro_result.ok) {
      throw std::runtime_error("macro expansion error: " + macro_result.error);
    }
  }
  amber::ast::expand_quotes(parse_result.items);

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
    const std::vector<amber::capability::CapabilityRequest> &capabilities = {},
    const amber::macros::MacroProviderMap *macro_providers = nullptr) {
  const std::string source = read_file(path);
  amber::bytecode::BcModule module = compile_source_text_to_module(
      source, path, expected_module_name, capabilities, macro_providers);
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

bool has_suffix(const std::string &value, const std::string &suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

bool is_amber_source_path(const std::string &path) {
  return has_suffix(path, ".am");
}

std::string synthetic_module_name_for_path(const std::string &path) {
  const std::string stem = std::filesystem::path(path).stem().string();
  return "amber.entry." + safe_artifact_name(stem.empty() ? "main" : stem);
}

std::string shell_single_quote(const std::string &value) {
  std::string out = "'";
  for (const char c : value) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out += "'";
  return out;
}

std::string executable_amberc_ref(const std::string &argv0) {
  if (argv0.find('/') == std::string::npos &&
      argv0.find('\\') == std::string::npos) {
    return "amberc";
  }
  try {
    return std::filesystem::absolute(argv0).lexically_normal().string();
  } catch (const std::exception &) {
    return argv0;
  }
}

std::string bytes_to_hex_text(const std::vector<std::uint8_t> &bytes) {
  std::string out;
  out.reserve(bytes.size() * 2U);
  const char *hex = "0123456789abcdef";
  for (const std::uint8_t byte : bytes) {
    out.push_back(hex[(byte >> 4U) & 0x0FU]);
    out.push_back(hex[byte & 0x0FU]);
  }
  return out;
}

std::vector<std::uint8_t> hex_text_to_bytes(const std::string &hex) {
  std::vector<std::uint8_t> bytes;
  int high = -1;
  for (const char c : hex) {
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
      continue;
    }
    const int digit = from_hex_digit(c);
    if (digit < 0) {
      throw std::runtime_error("invalid embedded executable hex payload");
    }
    if (high < 0) {
      high = digit;
    } else {
      bytes.push_back(static_cast<std::uint8_t>((high << 4) | digit));
      high = -1;
    }
  }
  if (high >= 0) {
    throw std::runtime_error("odd-length embedded executable hex payload");
  }
  return bytes;
}

std::string string_to_hex_text(const std::string &value) {
  return bytes_to_hex_text(
      std::vector<std::uint8_t>(value.begin(), value.end()));
}

std::string hex_text_to_string(const std::string &hex) {
  const std::vector<std::uint8_t> bytes = hex_text_to_bytes(hex);
  return std::string(bytes.begin(), bytes.end());
}

const amber::bytecode::BcMethod *
zero_arg_method_by_name(const amber::bytecode::BcModule &module,
                        const std::string &name) {
  for (const amber::bytecode::BcMethod &method : module.methods) {
    if (method.selector_sym_id < module.symbols.size() &&
        module.symbols[method.selector_sym_id] == name &&
        method.params.empty() &&
        (method.flags & (amber::bytecode::kMethodFlagInstance |
                         amber::bytecode::kMethodFlagClass |
                         amber::bytecode::kMethodFlagPropertyGetter |
                         amber::bytecode::kMethodFlagPropertySetter)) == 0U) {
      return &method;
    }
  }
  return nullptr;
}

enum class EntryExecutionMode { Init, MainAfterInit, MainOnly };

std::string entry_mode_name(EntryExecutionMode mode) {
  switch (mode) {
  case EntryExecutionMode::Init:
    return "init";
  case EntryExecutionMode::MainAfterInit:
    return "main";
  case EntryExecutionMode::MainOnly:
    return "main-only";
  }
  return "init";
}

EntryExecutionMode parse_entry_mode(const std::string &mode) {
  if (mode == "init") {
    return EntryExecutionMode::Init;
  }
  if (mode == "main") {
    return EntryExecutionMode::MainAfterInit;
  }
  if (mode == "main-only") {
    return EntryExecutionMode::MainOnly;
  }
  throw std::runtime_error("unknown embedded executable entry mode: " + mode);
}

EntryExecutionMode parse_source_build_entry_mode(const std::string &mode) {
  if (mode == "auto") {
    throw std::runtime_error("internal auto entry mode should be resolved");
  }
  if (mode == "init") {
    return EntryExecutionMode::Init;
  }
  if (mode == "main") {
    return EntryExecutionMode::MainAfterInit;
  }
  if (mode == "main-only") {
    return EntryExecutionMode::MainOnly;
  }
  throw std::runtime_error("unknown source build entry mode: " + mode);
}

EntryExecutionMode
default_entry_mode_for(bool has_package,
                       const amber::bytecode::BcModule &module) {
  if (has_package && zero_arg_method_by_name(module, "main") != nullptr) {
    return EntryExecutionMode::MainAfterInit;
  }
  return EntryExecutionMode::Init;
}

struct RunnableModuleArtifact {
  std::string module_name;
  EntryExecutionMode entry_mode = EntryExecutionMode::Init;
  std::vector<std::uint8_t> bytes;
  amber::bytecode::BcModule module;
  std::vector<amber::capability::CapabilityRequest> capability_grants;
  bool has_entry_init_code_id = false;
  std::uint32_t entry_init_code_id = 0;
  bool has_entry_main_code_id = false;
  std::uint32_t entry_main_code_id = 0;
  bool whole_graph_native = false;
  std::size_t graph_module_count = 1;
};

RunnableModuleArtifact compile_source_to_runnable_module(
    const std::string &path,
    const std::optional<EntryExecutionMode> forced_entry_mode = std::nullopt,
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

  // F1.5: expand macro calls, then quote/unquote, before binding.
  {
    const amber::macros::ExpandResult macro_result =
        amber::macros::expand_macros(parse_result.items,
                                     parse_result.module_name, source);
    if (!macro_result.ok) {
      throw std::runtime_error("macro expansion error: " + macro_result.error);
    }
  }
  amber::ast::expand_quotes(parse_result.items);

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

  const bool has_package = !parse_result.module_name.empty();
  const std::string module_name = has_package
                                      ? parse_result.module_name
                                      : synthetic_module_name_for_path(path);
  const EntryExecutionMode entry_mode = forced_entry_mode.value_or(
      default_entry_mode_for(has_package, emit_result.module));
  add_module_attr(&emit_result.module, "amber.entry.module", module_name);
  add_module_attr(&emit_result.module, "amber.entry.source", path);
  add_module_attr(&emit_result.module, "amber.entry.mode",
                  entry_mode_name(entry_mode));

  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(emit_result.module);
  amber::bytecode::DecodeResult decode_result =
      amber::bytecode::deserialize_module(bytes);
  if (!decode_result.ok()) {
    throw std::runtime_error(
        amber::bytecode::verify_errors_to_json(decode_result.errors));
  }

  RunnableModuleArtifact artifact;
  artifact.module_name = module_name;
  artifact.entry_mode = entry_mode;
  artifact.bytes = bytes;
  artifact.module = std::move(decode_result.module);
  artifact.capability_grants = capabilities;
  if (artifact.module.init.has_entry_code_id) {
    artifact.has_entry_init_code_id = true;
    artifact.entry_init_code_id = artifact.module.init.entry_code_id;
  }
  if (const amber::bytecode::BcMethod *main_method =
          zero_arg_method_by_name(artifact.module, "main")) {
    artifact.has_entry_main_code_id = true;
    artifact.entry_main_code_id = main_method->entry_code_id;
  }
  return artifact;
}

// Current resident set size of this process in bytes, or 0 if unavailable.
std::size_t current_rss_bytes() {
#if defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    return static_cast<std::size_t>(info.resident_size);
  }
  return 0;
#elif defined(__linux__)
  std::ifstream statm("/proc/self/statm");
  std::size_t pages_total = 0;
  std::size_t pages_resident = 0;
  if (statm >> pages_total >> pages_resident) {
    return pages_resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  }
  return 0;
#else
  return 0;
#endif
}

// Peak resident set size of this process in bytes, or 0 if unavailable. This is
// the kernel's high-water mark, so it captures the churn peak regardless of
// when objects were freed -- the most comparable number across allocator
// flavors.
std::size_t peak_rss_bytes() {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0;
  }
#if defined(__APPLE__)
  // Darwin reports ru_maxrss in bytes.
  return static_cast<std::size_t>(usage.ru_maxrss);
#else
  // Linux and the BSDs report ru_maxrss in kilobytes.
  return static_cast<std::size_t>(usage.ru_maxrss) * 1024u;
#endif
}

// When AMBER_HEAP_STATS is set to a non-empty, non-"0" value, print a single
// machine-parseable line of heap accounting (§9 measurement plan): the linked
// allocator, process RSS, VM-tracked live/shell bytes, and the fragmentation
// ratio (RSS / live heap bytes). Written to stderr so it never pollutes the
// program's stdout value.
void maybe_dump_heap_stats(const amber::runtime::RuntimeWorld &world,
                           const amber::bytecode::BcModule &module,
                           const amber::runtime::ExecutionResult *result) {
  const char *flag = std::getenv("AMBER_HEAP_STATS");
  if (flag == nullptr || flag[0] == '\0' || flag[0] == '0') {
    return;
  }
  const amber::runtime::RuntimeHeapStats stats = world.heap_stats();
  const std::size_t rss = current_rss_bytes();
  const std::size_t peak_rss = peak_rss_bytes();
  // Runtime string-table growth (the unbounded retention no allocator fixes --
  // RESEARCH §3.3/§7.1, DESIGN-string-table-lifecycle Option D). result holds
  // the full table only when it grew; the slots past the module's compile-time
  // count are the runtime-interned strings, which are never reclaimed today.
  std::size_t runtime_string_count = 0;
  std::size_t runtime_string_bytes = 0;
  if (result != nullptr &&
      result->runtime_strings.size() > module.strings.size()) {
    for (std::size_t i = module.strings.size();
         i < result->runtime_strings.size(); ++i) {
      runtime_string_bytes += result->runtime_strings[i].size();
    }
    runtime_string_count =
        result->runtime_strings.size() - module.strings.size();
  }
  const double frag_live =
      stats.live_object_bytes > 0
          ? static_cast<double>(rss) /
                static_cast<double>(stats.live_object_bytes)
          : 0.0;
  const double frag_tracked =
      stats.tracked_object_bytes > 0
          ? static_cast<double>(rss) /
                static_cast<double>(stats.tracked_object_bytes)
          : 0.0;
  std::fprintf(
      stderr,
      "[amber-heap] allocator=%s rss_bytes=%zu peak_rss_bytes=%zu "
      "live_object_bytes=%llu "
      "tracked_object_bytes=%llu live_objects=%llu allocations=%llu "
      "local_frees=%llu remote_frees_queued=%llu remote_queue_depth=%llu "
      "gc_cycles=%llu gc_reclaimed_objects=%llu runtime_string_count=%llu "
      "runtime_string_bytes=%llu frag_ratio_live=%.2f "
      "frag_ratio_tracked=%.2f\n",
      AMBER_ALLOCATOR, rss, peak_rss,
      static_cast<unsigned long long>(stats.live_object_bytes),
      static_cast<unsigned long long>(stats.tracked_object_bytes),
      static_cast<unsigned long long>(stats.live_objects),
      static_cast<unsigned long long>(stats.allocations),
      static_cast<unsigned long long>(stats.local_frees),
      static_cast<unsigned long long>(stats.remote_frees_queued),
      static_cast<unsigned long long>(stats.remote_queue_depth),
      static_cast<unsigned long long>(stats.gc_cycles),
      static_cast<unsigned long long>(stats.gc_reclaimed_objects),
      static_cast<unsigned long long>(runtime_string_count),
      static_cast<unsigned long long>(runtime_string_bytes), frag_live,
      frag_tracked);
}

amber::runtime::ExecutionResult
execute_runnable_module(const amber::bytecode::BcModule &module,
                        EntryExecutionMode mode) {
  amber::runtime::RuntimeWorld world(module);
  amber::runtime::ExecutionResult init_result;
  if (mode != EntryExecutionMode::MainOnly && module.init.has_entry_code_id) {
    init_result = world.execute(module.init.entry_code_id);
    if (!init_result.ok()) {
      maybe_dump_heap_stats(world, module, &init_result);
      return init_result;
    }
  }

  if (mode == EntryExecutionMode::MainAfterInit ||
      mode == EntryExecutionMode::MainOnly) {
    const amber::bytecode::BcMethod *main_method =
        zero_arg_method_by_name(module, "main");
    if (main_method == nullptr) {
      maybe_dump_heap_stats(world, module, nullptr);
      return {amber::runtime::Value::null(),
              amber::runtime::Fault{
                  "EntryError",
                  "entry mode requires a zero-argument main() method", 0, 0}};
    }
    const amber::runtime::ExecutionResult result =
        world.execute(main_method->entry_code_id);
    maybe_dump_heap_stats(world, module, &result);
    return result;
  }

  maybe_dump_heap_stats(world, module, &init_result);
  return init_result;
}

void print_execution_fault(const amber::runtime::ExecutionResult &result) {
  if (!result.fault.has_value()) {
    return;
  }
  std::cerr << result.fault->error_name << ": " << result.fault->message
            << "\n";
  if (!result.fault->trace_text.empty()) {
    std::cerr << result.fault->trace_text;
    if (!has_suffix(result.fault->trace_text, "\n")) {
      std::cerr << "\n";
    }
  }
}

bool should_print_run_value(const amber::runtime::Value &value) {
  return !value.is_null() && !value.is_closure();
}

int run_runnable_module(const std::string &module_name,
                        EntryExecutionMode entry_mode,
                        const std::vector<std::uint8_t> &bytes) {
  (void)module_name;
  amber::bytecode::DecodeResult decode_result =
      amber::bytecode::deserialize_module(bytes);
  if (!decode_result.ok()) {
    std::cerr << amber::bytecode::verify_errors_to_json(decode_result.errors);
    return 1;
  }
  const amber::runtime::ExecutionResult result =
      execute_runnable_module(decode_result.module, entry_mode);
  if (!result.ok()) {
    print_execution_fault(result);
    return 1;
  }
  if (should_print_run_value(result.value)) {
    std::cout << amber::runtime::value_to_debug_string(
                     result.value, &decode_result.module,
                     &result.runtime_strings, &result.runtime_symbols)
              << "\n";
  }
  return 0;
}

int run_source_file_command(const std::string &path) {
  const RunnableModuleArtifact artifact =
      compile_source_to_runnable_module(path);
  return run_runnable_module(artifact.module_name, artifact.entry_mode,
                             artifact.bytes);
}

std::string render_executable_script(const std::string &amberc_ref,
                                     const RunnableModuleArtifact &artifact) {
  const std::string quoted_amberc = shell_single_quote(amberc_ref);
  std::ostringstream out;
  out << "#!/bin/sh\n";
  out << "set -e\n";
  out << "if [ -n \"${AMBERC:-}\" ]; then\n";
  out << "  exec \"$AMBERC\" run-embedded \"$0\" \"$@\"\n";
  out << "fi\n";
  out << "if [ -x " << quoted_amberc << " ]; then\n";
  out << "  exec " << quoted_amberc << " run-embedded \"$0\" \"$@\"\n";
  out << "fi\n";
  out << "exec amberc run-embedded \"$0\" \"$@\"\n";
  out << "__AMBER_EXECUTABLE_V1__\n";
  out << "module " << string_to_hex_text(artifact.module_name) << "\n";
  out << "mode " << entry_mode_name(artifact.entry_mode) << "\n";
  out << "bytecode\n";
  const std::string hex = bytes_to_hex_text(artifact.bytes);
  for (std::size_t i = 0; i < hex.size(); i += 80U) {
    out << hex.substr(i, 80U) << "\n";
  }
  out << "__END_AMBER_EXECUTABLE_V1__\n";
  return out.str();
}

struct NativeCoverageRecord {
  std::uint32_t code_id = 0;
  std::string code_kind;
  std::string mode;
  std::string reason;
};

struct NativeCppNumericProfile {
  std::string int_type = "Int64";
  std::string overflow = "checked";
  amber::runtime::NumericPolicy policy;
  bool supported = true;
  std::string reason;
};

struct NativeCppBuildPlan {
  std::string source;
  std::set<std::uint32_t> native_code_ids;
  std::set<std::uint32_t> vm_callable_code_ids;
  std::vector<NativeCoverageRecord> coverage;
  std::string fallback_reason;
  std::string backend = "cpp-bytecode-direct-v1";
  std::string numeric_int_type = "Int64";
  std::string numeric_overflow = "checked";
  amber::runtime::NumericPolicy numeric_policy;
  bool entry_native = false;
  bool uses_bytecode_fallback = true;
};

struct NativeExecutableBuildResult {
  std::string output_path;
  std::string source_path;
  std::string hash;
  std::string backend;
  std::string cxx;
  std::size_t native_code_count = 0;
  std::size_t vm_callable_code_count = 0;
  std::size_t fallback_code_count = 0;
  std::size_t total_code_count = 0;
  bool entry_native = false;
  bool full_native_coverage = false;
  bool uses_bytecode_fallback = true;
  std::string fallback_reason;
  std::vector<NativeCoverageRecord> coverage;
};

bool operand_u32_value(const amber::bytecode::Instruction &instruction,
                       std::size_t index, std::uint32_t *out) {
  if (index >= instruction.operands.size()) {
    return false;
  }
  const amber::bytecode::InstructionOperand &operand =
      instruction.operands[index];
  if (operand.value < 0 || static_cast<std::uint64_t>(operand.value) >
                               static_cast<std::uint64_t>(
                                   std::numeric_limits<std::uint32_t>::max())) {
    return false;
  }
  *out = static_cast<std::uint32_t>(operand.value);
  return true;
}

bool operand_is_no_block(const amber::bytecode::Instruction &instruction,
                         std::size_t index) {
  if (index >= instruction.operands.size()) {
    return false;
  }
  const std::int64_t value = instruction.operands[index].value;
  return value < 0 || value == static_cast<std::int64_t>(
                                   std::numeric_limits<std::uint32_t>::max());
}

std::string native_cpp_function_name(std::uint32_t code_id) {
  return "amber_native_c" + std::to_string(code_id);
}

std::string cpp_decimal_i64(std::int64_t value) {
  return "static_cast<std::int64_t>(" + std::to_string(value) + "LL)";
}

std::string cpp_native_i64_expr(std::int64_t value) {
  if (value == std::numeric_limits<std::int64_t>::min()) {
    return "std::numeric_limits<std::int64_t>::min()";
  }
  if (value == std::numeric_limits<std::int64_t>::max()) {
    return "std::numeric_limits<std::int64_t>::max()";
  }
  return "static_cast<std::int64_t>(" + std::to_string(value) + "LL)";
}

std::string
native_cpp_numeric_mode_expr(amber::runtime::NumericOverflowMode mode) {
  switch (mode) {
  case amber::runtime::NumericOverflowMode::Wrapping:
    return "NativeNumericOverflowMode::Wrapping";
  case amber::runtime::NumericOverflowMode::Saturating:
    return "NativeNumericOverflowMode::Saturating";
  case amber::runtime::NumericOverflowMode::Checked:
  default:
    return "NativeNumericOverflowMode::Checked";
  }
}

NativeCppNumericProfile
native_cpp_numeric_profile(const amber::bytecode::BcModule &module) {
  NativeCppNumericProfile profile;
  for (const amber::bytecode::AttrEntry &attr : module.attrs) {
    const auto attr_text = [&](std::uint32_t id) -> std::string {
      return id < module.strings.size() ? module.strings[id] : std::string{};
    };
    const std::string key = attr_text(attr.key_str_id);
    if (key == "amber.numeric.int") {
      profile.int_type = attr_text(attr.value_str_id);
    } else if (key == "amber.numeric.overflow") {
      profile.overflow = attr_text(attr.value_str_id);
    }
  }
  if (profile.int_type.empty()) {
    profile.int_type = "Int64";
  }
  if (profile.overflow.empty()) {
    profile.overflow = "checked";
  }
  const std::optional<amber::runtime::NumericPolicy> policy =
      amber::runtime::numeric_policy_for(profile.int_type, profile.overflow);
  if (!policy.has_value()) {
    profile.supported = false;
    profile.reason = "module numeric profile is not supported by native "
                     "backend (int: " +
                     profile.int_type + ", overflow: " + profile.overflow + ")";
    return profile;
  }
  profile.policy = *policy;
  return profile;
}

bool native_cpp_scalar_selector(const amber::bytecode::BcModule &module,
                                std::uint32_t symbol_id,
                                std::string *selector) {
  if (symbol_id >= module.symbols.size()) {
    return false;
  }
  *selector = module.symbols[symbol_id];
  return *selector == "+" || *selector == "-" || *selector == "*" ||
         *selector == "/" || *selector == "%" || *selector == "//" ||
         *selector == "**" || *selector == "<" || *selector == ">" ||
         *selector == "<=" || *selector == ">=" || *selector == "==" ||
         *selector == "!=" || *selector == "=~" || *selector == "!~" ||
         *selector == "<=>" || *selector == "&" || *selector == "|" ||
         *selector == "^" || *selector == "<<" || *selector == ">>";
}

bool native_cpp_scalar_nullary_selector(const std::string &selector) {
  return selector == "u+" || selector == "u-" || selector == "abs" ||
         selector == "class";
}

bool native_cpp_scalar_conversion_selector(const std::string &selector) {
  return selector == "to_str" || selector == "to_int" ||
         selector == "to_float" || selector == "to_bool" ||
         selector == "to_symbol";
}

bool native_cpp_collection_selector(const std::string &selector,
                                    std::uint32_t pos_count) {
  return ((selector == "[]" || selector == "[]?" || selector == "has_index?") &&
          pos_count == 1U) ||
         (selector == "[]=" && pos_count == 2U) ||
         ((selector == "each" || selector == "map" || selector == "select" ||
           selector == "reject" || selector == "find" || selector == "any?" ||
           selector == "all?" || selector == "none?" ||
           selector == "transform_keys" || selector == "transform_values") &&
          pos_count == 0U) ||
         (selector == "reduce" && (pos_count == 0U || pos_count == 1U)) ||
         ((selector == "count" || selector == "length" || selector == "size" ||
           selector == "empty?" || selector == "bytesize" ||
           selector == "deconstruct" || selector == "to_array" ||
           selector == "keys" || selector == "values" ||
           selector == "entries" || selector == "reversed" ||
           selector == "sorted" || selector == "min" || selector == "max" ||
           selector == "minmax" || selector == "init" || selector == "tail") &&
          pos_count == 0U) ||
         (selector == "first" && (pos_count == 0U || pos_count == 1U)) ||
         (selector == "bytes" && pos_count == 0U) ||
         (selector == "appended" && pos_count >= 1U) ||
         (selector == "inserted" && pos_count >= 2U) ||
         (selector == "deleted" && pos_count == 1U) ||
         ((selector == "take" || selector == "drop") && pos_count == 1U) ||
         ((selector == "contains?" || selector == "includes?" ||
           selector == "include?" || selector == "member?" ||
           selector == "has_key?" || selector == "key?" ||
           selector == "value?" || selector == "has_value?" ||
           selector == "starts_with?" || selector == "ends_with?" ||
           selector == "split") &&
          pos_count == 1U) ||
         ((selector == "union" || selector == "intersection" ||
           selector == "difference" || selector == "left_difference" ||
           selector == "symmetric_difference" || selector == "subset?" ||
           selector == "proper_subset?" || selector == "superset?" ||
           selector == "proper_superset?" || selector == "disjoint?" ||
           selector == "added" || selector == "add!" ||
           selector == "subtract!") &&
          pos_count == 1U) ||
         (selector == "slice" && (pos_count == 1U || pos_count == 2U)) ||
         (selector == "with" && pos_count == 2U) ||
         ((selector == "without" && pos_count == 1U) ||
          (selector == "except" && pos_count >= 1U)) ||
         (selector == "merge" && pos_count == 1U) ||
         (selector == "compact" && pos_count == 0U) ||
         ((selector == "replace" || selector == "replaced") &&
          pos_count == 2U) ||
         ((selector == "upcase" || selector == "downcase" ||
           selector == "trim" || selector == "strip" || selector == "reverse" ||
           selector == "chars") &&
          pos_count == 0U) ||
         (selector == "concat" && pos_count == 1U);
}

bool native_cpp_time_unit_selector(const std::string &selector) {
  return selector == "nanosecond" || selector == "nanoseconds" ||
         selector == "microsecond" || selector == "microseconds" ||
         selector == "millisecond" || selector == "milliseconds" ||
         selector == "second" || selector == "seconds" ||
         selector == "minute" || selector == "minutes" || selector == "hour" ||
         selector == "hours" || selector == "day" || selector == "days" ||
         selector == "week" || selector == "weeks" || selector == "month" ||
         selector == "months" || selector == "year" || selector == "years";
}

bool native_cpp_time_nullary_selector(const std::string &selector) {
  return native_cpp_time_unit_selector(selector) || selector == "iso8601" ||
         selector == "to_str" || selector == "inspect" ||
         selector == "unix_seconds" || selector == "unix_milliseconds" ||
         selector == "unix_nanoseconds" || selector == "year" ||
         selector == "month" || selector == "day" || selector == "hour" ||
         selector == "minute" || selector == "second" ||
         selector == "nanosecond" || selector == "weekday" ||
         selector == "yearday" || selector == "months" || selector == "days" ||
         selector == "nanoseconds" || selector == "fixed?" ||
         selector == "total_nanoseconds";
}

std::string native_cpp_time_selector_enum(const std::string &selector) {
  if (selector == "nanosecond")
    return "NativeTimeSelector::Nanosecond";
  if (selector == "nanoseconds")
    return "NativeTimeSelector::Nanoseconds";
  if (selector == "microsecond")
    return "NativeTimeSelector::Microsecond";
  if (selector == "microseconds")
    return "NativeTimeSelector::Microseconds";
  if (selector == "millisecond")
    return "NativeTimeSelector::Millisecond";
  if (selector == "milliseconds")
    return "NativeTimeSelector::Milliseconds";
  if (selector == "second")
    return "NativeTimeSelector::Second";
  if (selector == "seconds")
    return "NativeTimeSelector::Seconds";
  if (selector == "minute")
    return "NativeTimeSelector::Minute";
  if (selector == "minutes")
    return "NativeTimeSelector::Minutes";
  if (selector == "hour")
    return "NativeTimeSelector::Hour";
  if (selector == "hours")
    return "NativeTimeSelector::Hours";
  if (selector == "day")
    return "NativeTimeSelector::Day";
  if (selector == "days")
    return "NativeTimeSelector::Days";
  if (selector == "week")
    return "NativeTimeSelector::Week";
  if (selector == "weeks")
    return "NativeTimeSelector::Weeks";
  if (selector == "month")
    return "NativeTimeSelector::Month";
  if (selector == "months")
    return "NativeTimeSelector::Months";
  if (selector == "year")
    return "NativeTimeSelector::Year";
  if (selector == "years")
    return "NativeTimeSelector::Years";
  if (selector == "iso8601")
    return "NativeTimeSelector::Iso8601";
  if (selector == "to_str")
    return "NativeTimeSelector::ToStr";
  if (selector == "inspect")
    return "NativeTimeSelector::Inspect";
  if (selector == "unix_seconds")
    return "NativeTimeSelector::UnixSeconds";
  if (selector == "unix_milliseconds") {
    return "NativeTimeSelector::UnixMilliseconds";
  }
  if (selector == "unix_nanoseconds") {
    return "NativeTimeSelector::UnixNanoseconds";
  }
  if (selector == "weekday")
    return "NativeTimeSelector::Weekday";
  if (selector == "yearday")
    return "NativeTimeSelector::Yearday";
  if (selector == "fixed?")
    return "NativeTimeSelector::FixedPredicate";
  if (selector == "total_nanoseconds") {
    return "NativeTimeSelector::TotalNanoseconds";
  }
  throw std::logic_error("unsupported native Time selector: " + selector);
}

bool native_cpp_uuid_nullary_selector(const std::string &selector) {
  return selector == "to_str" || selector == "inspect" || selector == "version";
}

bool native_cpp_regexp_selector(const std::string &selector,
                                std::uint32_t pos_count) {
  return ((selector == "compile" || selector == "new" || selector == "r" ||
           selector == "__call__" || selector == "escape" ||
           selector == "match" || selector == "find" || selector == "match?" ||
           selector == "matches?" || selector == "full_match" ||
           selector == "full_match?" || selector == "=~" || selector == "!~" ||
           selector == "[]" || selector == "group") &&
          pos_count == 1U) ||
         ((selector == "source" || selector == "to_str" ||
           selector == "inspect" || selector == "text" ||
           selector == "pattern" || selector == "count" || selector == "size" ||
           selector == "length" || selector == "start" ||
           selector == "finish" || selector == "end" ||
           selector == "captures") &&
          pos_count == 0U);
}

bool native_cpp_url_selector(const std::string &selector,
                             std::uint32_t pos_count) {
  return (selector == "parse" || selector == "build" ||
          selector == "percent_encode" || selector == "percent_decode" ||
          selector == "parse_query" || selector == "build_query") &&
         pos_count == 1U;
}

bool native_cpp_benchmark_selector(const std::string &selector,
                                   std::uint32_t pos_count, bool has_block) {
  if ((selector == "time" || selector == "measure" || selector == "profile" ||
       selector == "run") &&
      pos_count <= 1U) {
    return has_block;
  }
  if (selector == "section" && pos_count == 1U) {
    return has_block;
  }
  if (has_block) {
    return false;
  }
  if (selector == "compare") {
    return pos_count >= 1U;
  }
  if (selector == "format" || selector == "table" || selector == "pretty" ||
      selector == "to_map" || selector == "to_json" || selector == "map") {
    return pos_count <= 1U;
  }
  if (selector == "find") {
    return pos_count == 1U;
  }
  if (selector == "label" || selector == "kind" || selector == "data" ||
      selector == "value" || selector == "elapsed" ||
      selector == "elapsed_ns" || selector == "iterations" ||
      selector == "samples" || selector == "per_iteration" ||
      selector == "per_iteration_ns" || selector == "mean" ||
      selector == "mean_ns" || selector == "min" || selector == "min_ns" ||
      selector == "max" || selector == "max_ns" || selector == "p50" ||
      selector == "p50_ns" || selector == "p90" || selector == "p90_ns" ||
      selector == "p95" || selector == "p95_ns" || selector == "p99" ||
      selector == "p99_ns" || selector == "ops_per_second" ||
      selector == "sample_times" || selector == "sample_ns" ||
      selector == "cases" || selector == "fastest" || selector == "slowest" ||
      selector == "relative" || selector == "total" || selector == "total_ns" ||
      selector == "spans" || selector == "summary" || selector == "inspect") {
    return pos_count == 0U;
  }
  return (selector == "from_map" || selector == "from_json") && pos_count == 1U;
}

bool native_cpp_math_selector(const std::string &selector,
                              std::uint32_t pos_count) {
  return ((selector == "PI" || selector == "E") && pos_count == 0U) ||
         ((selector == "abs" || selector == "sign" || selector == "sqrt" ||
           selector == "cbrt" || selector == "exp" || selector == "log2" ||
           selector == "log10" || selector == "sin" || selector == "cos" ||
           selector == "tan" || selector == "asin" || selector == "acos" ||
           selector == "atan" || selector == "floor" || selector == "ceil" ||
           selector == "round" || selector == "trunc") &&
          pos_count == 1U) ||
         ((selector == "min" || selector == "max" || selector == "pow" ||
           selector == "hypot" || selector == "atan2") &&
          pos_count == 2U);
}

// Eligibility allowlist for the cpp-bytecode-direct backend.
//
// INVARIANT (amber.native-backend-equivalence.v1): every opcode/selector
// admitted here must be observably side-effect-free. The native lane handles
// anything it cannot execute (including checked-Int overflow) by throwing
// NativeBailout and re-running the WHOLE program under the VM; that restart
// is only sound while eligible code has produced no observable effects.
// Do not admit output/IO/channel/shared-state selectors here. Code objects
// rejected by this allowlist may still avoid the whole-program restart via
// the per-function scalar VM bridge (`native_cpp_code_vm_callable` below),
// which has its own effect-free constraint. `make backend-equivalence`
// asserts the observable half of these invariants over corpus/run.
bool native_cpp_code_supported(const amber::bytecode::BcModule &module,
                               const amber::bytecode::BcCode &code,
                               std::string *reason) {
  if (!code.handler_table.empty()) {
    *reason = "exceptions still use VM fallback";
    return false;
  }
  for (std::size_t pc = 0; pc < code.instructions.size(); ++pc) {
    const amber::bytecode::Instruction &instruction = code.instructions[pc];
    using amber::bytecode::Opcode;
    const bool integer_k_opcode = instruction.opcode == Opcode::IAddK ||
                                  instruction.opcode == Opcode::ISubK ||
                                  instruction.opcode == Opcode::ILtK ||
                                  instruction.opcode == Opcode::IGtK ||
                                  instruction.opcode == Opcode::IMulK ||
                                  instruction.opcode == Opcode::IDivK ||
                                  instruction.opcode == Opcode::IModK ||
                                  instruction.opcode == Opcode::IFloorDivK ||
                                  instruction.opcode == Opcode::ILeK ||
                                  instruction.opcode == Opcode::IGeK ||
                                  instruction.opcode == Opcode::IEqK ||
                                  instruction.opcode == Opcode::INeK ||
                                  instruction.opcode == Opcode::ICmpK ||
                                  instruction.opcode == Opcode::IBitAndK ||
                                  instruction.opcode == Opcode::IBitOrK ||
                                  instruction.opcode == Opcode::IBitXorK ||
                                  instruction.opcode == Opcode::IShlK ||
                                  instruction.opcode == Opcode::IShrK;
    if (integer_k_opcode) {
      std::uint32_t const_id = 0;
      if (!operand_u32_value(instruction, 2, &const_id) ||
          const_id >= module.const_pool.size() ||
          module.const_pool[const_id].kind !=
              amber::bytecode::ConstantKind::Integer) {
        *reason = "integer-K opcode expects an integer constant";
        return false;
      }
    }
    switch (instruction.opcode) {
    case Opcode::LoadK: {
      std::uint32_t ignored_dst = 0;
      std::uint32_t const_id = 0;
      if (!operand_u32_value(instruction, 0, &ignored_dst) ||
          !operand_u32_value(instruction, 1, &const_id) ||
          const_id >= module.const_pool.size()) {
        *reason = "invalid LOADK operand";
        return false;
      }
      const amber::bytecode::ConstantKind kind =
          module.const_pool[const_id].kind;
      if (kind != amber::bytecode::ConstantKind::Null &&
          kind != amber::bytecode::ConstantKind::Bool &&
          kind != amber::bytecode::ConstantKind::Integer &&
          kind != amber::bytecode::ConstantKind::Float &&
          kind != amber::bytecode::ConstantKind::StringRef &&
          kind != amber::bytecode::ConstantKind::SymbolRef) {
        *reason = "non-scalar constants still use VM fallback";
        return false;
      }
      if (kind == amber::bytecode::ConstantKind::StringRef &&
          module.const_pool[const_id].ref_id >= module.strings.size()) {
        *reason = "string constant ref is out of range";
        return false;
      }
      if (kind == amber::bytecode::ConstantKind::SymbolRef &&
          module.const_pool[const_id].ref_id >= module.symbols.size()) {
        *reason = "symbol constant ref is out of range";
        return false;
      }
      if (kind == amber::bytecode::ConstantKind::Float &&
          !std::isfinite(module.const_pool[const_id].float_value)) {
        *reason = "non-finite float constants still use VM fallback";
        return false;
      }
      break;
    }
    case Opcode::LoadNull:
    case Opcode::LoadBool:
    case Opcode::Move:
    case Opcode::GetLast:
    case Opcode::SetLast:
    case Opcode::MakeList:
    case Opcode::MakeSet:
    case Opcode::MakeTuple:
    case Opcode::MakeMap:
    case Opcode::MakeSetSpread:
    case Opcode::MakeMapDyn:
    case Opcode::MakeMapSpread:
    case Opcode::LookupConst:
    case Opcode::LoadUpval:
    case Opcode::StoreUpval:
    case Opcode::IAdd:
    case Opcode::ISub:
    case Opcode::ILt:
    case Opcode::IGt:
    case Opcode::IMul:
    case Opcode::IDiv:
    case Opcode::IMod:
    case Opcode::IFloorDiv:
    case Opcode::ILe:
    case Opcode::IGe:
    case Opcode::IEq:
    case Opcode::INe:
    case Opcode::ICmp:
    case Opcode::IBitAnd:
    case Opcode::IBitOr:
    case Opcode::IBitXor:
    case Opcode::IShl:
    case Opcode::IShr:
    case Opcode::IAddK:
    case Opcode::ISubK:
    case Opcode::ILtK:
    case Opcode::IGtK:
    case Opcode::IMulK:
    case Opcode::IDivK:
    case Opcode::IModK:
    case Opcode::IFloorDivK:
    case Opcode::ILeK:
    case Opcode::IGeK:
    case Opcode::IEqK:
    case Opcode::INeK:
    case Opcode::ICmpK:
    case Opcode::IBitAndK:
    case Opcode::IBitOrK:
    case Opcode::IBitXorK:
    case Opcode::IShlK:
    case Opcode::IShrK:
    case Opcode::Jump:
    case Opcode::JumpIfTrue:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull:
    case Opcode::Return:
    case Opcode::Safepoint:
    case Opcode::CloseUpvalues:
    case Opcode::TypeCheck:
    case Opcode::PBind:
    case Opcode::PCommit:
      break;
    case Opcode::MakeClosure: {
      std::uint32_t ignored_dst = 0;
      std::uint32_t code_id = 0;
      std::uint32_t capture_count = 0;
      if (!operand_u32_value(instruction, 0, &ignored_dst) ||
          !operand_u32_value(instruction, 1, &code_id) ||
          !operand_u32_value(instruction, 2, &capture_count)) {
        *reason = "invalid MAKE_CLOSURE operand";
        return false;
      }
      std::size_t operand_index = 3U;
      for (std::uint32_t capture_i = 0; capture_i < capture_count;
           ++capture_i) {
        std::uint32_t kind = 0;
        std::uint32_t slot = 0;
        if (!operand_u32_value(instruction, operand_index++, &kind) ||
            !operand_u32_value(instruction, operand_index++, &slot) ||
            kind > 1U) {
          *reason = "invalid MAKE_CLOSURE capture operand";
          return false;
        }
      }
      break;
    }
    case Opcode::Call: {
      std::uint32_t ignored_dst = 0;
      std::uint32_t ignored_callee = 0;
      std::uint32_t pos_count = 0;
      if (!operand_u32_value(instruction, 0, &ignored_dst) ||
          !operand_u32_value(instruction, 1, &ignored_callee) ||
          !operand_u32_value(instruction, 2, &pos_count)) {
        *reason = "invalid CALL operand";
        return false;
      }
      const std::size_t kw_index = 3U + pos_count;
      std::uint32_t kw_count = 0;
      if (!operand_u32_value(instruction, kw_index, &kw_count) ||
          kw_count != 0U || !operand_is_no_block(instruction, kw_index + 1U)) {
        *reason = "keyword/block calls still use VM fallback";
        return false;
      }
      break;
    }
    case Opcode::Send: {
      std::uint32_t ignored_dst = 0;
      std::uint32_t ignored_recv = 0;
      std::uint32_t symbol_id = 0;
      std::uint32_t pos_count = 0;
      std::string selector;
      if (!operand_u32_value(instruction, 0, &ignored_dst) ||
          !operand_u32_value(instruction, 1, &ignored_recv) ||
          !operand_u32_value(instruction, 2, &symbol_id) ||
          symbol_id >= module.symbols.size() ||
          !operand_u32_value(instruction, 3, &pos_count)) {
        *reason = "invalid SEND operand";
        return false;
      }
      selector = module.symbols[symbol_id];
      const bool scalar_binary =
          native_cpp_scalar_selector(module, symbol_id, &selector) &&
          pos_count == 1U;
      const bool scalar =
          scalar_binary ||
          (native_cpp_scalar_nullary_selector(selector) && pos_count == 0U) ||
          (native_cpp_scalar_conversion_selector(selector) && pos_count == 0U);
      const std::size_t kw_index = 4U + pos_count;
      std::uint32_t kw_count = 0;
      if (!operand_u32_value(instruction, kw_index, &kw_count)) {
        *reason = "invalid SEND keyword operand";
        return false;
      }
      const std::size_t block_index = kw_index + 1U + kw_count * 2U;
      const bool no_block = operand_is_no_block(instruction, block_index);
      const bool collection_block_send =
          ((selector == "each" || selector == "map" || selector == "select" ||
            selector == "reject" || selector == "transform_keys" ||
            selector == "transform_values" || selector == "find" ||
            selector == "any?" || selector == "all?" || selector == "none?") &&
           pos_count == 0U && kw_count == 0U && !no_block) ||
          (selector == "reduce" && (pos_count == 0U || pos_count == 1U) &&
           kw_count == 0U && !no_block);
      bool json_send = false;
      if ((selector == "to_json" && pos_count == 0U && kw_count == 0U &&
           no_block) ||
          ((selector == "parse" || selector == "generate" ||
            selector == "pretty_generate") &&
           pos_count == 1U && kw_count == 0U && no_block)) {
        json_send = true;
      } else if (selector == "stream_parse_file" && pos_count == 1U &&
                 kw_count == 1U && !no_block) {
        std::uint32_t kw_symbol_id = 0;
        if (!operand_u32_value(instruction, kw_index + 1U, &kw_symbol_id) ||
            kw_symbol_id >= module.symbols.size() ||
            module.symbols[kw_symbol_id] != "jsonl") {
          *reason = "unsupported Json.stream_parse_file keyword shape";
          return false;
        }
        json_send = true;
      }
      bool codec_send = false;
      if (selector == "new" && pos_count == 1U && kw_count == 0U && no_block) {
        codec_send = true;
      } else if (selector == "hex" && pos_count == 0U && kw_count == 0U &&
                 no_block) {
        codec_send = true;
      } else if ((selector == "encode" || selector == "decode") &&
                 pos_count == 1U && kw_count <= 1U && no_block) {
        if (kw_count == 1U) {
          std::uint32_t kw_symbol_id = 0;
          if (!operand_u32_value(instruction, kw_index + 1U, &kw_symbol_id) ||
              kw_symbol_id >= module.symbols.size()) {
            *reason = "invalid codec keyword operand";
            return false;
          }
          const std::string &kw_name = module.symbols[kw_symbol_id];
          if ((selector == "encode" && kw_name != "padding") ||
              (selector == "decode" && kw_name != "mode")) {
            *reason = "unsupported codec keyword shape";
            return false;
          }
        }
        codec_send = true;
      }
      const bool digest_send =
          (((selector == "crc32" || selector == "md5" || selector == "sha1" ||
             selector == "sha256" || selector == "streebog256" ||
             selector == "streebog512" || selector == "gost256" ||
             selector == "gost512" || selector == "гост256" ||
             selector == "гост512" || selector == "стрибог256" ||
             selector == "стрибог512") &&
            pos_count == 1U) ||
           (selector == "hmac_sha256" && pos_count == 2U)) &&
          kw_count == 0U && no_block;
      bool range_send = false;
      if (selector == "new" && pos_count == 2U && kw_count <= 2U && no_block) {
        std::set<std::string> seen_keywords;
        for (std::uint32_t kw_i = 0; kw_i < kw_count; ++kw_i) {
          std::uint32_t kw_symbol_id = 0;
          if (!operand_u32_value(instruction, kw_index + 1U + kw_i * 2U,
                                 &kw_symbol_id) ||
              kw_symbol_id >= module.symbols.size()) {
            *reason = "invalid Range keyword operand";
            return false;
          }
          const std::string &kw_name = module.symbols[kw_symbol_id];
          if ((kw_name != "inclusive_end" && kw_name != "step") ||
              !seen_keywords.insert(kw_name).second) {
            *reason = "unsupported Range keyword shape";
            return false;
          }
        }
        range_send = true;
      }
      bool random_send = false;
      if ((selector == "bytes" || selector == "hex" || selector == "base64" ||
           selector == "base64url") &&
          pos_count == 1U && kw_count <= 1U && no_block) {
        if (kw_count == 1U) {
          std::uint32_t kw_symbol_id = 0;
          if (!operand_u32_value(instruction, kw_index + 1U, &kw_symbol_id) ||
              kw_symbol_id >= module.symbols.size()) {
            *reason = "invalid SecureRandom keyword operand";
            return false;
          }
          const std::string &kw_name = module.symbols[kw_symbol_id];
          if ((selector == "bytes" || selector == "hex") ||
              kw_name != "padding") {
            *reason = "unsupported SecureRandom keyword shape";
            return false;
          }
        }
        random_send = true;
      } else if (selector == "uuid" && pos_count == 0U && kw_count == 0U &&
                 no_block) {
        random_send = true;
      } else if (selector == "int" && pos_count == 1U && kw_count == 0U &&
                 no_block) {
        random_send = true;
      }
      bool time_send = false;
      if (native_cpp_time_nullary_selector(selector) && pos_count == 0U &&
          kw_count == 0U && no_block) {
        time_send = true;
      } else if ((selector == "parse" || selector == "from_unix_ms" ||
                  selector == "from_unix_ns") &&
                 pos_count == 1U && kw_count == 0U && no_block) {
        time_send = true;
      } else if (selector == "from_unix" && pos_count == 1U && kw_count <= 1U &&
                 no_block) {
        if (kw_count == 1U) {
          std::uint32_t kw_symbol_id = 0;
          if (!operand_u32_value(instruction, kw_index + 1U, &kw_symbol_id) ||
              kw_symbol_id >= module.symbols.size() ||
              module.symbols[kw_symbol_id] != "nanosecond") {
            *reason = "unsupported Time.from_unix keyword shape";
            return false;
          }
        }
        time_send = true;
      } else if (selector == "utc" && pos_count == 3U && kw_count <= 4U &&
                 no_block) {
        std::set<std::string> seen_keywords;
        for (std::uint32_t kw_i = 0; kw_i < kw_count; ++kw_i) {
          std::uint32_t kw_symbol_id = 0;
          if (!operand_u32_value(instruction, kw_index + 1U + kw_i * 2U,
                                 &kw_symbol_id) ||
              kw_symbol_id >= module.symbols.size()) {
            *reason = "invalid Time.utc keyword operand";
            return false;
          }
          const std::string &kw_name = module.symbols[kw_symbol_id];
          if ((kw_name != "hour" && kw_name != "minute" &&
               kw_name != "second" && kw_name != "nanosecond") ||
              !seen_keywords.insert(kw_name).second) {
            *reason = "unsupported Time.utc keyword shape";
            return false;
          }
        }
        time_send = true;
      }
      bool uuid_send = false;
      if ((selector == "v4" || selector == "v7" ||
           native_cpp_uuid_nullary_selector(selector)) &&
          pos_count == 0U && kw_count == 0U && no_block) {
        uuid_send = true;
      } else if ((selector == "parse" || selector == "===") &&
                 pos_count == 1U && kw_count == 0U && no_block) {
        uuid_send = true;
      }
      bool regexp_send = false;
      if (native_cpp_regexp_selector(selector, pos_count) && no_block) {
        if ((selector == "compile" || selector == "new" || selector == "r" ||
             selector == "__call__") &&
            kw_count <= 1U) {
          if (kw_count == 1U) {
            std::uint32_t kw_symbol_id = 0;
            if (!operand_u32_value(instruction, kw_index + 1U, &kw_symbol_id) ||
                kw_symbol_id >= module.symbols.size() ||
                module.symbols[kw_symbol_id] != "flags") {
              *reason = "unsupported Regexp.compile keyword shape";
              return false;
            }
          }
          regexp_send = true;
        } else if (kw_count == 0U) {
          regexp_send = true;
        }
      }
      bool regexp_replace_send = false;
      if ((selector == "replace" || selector == "replaced") &&
          (pos_count == 1U || pos_count == 2U) && kw_count <= 1U &&
          ((no_block && pos_count == 2U) || (!no_block && pos_count == 1U))) {
        if (kw_count == 1U) {
          std::uint32_t kw_symbol_id = 0;
          if (!operand_u32_value(instruction, kw_index + 1U, &kw_symbol_id) ||
              kw_symbol_id >= module.symbols.size() ||
              module.symbols[kw_symbol_id] != "count") {
            *reason = "unsupported regexp replace keyword shape";
            return false;
          }
        }
        regexp_replace_send = true;
      }
      const bool url_send = native_cpp_url_selector(selector, pos_count) &&
                            kw_count == 0U && no_block;
      const bool math_send = native_cpp_math_selector(selector, pos_count) &&
                             kw_count == 0U && no_block;
      bool benchmark_send =
          native_cpp_benchmark_selector(selector, pos_count, !no_block);
      if (benchmark_send) {
        std::set<std::string> allowed_benchmark_keywords;
        if (selector == "time" || selector == "measure" ||
            selector == "profile") {
          allowed_benchmark_keywords = {"gc"};
        } else if (selector == "section") {
          allowed_benchmark_keywords = {"data"};
        } else if (selector == "run") {
          allowed_benchmark_keywords = {"iterations", "warmup", "samples",
                                        "min_time", "gc"};
        } else if (selector == "format" || selector == "table" ||
                   selector == "pretty") {
          allowed_benchmark_keywords = {"layout",    "unit", "style",
                                        "highlight", "sort", "metric"};
        } else if (selector == "to_map" || selector == "map") {
          allowed_benchmark_keywords = {"value"};
        } else if (selector == "to_json") {
          allowed_benchmark_keywords = {"pretty"};
        }
        for (std::uint32_t kw_i = 0; kw_i < kw_count; ++kw_i) {
          std::uint32_t kw_symbol_id = 0;
          if (!operand_u32_value(instruction, kw_index + 1U + kw_i * 2U,
                                 &kw_symbol_id) ||
              kw_symbol_id >= module.symbols.size()) {
            *reason = "invalid Benchmark keyword operand";
            return false;
          }
          if (allowed_benchmark_keywords.find(module.symbols[kw_symbol_id]) ==
              allowed_benchmark_keywords.end()) {
            *reason = "unsupported Benchmark keyword shape";
            return false;
          }
        }
      }
      bool argparser_send = false;
      const auto kw_allowed = [&](std::initializer_list<const char *> names) {
        for (std::uint32_t kw_i = 0; kw_i < kw_count; ++kw_i) {
          std::uint32_t kw_symbol = 0;
          if (!operand_u32_value(instruction, kw_index + 1U + kw_i * 2U,
                                 &kw_symbol) ||
              kw_symbol >= module.symbols.size()) {
            *reason = "invalid ArgParser keyword operand";
            return false;
          }
          bool matched = false;
          for (const char *name : names) {
            if (module.symbols[kw_symbol] == name) {
              matched = true;
              break;
            }
          }
          if (!matched) {
            *reason = "unsupported ArgParser keyword shape";
            return false;
          }
        }
        return true;
      };
      if (selector == "new" && pos_count == 0U && kw_count <= 5U && no_block) {
        argparser_send =
            kw_allowed({"cmdline", "name", "about", "env", "add_help"});
      } else if ((selector == "name" || selector == "about") &&
                 pos_count == 1U && kw_count == 0U && no_block) {
        argparser_send = true;
      } else if ((selector == "arg" || selector == "flag") && pos_count >= 1U &&
                 kw_count <= 8U && no_block) {
        argparser_send =
            kw_allowed({"name", "type", "default", "required", "choices",
                        "multiple", "env", "negatable"});
      } else if ((selector == "pos" || selector == "rest") && pos_count == 1U &&
                 kw_count <= 6U && no_block) {
        argparser_send = kw_allowed(
            {"type", "default", "required", "choices", "multiple", "env"});
      } else if (selector == "parse_or_raise" && pos_count == 0U &&
                 kw_count <= 1U && no_block) {
        argparser_send = kw_allowed({"cmdline"});
      }
      const bool fs_path_send =
          ((selector == "Path" && pos_count == 1U) ||
           ((selector == "/" || selector == "join") && pos_count >= 1U) ||
           ((selector == "basename" || selector == "extname" ||
             selector == "parent" || selector == "absolute?" ||
             selector == "normalize" || selector == "to_str") &&
            pos_count == 0U)) &&
          kw_count == 0U && no_block;
      if (!scalar && !native_cpp_collection_selector(selector, pos_count) &&
          !json_send && !codec_send && !digest_send && !range_send &&
          !random_send && !time_send && !uuid_send && !regexp_send &&
          !regexp_replace_send && !url_send && !math_send && !benchmark_send &&
          !argparser_send && !fs_path_send) {
        *reason = "unsupported SEND still uses VM fallback";
        return false;
      }
      if (!json_send && !codec_send && !digest_send && !range_send &&
          !random_send && !time_send && !uuid_send && !regexp_send &&
          !regexp_replace_send && !url_send && !math_send && !benchmark_send &&
          !argparser_send && !fs_path_send &&
          (kw_count != 0U || (!no_block && !collection_block_send))) {
        *reason = "keyword/block SEND still uses VM fallback";
        return false;
      }
      break;
    }
    default:
      *reason = "unsupported opcode for direct native C++: " +
                amber::bytecode::opcode_name(instruction.opcode);
      return false;
    }
  }
  return true;
}

// Per-function VM fallback (amber.native-backend-equivalence.v1, step 2).
//
// A non-native code object may instead execute through an embedded VM call
// at runtime — without restarting the whole program — when it provably
// cannot observe or mutate state shared with the native lane and cannot
// produce observable effects:
//   - no captures (cross-function references in this design are captures);
//   - no closure creation, calls, dynamic sends, or object/class state;
//   - every send selector is from the pure compute allow-list below.
// The call site additionally gates at runtime: arguments must be immutable
// bridge values (null/bool/Int/Float/Uuid) and the result must convert back to
// a native value; otherwise the program falls back to the whole-program
// restart, which stays sound because these functions are effect-free.
bool native_vm_callable_pure_selector(const std::string &selector) {
  // Every selector here must be observably effect-free: it may allocate and
  // read locally but must not mutate state shared with the native lane or
  // perform IO. Soundness does not actually require it to be non-mutating
  // (a bridged function can only reach scalars and values it constructed
  // itself -- never shared state -- and emits no output before it can bail),
  // but we keep the list to side-effect-free *value transforms* so it mirrors
  // the runtime's own pure/`!`-mutator split (vm.cpp RFC §7.1/§8.x) and the
  // "pure compute" framing of amber.native-backend-equivalence.v1. The
  // mutating `!`-suffixed verbs are deliberately excluded.
  static const std::set<std::string> pure = {
      // numeric / comparison / bitwise (Int and Float share selectors)
      "+",
      "-",
      "*",
      "/",
      "%",
      "//",
      "<",
      ">",
      "<=",
      ">=",
      "==",
      "!=",
      "<=>",
      "&",
      "|",
      "^",
      "<<",
      ">>",
      "**",
      "u+",
      "u-",
      "abs",
      // conversions and formatting (allocate locally, no effects)
      "to_str",
      "to_int",
      "to_float",
      "inspect",
      "cast",
      "cast?",
      "to_type",
      // local collection/string reads
      "[]",
      "[]=",
      "[]?",
      "count",
      "length",
      "size",
      "first",
      "empty?",
      "has_index?",
      "include?",
      "includes?",
      "contains?",
      "member?",
      "has_key?",
      "key?",
      "value?",
      "has_value?",
      "keys",
      "values",
      "entries",
      "to_a",
      "to_array",
      "deconstruct",
      "deconstruct_keys",
      // pure (copy-edit) collection verbs: each returns a new value and never
      // mutates the receiver (vm.cpp RFC §7.1 + the sequence "extra" verbs).
      "reverse",
      "reversed",
      "sort",
      "sorted",
      "uniq",
      "concat",
      "compact",
      "appended",
      "inserted",
      "deleted",
      "added",
      "init",
      "tail",
      "take",
      "drop",
      "with",
      "without",
      "slice",
      "join",
      "min",
      "max",
      "minmax",
      // pure Map copy verbs (each returns a new map; the `!` forms mutate).
      "except",
      "merge",
      "invert",
      // pure Set algebra (each returns a new set; predicates return Bool).
      "union",
      "intersection",
      "difference",
      "symmetric_difference",
      "subset?",
      "superset?",
      "proper_subset?",
      "proper_superset?",
      "disjoint?",
      // pure higher-order enumerables: each consumes a block but never mutates
      // the receiver (the in-place forms are the `!` mutators). Admitting them
      // is only useful together with MakeClosure/block-send support below, and
      // is sound only because the block body is itself proven bridge-pure.
      "map",
      "flat_map",
      "select",
      "filter",
      "reject",
      "find",
      "detect",
      "find_index",
      "each",
      "each_pair",
      "each_cons",
      "reduce",
      "fold",
      "inject",
      "group",
      "take_while",
      "drop_while",
      "any?",
      "all?",
      "none?",
      "transform_keys",
      "transform_values",
      "partition",
      "count_by",
      // pure Math-prelude / numeric methods. `Math` resolves to a built-in
      // value independent of module init, so it is reachable in the embedded
      // bridge world, and these run the same libm as every other lane (so the
      // result is bit-identical). `log` is intentionally EXCLUDED: it is
      // overloaded as the effectful `io.Logger#log`; likewise we never admit
      // info/warn/error/debug/write/puts/print.
      "sqrt",
      "cbrt",
      "pow",
      "exp",
      "hypot",
      "floor",
      "ceil",
      "round",
      "trunc",
      "sign",
      "sin",
      "cos",
      "tan",
      "asin",
      "acos",
      "atan",
      "log2",
      "log10",
      // pure string transforms (Str is immutable; these allocate new strings)
      "upcase",
      "downcase",
      "trim",
      "strip",
      "split",
      "replace",
      "starts_with?",
      "ends_with?",
      "chars",
  };
  return pure.find(selector) != pure.end();
}

// In-place (`!`-suffixed) collection mutators are *also* bridge-eligible, even
// though they are not pure. Soundness here does not come from purity but from
// reachability: a vm-callable function takes only immutable copied arguments
// (runtime gate) and has no captures, upvalues, ivars/cvars, closures, or
// `Call`s (static gates), so every mutable heap value it touches is one it
// constructed itself inside the embedded fallback world. Mutating such a value
// is invisible to the native lane (separate heaps, no aliasing) and is
// discarded after the call, and the function still emits no observable output
// before it can bail.
// Re-running the whole program on bailout therefore stays byte-identical.
// The block-taking mutators (`delete_if!`, `select!`, `map!`, `transform_*!`,
// …) are admitted too: a block-bearing send is now allowed when its block body
// is proven bridge-pure (see `native_vm_callable_code_body`), and the same
// reachability argument applies -- the receiver is still a locally-constructed
// value and the pure block produces no effect, so the in-place mutation stays
// invisible to the native lane.
bool native_vm_callable_local_mutator_selector(const std::string &selector) {
  static const std::set<std::string> mutators = {
      // Array (vm.cpp RFC §8.1)
      "push!",
      "append!",
      "unshift!",
      "prepend!",
      "insert!",
      "pop!",
      "shift!",
      "delete_at!",
      "delete!",
      "sort!",
      "uniq!",
      "reverse!",
      "clear!",
      "replace!",
      // Map (vm.cpp RFC §8.2)
      "get_or_set!",
      "store!",
      "merge!",
      "update!",
      "compact!",
      // Set (vm.cpp RFC §8.3)
      "add!",
      "subtract!",
      // Block-taking in-place mutators (sound once the block body is proven
      // bridge-pure): shared by Array/Map/Set.
      "delete_if!",
      "keep_if!",
      "select!",
      "reject!",
      "map!",
      "transform_keys!",
      "transform_values!",
  };
  return mutators.find(selector) != mutators.end();
}

const amber::bytecode::BcCode *
native_code_by_id(const amber::bytecode::BcModule &module,
                  std::uint32_t code_id) {
  for (const amber::bytecode::BcCode &code : module.code_objects) {
    if (code.code_id == code_id) {
      return &code;
    }
  }
  return nullptr;
}

// Recursive bridge-eligibility check shared by the entry function and the block
// bodies it constructs. `allow_captures` is false for the bridged entry (whose
// captures would reach module-level sibling closures, unavailable in the
// init-less fallback world) and true for block bodies, whose captures are
// always enclosing locals of the entry frame -- local to the embedded
// execution, never shared state, because the entry itself has no captures.
bool native_vm_callable_code_body(const amber::bytecode::BcModule &module,
                                  const amber::bytecode::BcCode &code,
                                  bool allow_captures,
                                  std::set<std::uint32_t> &visiting,
                                  std::string *reason) {
  if (!allow_captures && !code.capture_layout.empty()) {
    *reason = "captures reach shared state";
    return false;
  }
  if (!visiting.insert(code.code_id).second) {
    return true; // already proven / on the current recursion path
  }
  for (const amber::bytecode::Instruction &instruction : code.instructions) {
    using amber::bytecode::Opcode;
    switch (instruction.opcode) {
    case Opcode::Call:
    case Opcode::CallSpread:
      *reason = "calls can reach output helpers";
      return false;
    case Opcode::SendDyn:
    case Opcode::SendDynSpread:
      *reason = "dynamic selector is not statically pure";
      return false;
    case Opcode::MakeClosure: {
      // A constructed closure can only be consumed by a pure block-send below
      // (Call is rejected, and a returned/escaped closure bails at result
      // conversion), so the bridge stays sound as long as the block body is
      // itself bridge-pure. Verify it recursively, allowing its captures.
      std::uint32_t code_id = 0;
      if (!operand_u32_value(instruction, 1, &code_id)) {
        *reason = "invalid MAKE_CLOSURE operand";
        return false;
      }
      const amber::bytecode::BcCode *block = native_code_by_id(module, code_id);
      if (block == nullptr || !native_vm_callable_code_body(
                                  module, *block, true, visiting, reason)) {
        if (block == nullptr) {
          *reason = "block body is not in the module";
        }
        return false;
      }
      break;
    }
    case Opcode::LoadUpval:
    case Opcode::StoreUpval:
      if (!allow_captures) {
        *reason = "upvalues reach shared state";
        return false;
      }
      break;
    case Opcode::LoadIvar:
    case Opcode::StoreIvar:
    case Opcode::LoadCvar:
    case Opcode::StoreCvar:
      *reason = "object state is shared with the native lane";
      return false;
    case Opcode::Send:
    case Opcode::SendSpread: {
      std::uint32_t symbol_id = 0;
      std::uint32_t pos_count = 0;
      if (!operand_u32_value(instruction, 2, &symbol_id) ||
          symbol_id >= module.symbols.size() ||
          !operand_u32_value(instruction, 3, &pos_count)) {
        *reason = "invalid SEND operand";
        return false;
      }
      const std::string &selector = module.symbols[symbol_id];
      if (!native_vm_callable_pure_selector(selector) &&
          !native_vm_callable_local_mutator_selector(selector)) {
        *reason = "selector `" + selector + "` is not in the bridge allow-list";
        return false;
      }
      // Keyword arguments still bail, but a trailing block operand is now
      // allowed: the block was constructed by a MakeClosure already verified
      // bridge-pure above, and the receiver selector is on the pure/mutator
      // allow-list (the higher-order enumerables never mutate the receiver).
      const std::size_t per_arg =
          instruction.opcode == Opcode::SendSpread ? 2U : 1U;
      const std::size_t kw_index = 4U + pos_count * per_arg;
      std::uint32_t kw_count = 0;
      if (!operand_u32_value(instruction, kw_index, &kw_count) ||
          kw_count != 0U) {
        *reason = "keyword sends are not statically pure";
        return false;
      }
      break;
    }
    default:
      break;
    }
  }
  return true;
}

bool native_cpp_code_vm_callable(const amber::bytecode::BcModule &module,
                                 const amber::bytecode::BcCode &code,
                                 std::string *reason) {
  std::set<std::uint32_t> visiting;
  return native_vm_callable_code_body(module, code, /*allow_captures=*/false,
                                      visiting, reason);
}

std::string
native_cpp_constant_expr(const amber::bytecode::Constant &constant) {
  switch (constant.kind) {
  case amber::bytecode::ConstantKind::Null:
    return "NativeValue::nullv()";
  case amber::bytecode::ConstantKind::Bool:
    return std::string("NativeValue::boolean(") +
           (constant.bool_value ? "true" : "false") + ")";
  case amber::bytecode::ConstantKind::Integer:
    return "NativeValue::integer(" + cpp_decimal_i64(constant.int_value) + ")";
  case amber::bytecode::ConstantKind::Float: {
    // Hexfloat round-trips the double bit-exactly through the C++ compiler.
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%a", constant.float_value);
    return std::string("NativeValue::floating(") + buffer + ")";
  }
  case amber::bytecode::ConstantKind::StringRef:
    // The native string table is seeded with the module strings, so the
    // bytecode ref id is the native string id.
    return "NativeValue::string_ref(" + std::to_string(constant.ref_id) + ")";
  case amber::bytecode::ConstantKind::SymbolRef:
    return "NativeValue::symbol_ref(" + std::to_string(constant.ref_id) + ")";
  default:
    return "NativeValue::nullv()";
  }
}

bool native_cpp_code_uses_local_capture_cells(
    const amber::bytecode::BcCode &code) {
  for (const amber::bytecode::Instruction &instruction : code.instructions) {
    if (instruction.opcode != amber::bytecode::Opcode::MakeClosure) {
      continue;
    }
    std::uint32_t capture_count = 0;
    if (!operand_u32_value(instruction, 2, &capture_count)) {
      continue;
    }
    std::size_t operand_index = 3U;
    for (std::uint32_t capture_i = 0; capture_i < capture_count; ++capture_i) {
      std::uint32_t kind = 0;
      std::uint32_t slot = 0;
      if (!operand_u32_value(instruction, operand_index++, &kind) ||
          !operand_u32_value(instruction, operand_index++, &slot)) {
        break;
      }
      (void)slot;
      if (kind == 0U) {
        return true;
      }
    }
  }
  return false;
}

enum class NativeScalarKind { Unknown, Integer, Float, Bool };

struct NativeScalarFlow {
  std::vector<std::vector<NativeScalarKind>> at_pc;
  std::vector<std::vector<NativeScalarKind>> after_pc;
};

NativeScalarKind native_cpp_scalar_meet(NativeScalarKind lhs,
                                        NativeScalarKind rhs) {
  return lhs == rhs ? lhs : NativeScalarKind::Unknown;
}

bool native_cpp_scalar_numeric(NativeScalarKind kind) {
  return kind == NativeScalarKind::Integer || kind == NativeScalarKind::Float;
}

bool native_cpp_opcode_numeric_arithmetic(amber::bytecode::Opcode opcode) {
  using amber::bytecode::Opcode;
  switch (opcode) {
  case Opcode::IAdd:
  case Opcode::ISub:
  case Opcode::IMul:
  case Opcode::IDiv:
  case Opcode::IMod:
  case Opcode::IFloorDiv:
  case Opcode::IAddK:
  case Opcode::ISubK:
  case Opcode::IMulK:
  case Opcode::IDivK:
  case Opcode::IModK:
  case Opcode::IFloorDivK:
    return true;
  default:
    return false;
  }
}

bool native_cpp_opcode_integer_only(amber::bytecode::Opcode opcode) {
  using amber::bytecode::Opcode;
  switch (opcode) {
  case Opcode::IBitAnd:
  case Opcode::IBitOr:
  case Opcode::IBitXor:
  case Opcode::IShl:
  case Opcode::IShr:
  case Opcode::IBitAndK:
  case Opcode::IBitOrK:
  case Opcode::IBitXorK:
  case Opcode::IShlK:
  case Opcode::IShrK:
    return true;
  default:
    return false;
  }
}

bool native_cpp_opcode_numeric_cmp(amber::bytecode::Opcode opcode) {
  using amber::bytecode::Opcode;
  switch (opcode) {
  case Opcode::ICmp:
  case Opcode::ICmpK:
    return true;
  default:
    return false;
  }
}

bool native_cpp_opcode_bool_compare(amber::bytecode::Opcode opcode) {
  using amber::bytecode::Opcode;
  switch (opcode) {
  case Opcode::ILt:
  case Opcode::IGt:
  case Opcode::ILe:
  case Opcode::IGe:
  case Opcode::IEq:
  case Opcode::INe:
  case Opcode::ILtK:
  case Opcode::IGtK:
  case Opcode::ILeK:
  case Opcode::IGeK:
  case Opcode::IEqK:
  case Opcode::INeK:
    return true;
  default:
    return false;
  }
}

NativeScalarKind
native_cpp_numeric_binary_result_kind(amber::bytecode::Opcode opcode,
                                      NativeScalarKind lhs,
                                      NativeScalarKind rhs) {
  if (native_cpp_opcode_bool_compare(opcode)) {
    return native_cpp_scalar_numeric(lhs) && native_cpp_scalar_numeric(rhs)
               ? NativeScalarKind::Bool
               : NativeScalarKind::Unknown;
  }
  if (native_cpp_opcode_numeric_cmp(opcode)) {
    return native_cpp_scalar_numeric(lhs) && native_cpp_scalar_numeric(rhs)
               ? NativeScalarKind::Integer
               : NativeScalarKind::Unknown;
  }
  if (native_cpp_opcode_integer_only(opcode)) {
    return lhs == NativeScalarKind::Integer && rhs == NativeScalarKind::Integer
               ? NativeScalarKind::Integer
               : NativeScalarKind::Unknown;
  }
  if (!native_cpp_opcode_numeric_arithmetic(opcode) ||
      !native_cpp_scalar_numeric(lhs) || !native_cpp_scalar_numeric(rhs)) {
    return NativeScalarKind::Unknown;
  }
  return lhs == NativeScalarKind::Integer && rhs == NativeScalarKind::Integer
             ? NativeScalarKind::Integer
             : NativeScalarKind::Float;
}

NativeScalarKind
native_cpp_numeric_k_result_kind(amber::bytecode::Opcode opcode,
                                 NativeScalarKind lhs) {
  if (native_cpp_opcode_bool_compare(opcode)) {
    return native_cpp_scalar_numeric(lhs) ? NativeScalarKind::Bool
                                          : NativeScalarKind::Unknown;
  }
  if (native_cpp_opcode_numeric_cmp(opcode)) {
    return native_cpp_scalar_numeric(lhs) ? NativeScalarKind::Integer
                                          : NativeScalarKind::Unknown;
  }
  if (native_cpp_opcode_integer_only(opcode)) {
    return lhs == NativeScalarKind::Integer ? NativeScalarKind::Integer
                                            : NativeScalarKind::Unknown;
  }
  if (!native_cpp_opcode_numeric_arithmetic(opcode) ||
      !native_cpp_scalar_numeric(lhs)) {
    return NativeScalarKind::Unknown;
  }
  return lhs == NativeScalarKind::Integer ? NativeScalarKind::Integer
                                          : NativeScalarKind::Float;
}

NativeScalarFlow
native_cpp_scalar_registers(const amber::bytecode::BcModule &module,
                            const amber::bytecode::BcCode &code) {
  using amber::bytecode::Opcode;

  const std::size_t pc_count = code.instructions.size();
  NativeScalarFlow flow;
  flow.at_pc.assign(pc_count, std::vector<NativeScalarKind>(
                                  code.reg_count, NativeScalarKind::Unknown));
  flow.after_pc = flow.at_pc;
  std::vector<bool> reached(pc_count, false);
  std::vector<std::size_t> worklist;
  if (pc_count == 0U) {
    return flow;
  }

  const auto enqueue = [&](std::size_t target,
                           const std::vector<NativeScalarKind> &state) {
    if (target >= pc_count) {
      return;
    }
    bool changed = false;
    if (!reached[target]) {
      flow.at_pc[target] = state;
      reached[target] = true;
      changed = true;
    } else {
      for (std::size_t reg = 0; reg < state.size(); ++reg) {
        const NativeScalarKind merged =
            native_cpp_scalar_meet(flow.at_pc[target][reg], state[reg]);
        if (merged != flow.at_pc[target][reg]) {
          flow.at_pc[target][reg] = merged;
          changed = true;
        }
      }
    }
    if (changed) {
      worklist.push_back(target);
    }
  };

  reached[0] = true;
  worklist.push_back(0);
  while (!worklist.empty()) {
    const std::size_t pc = worklist.back();
    worklist.pop_back();
    const amber::bytecode::Instruction &instruction = code.instructions[pc];
    std::vector<NativeScalarKind> next = flow.at_pc[pc];

    const auto set_dst = [&](NativeScalarKind kind) {
      std::uint32_t dst = 0;
      if (operand_u32_value(instruction, 0, &dst) && dst < next.size()) {
        next[dst] = kind;
      }
    };
    const auto set_dst_from_reg = [&](std::uint32_t src) {
      std::uint32_t dst = 0;
      if (operand_u32_value(instruction, 0, &dst) && dst < next.size()) {
        next[dst] = src < flow.at_pc[pc].size() ? flow.at_pc[pc][src]
                                                : NativeScalarKind::Unknown;
      }
    };
    const auto reg_kind = [&](std::size_t operand_index) {
      std::uint32_t reg = 0;
      if (!operand_u32_value(instruction, operand_index, &reg) ||
          reg >= flow.at_pc[pc].size()) {
        return NativeScalarKind::Unknown;
      }
      return flow.at_pc[pc][reg];
    };

    switch (instruction.opcode) {
    case Opcode::LoadK: {
      std::uint32_t dst = 0;
      std::uint32_t const_id = 0;
      if (operand_u32_value(instruction, 0, &dst) &&
          operand_u32_value(instruction, 1, &const_id) && dst < next.size()) {
        NativeScalarKind kind = NativeScalarKind::Unknown;
        if (const_id < module.const_pool.size()) {
          if (module.const_pool[const_id].kind ==
              amber::bytecode::ConstantKind::Integer) {
            kind = NativeScalarKind::Integer;
          } else if (module.const_pool[const_id].kind ==
                     amber::bytecode::ConstantKind::Float) {
            kind = NativeScalarKind::Float;
          }
        }
        next[dst] = kind;
      }
      break;
    }
    case Opcode::Move: {
      std::uint32_t src = 0;
      if (operand_u32_value(instruction, 1, &src)) {
        set_dst_from_reg(src);
      }
      break;
    }
    case Opcode::IAdd:
    case Opcode::ISub:
    case Opcode::ILt:
    case Opcode::IGt:
    case Opcode::IMul:
    case Opcode::IDiv:
    case Opcode::IMod:
    case Opcode::IFloorDiv:
    case Opcode::ILe:
    case Opcode::IGe:
    case Opcode::IEq:
    case Opcode::INe:
    case Opcode::ICmp:
    case Opcode::IBitAnd:
    case Opcode::IBitOr:
    case Opcode::IBitXor:
    case Opcode::IShl:
    case Opcode::IShr:
      set_dst(native_cpp_numeric_binary_result_kind(instruction.opcode,
                                                    reg_kind(1), reg_kind(2)));
      break;
    case Opcode::IAddK:
    case Opcode::ISubK:
    case Opcode::ILtK:
    case Opcode::IGtK:
    case Opcode::IMulK:
    case Opcode::IDivK:
    case Opcode::IModK:
    case Opcode::IFloorDivK:
    case Opcode::ILeK:
    case Opcode::IGeK:
    case Opcode::IEqK:
    case Opcode::INeK:
    case Opcode::ICmpK:
    case Opcode::IBitAndK:
    case Opcode::IBitOrK:
    case Opcode::IBitXorK:
    case Opcode::IShlK:
    case Opcode::IShrK:
      set_dst(
          native_cpp_numeric_k_result_kind(instruction.opcode, reg_kind(1)));
      break;
    case Opcode::LoadBool:
      set_dst(NativeScalarKind::Bool);
      break;
    case Opcode::LoadNull:
    case Opcode::GetLast:
    case Opcode::MakeList:
    case Opcode::MakeSet:
    case Opcode::MakeTuple:
    case Opcode::MakeMap:
    case Opcode::MakeSetSpread:
    case Opcode::MakeMapDyn:
    case Opcode::MakeMapSpread:
    case Opcode::LookupConst:
    case Opcode::LoadUpval:
    case Opcode::MakeClosure:
    case Opcode::Call:
    case Opcode::Send:
    case Opcode::SendSpread:
      set_dst(NativeScalarKind::Unknown);
      break;
    default:
      break;
    }

    flow.after_pc[pc] = next;

    std::uint32_t target = 0;
    if (instruction.opcode == Opcode::Jump) {
      if (operand_u32_value(instruction, 0, &target)) {
        enqueue(target, next);
      }
    } else if (instruction.opcode == Opcode::JumpIfTrue ||
               instruction.opcode == Opcode::JumpIfFalse ||
               instruction.opcode == Opcode::JumpIfNull) {
      if (operand_u32_value(instruction, 1, &target)) {
        enqueue(target, next);
      }
      enqueue(pc + 1U, next);
    } else if (instruction.opcode != Opcode::Return) {
      enqueue(pc + 1U, next);
    }
  }
  return flow;
}

std::vector<std::vector<bool>>
native_cpp_live_registers_at_pc(const amber::bytecode::BcCode &code) {
  using amber::bytecode::Opcode;

  const std::size_t pc_count = code.instructions.size();
  std::vector<std::vector<bool>> live_in(
      pc_count, std::vector<bool>(code.reg_count, false));
  if (pc_count == 0U) {
    return live_in;
  }

  const auto add_reg = [&](std::vector<bool> &regs, std::uint32_t reg) {
    if (reg < regs.size()) {
      regs[reg] = true;
    }
  };
  const auto remove_reg = [&](std::vector<bool> &regs, std::uint32_t reg) {
    if (reg < regs.size()) {
      regs[reg] = false;
    }
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t reverse_pc = pc_count; reverse_pc > 0U; --reverse_pc) {
      const std::size_t pc = reverse_pc - 1U;
      const amber::bytecode::Instruction &instruction = code.instructions[pc];
      std::vector<bool> live(code.reg_count, false);
      const auto merge_successor = [&](std::size_t target) {
        if (target >= pc_count) {
          return;
        }
        for (std::size_t reg = 0; reg < code.reg_count; ++reg) {
          live[reg] = live[reg] || live_in[target][reg];
        }
      };

      std::uint32_t target = 0;
      if (instruction.opcode == Opcode::Jump) {
        if (operand_u32_value(instruction, 0, &target)) {
          merge_successor(target);
        }
      } else if (instruction.opcode == Opcode::JumpIfTrue ||
                 instruction.opcode == Opcode::JumpIfFalse ||
                 instruction.opcode == Opcode::JumpIfNull) {
        if (operand_u32_value(instruction, 1, &target)) {
          merge_successor(target);
        }
        merge_successor(pc + 1U);
      } else if (instruction.opcode != Opcode::Return) {
        merge_successor(pc + 1U);
      }

      const auto use_operand = [&](std::size_t index) {
        std::uint32_t reg = 0;
        if (operand_u32_value(instruction, index, &reg)) {
          add_reg(live, reg);
        }
      };
      const auto def_operand = [&](std::size_t index) {
        std::uint32_t reg = 0;
        if (operand_u32_value(instruction, index, &reg)) {
          remove_reg(live, reg);
        }
      };

      switch (instruction.opcode) {
      case Opcode::LoadK:
      case Opcode::LoadNull:
      case Opcode::LoadBool:
      case Opcode::LookupConst:
      case Opcode::GetLast:
      case Opcode::LoadUpval:
        def_operand(0);
        break;
      case Opcode::Move:
        def_operand(0);
        use_operand(1);
        break;
      case Opcode::MakeList:
      case Opcode::MakeSet:
      case Opcode::MakeTuple: {
        def_operand(0);
        std::uint32_t first_reg = 0;
        std::uint32_t count = 0;
        if (operand_u32_value(instruction, 1, &first_reg) &&
            operand_u32_value(instruction, 2, &count)) {
          for (std::uint32_t index = 0; index < count; ++index) {
            add_reg(live, first_reg + index);
          }
        }
        break;
      }
      case Opcode::MakeSetSpread: {
        def_operand(0);
        std::uint32_t count = 0;
        if (operand_u32_value(instruction, 1, &count)) {
          std::size_t operand_index = 2U;
          for (std::uint32_t index = 0; index < count; ++index) {
            ++operand_index;
            std::uint32_t value_reg = 0;
            if (operand_u32_value(instruction, operand_index++, &value_reg)) {
              add_reg(live, value_reg);
            }
          }
        }
        break;
      }
      case Opcode::MakeMap: {
        def_operand(0);
        std::uint32_t count = 0;
        if (operand_u32_value(instruction, 1, &count)) {
          count &= amber::bytecode::kMapCountMask;
          for (std::uint32_t index = 0; index < count; ++index) {
            use_operand(3U + index * 2U);
          }
        }
        break;
      }
      case Opcode::MakeMapDyn: {
        def_operand(0);
        std::uint32_t count = 0;
        if (operand_u32_value(instruction, 1, &count)) {
          count &= amber::bytecode::kMapCountMask;
          for (std::uint32_t index = 0; index < count; ++index) {
            use_operand(2U + index * 2U);
            use_operand(3U + index * 2U);
          }
        }
        break;
      }
      case Opcode::MakeMapSpread: {
        def_operand(0);
        std::uint32_t count = 0;
        if (operand_u32_value(instruction, 1, &count)) {
          count &= amber::bytecode::kMapCountMask;
          for (std::uint32_t index = 0; index < count; ++index) {
            const std::size_t base = 2U + index * 3U;
            std::uint32_t kind = 0;
            if (operand_u32_value(instruction, base, &kind) &&
                kind == amber::bytecode::kMapSpreadEntryDynamic) {
              use_operand(base + 1U);
            }
            use_operand(base + 2U);
          }
        }
        break;
      }
      case Opcode::SetLast:
      case Opcode::StoreUpval:
        use_operand(instruction.opcode == Opcode::SetLast ? 0U : 1U);
        break;
      case Opcode::MakeClosure: {
        def_operand(0);
        std::uint32_t capture_count = 0;
        if (operand_u32_value(instruction, 2, &capture_count)) {
          std::size_t operand_index = 3U;
          for (std::uint32_t index = 0; index < capture_count; ++index) {
            std::uint32_t kind = 0;
            std::uint32_t slot = 0;
            if (!operand_u32_value(instruction, operand_index++, &kind) ||
                !operand_u32_value(instruction, operand_index++, &slot)) {
              break;
            }
            if (kind == 0U) {
              add_reg(live, slot);
            }
          }
        }
        break;
      }
      case Opcode::Call: {
        def_operand(0);
        use_operand(1);
        std::uint32_t pos_count = 0;
        if (operand_u32_value(instruction, 2, &pos_count)) {
          for (std::uint32_t index = 0; index < pos_count; ++index) {
            use_operand(3U + index);
          }
        }
        break;
      }
      case Opcode::Send: {
        def_operand(0);
        use_operand(1);
        std::uint32_t pos_count = 0;
        if (!operand_u32_value(instruction, 3, &pos_count)) {
          break;
        }
        for (std::uint32_t index = 0; index < pos_count; ++index) {
          use_operand(4U + index);
        }
        const std::size_t kw_index = 4U + pos_count;
        std::uint32_t kw_count = 0;
        if (operand_u32_value(instruction, kw_index, &kw_count)) {
          for (std::uint32_t index = 0; index < kw_count; ++index) {
            use_operand(kw_index + 2U + index * 2U);
          }
          const std::size_t block_index = kw_index + 1U + kw_count * 2U;
          if (block_index < instruction.operands.size() &&
              !operand_is_no_block(instruction, block_index)) {
            use_operand(block_index);
          }
        }
        break;
      }
      case Opcode::SendSpread:
        def_operand(0);
        use_operand(1);
        break;
      case Opcode::IAdd:
      case Opcode::ISub:
      case Opcode::ILt:
      case Opcode::IGt:
      case Opcode::IMul:
      case Opcode::IDiv:
      case Opcode::IMod:
      case Opcode::IFloorDiv:
      case Opcode::ILe:
      case Opcode::IGe:
      case Opcode::IEq:
      case Opcode::INe:
      case Opcode::ICmp:
      case Opcode::IBitAnd:
      case Opcode::IBitOr:
      case Opcode::IBitXor:
      case Opcode::IShl:
      case Opcode::IShr:
        def_operand(0);
        use_operand(1);
        use_operand(2);
        break;
      case Opcode::IAddK:
      case Opcode::ISubK:
      case Opcode::ILtK:
      case Opcode::IGtK:
      case Opcode::IMulK:
      case Opcode::IDivK:
      case Opcode::IModK:
      case Opcode::IFloorDivK:
      case Opcode::ILeK:
      case Opcode::IGeK:
      case Opcode::IEqK:
      case Opcode::INeK:
      case Opcode::ICmpK:
      case Opcode::IBitAndK:
      case Opcode::IBitOrK:
      case Opcode::IBitXorK:
      case Opcode::IShlK:
      case Opcode::IShrK:
        def_operand(0);
        use_operand(1);
        break;
      case Opcode::JumpIfTrue:
      case Opcode::JumpIfFalse:
      case Opcode::JumpIfNull:
      case Opcode::Return:
        use_operand(0);
        break;
      default:
        break;
      }

      if (live != live_in[pc]) {
        live_in[pc] = std::move(live);
        changed = true;
      }
    }
  }
  return live_in;
}

std::string
emit_native_cpp_code_function(const amber::bytecode::BcModule &module,
                              const amber::bytecode::BcCode &code) {
  std::ostringstream out;
  const std::string fn = native_cpp_function_name(code.code_id);
  const bool uses_local_capture_cells =
      native_cpp_code_uses_local_capture_cells(code);
  const NativeScalarFlow scalar_flow =
      uses_local_capture_cells ? NativeScalarFlow{}
                               : native_cpp_scalar_registers(module, code);
  const std::vector<std::vector<bool>> live_registers_at_pc =
      uses_local_capture_cells ? std::vector<std::vector<bool>>{}
                               : native_cpp_live_registers_at_pc(code);
  std::vector<bool> int_lane_used(code.reg_count, false);
  std::vector<bool> float_lane_used(code.reg_count, false);
  std::vector<bool> bool_lane_used(code.reg_count, false);
  if (!uses_local_capture_cells) {
    for (const std::vector<NativeScalarKind> &state : scalar_flow.at_pc) {
      for (std::size_t reg = 0; reg < state.size(); ++reg) {
        if (state[reg] == NativeScalarKind::Integer) {
          int_lane_used[reg] = true;
        } else if (state[reg] == NativeScalarKind::Float) {
          float_lane_used[reg] = true;
        } else if (state[reg] == NativeScalarKind::Bool) {
          bool_lane_used[reg] = true;
        }
      }
    }
    for (const std::vector<NativeScalarKind> &state : scalar_flow.after_pc) {
      for (std::size_t reg = 0; reg < state.size(); ++reg) {
        if (state[reg] == NativeScalarKind::Integer) {
          int_lane_used[reg] = true;
        } else if (state[reg] == NativeScalarKind::Float) {
          float_lane_used[reg] = true;
        } else if (state[reg] == NativeScalarKind::Bool) {
          bool_lane_used[reg] = true;
        }
      }
    }
  }
  const auto read_frame_reg_expr =
      [uses_local_capture_cells](std::uint32_t reg) {
        if (uses_local_capture_cells) {
          return "read_reg(frame, " + std::to_string(reg) + ")";
        }
        return "frame.regs[" + std::to_string(reg) + "]";
      };
  const auto int_lane_expr = [](std::uint32_t reg) {
    return "ireg_" + std::to_string(reg);
  };
  const auto float_lane_expr = [](std::uint32_t reg) {
    return "freg_" + std::to_string(reg);
  };
  const auto bool_lane_expr = [](std::uint32_t reg) {
    return "breg_" + std::to_string(reg);
  };
  const auto write_reg_stmt = [&](std::uint32_t reg, const std::string &expr) {
    if (uses_local_capture_cells) {
      out << "  write_reg(frame, " << reg << ", " << expr << ");\n";
    } else {
      out << "  frame.regs[" << reg << "] = " << expr << ";\n";
    }
  };
  const auto write_int_reg_stmt = [&](std::uint32_t reg,
                                      const std::string &expr) {
    if (!uses_local_capture_cells && reg < int_lane_used.size() &&
        int_lane_used[reg]) {
      out << "  " << int_lane_expr(reg) << " = " << expr << ";\n";
    } else {
      write_reg_stmt(reg, "NativeValue::integer(" + expr + ")");
    }
  };
  const auto write_float_reg_stmt = [&](std::uint32_t reg,
                                        const std::string &expr) {
    if (!uses_local_capture_cells && reg < float_lane_used.size() &&
        float_lane_used[reg]) {
      out << "  " << float_lane_expr(reg) << " = " << expr << ";\n";
    } else {
      write_reg_stmt(reg, "NativeValue::floating(" + expr + ")");
    }
  };
  const auto write_bool_reg_stmt = [&](std::uint32_t reg,
                                       const std::string &expr) {
    if (!uses_local_capture_cells && reg < bool_lane_used.size() &&
        bool_lane_used[reg]) {
      out << "  " << bool_lane_expr(reg) << " = " << expr << ";\n";
    } else {
      write_reg_stmt(reg, "NativeValue::boolean(" + expr + ")");
    }
  };
  const auto scalar_kind = [](std::uint32_t reg,
                              const std::vector<NativeScalarKind> &state) {
    return reg < state.size() ? state[reg] : NativeScalarKind::Unknown;
  };
  const auto boxed_reg_expr = [&](std::uint32_t reg,
                                  const std::vector<NativeScalarKind> &state) {
    if (!uses_local_capture_cells && reg < state.size()) {
      if (state[reg] == NativeScalarKind::Integer) {
        return "NativeValue::integer(" + int_lane_expr(reg) + ")";
      }
      if (state[reg] == NativeScalarKind::Float) {
        return "NativeValue::floating(" + float_lane_expr(reg) + ")";
      }
      if (state[reg] == NativeScalarKind::Bool) {
        return "NativeValue::boolean(" + bool_lane_expr(reg) + ")";
      }
    }
    return read_frame_reg_expr(reg);
  };
  const auto int_reg_expr = [&](std::uint32_t reg,
                                const std::vector<NativeScalarKind> &state) {
    if (!uses_local_capture_cells && reg < state.size() &&
        state[reg] == NativeScalarKind::Integer) {
      return int_lane_expr(reg);
    }
    return "as_int(" + boxed_reg_expr(reg, state) + ")";
  };
  const auto double_reg_expr = [&](std::uint32_t reg,
                                   const std::vector<NativeScalarKind> &state) {
    if (!uses_local_capture_cells && reg < state.size()) {
      if (state[reg] == NativeScalarKind::Float) {
        return float_lane_expr(reg);
      }
      if (state[reg] == NativeScalarKind::Integer) {
        return "static_cast<double>(" + int_lane_expr(reg) + ")";
      }
    }
    return "as_double_numeric(" + boxed_reg_expr(reg, state) + ")";
  };
  const auto truthy_reg_expr = [&](std::uint32_t reg,
                                   const std::vector<NativeScalarKind> &state) {
    if (!uses_local_capture_cells && reg < state.size() &&
        state[reg] == NativeScalarKind::Bool) {
      return bool_lane_expr(reg);
    }
    return "truthy(" + boxed_reg_expr(reg, state) + ")";
  };
  out << "static NativeValue " << fn
      << "(std::initializer_list<NativeValue> args, "
         "NativeClosure *current_closure) {\n";
  out << "  std::array<NativeValue, " << code.reg_count << "> regs{};\n";
  if (uses_local_capture_cells) {
    out << "  std::array<NativeCell *, " << code.reg_count
        << "> local_cells{};\n";
    out << "  NativeFrame frame(regs.data(), local_cells.data(), regs.size(), "
           "current_closure);\n";
  } else {
    out << "  NativeFrame frame(regs.data(), nullptr, regs.size(), "
           "current_closure);\n";
  }
  out << "  std::size_t arg_index = 0;\n";
  out << "  for (const NativeValue &arg : args) {\n";
  out << "    if (arg_index >= frame.reg_count) break;\n";
  if (uses_local_capture_cells) {
    out << "    write_reg(frame, static_cast<std::uint32_t>(arg_index++), "
           "arg);\n";
  } else {
    out << "    frame.regs[arg_index++] = arg;\n";
  }
  out << "  }\n";
  if (!uses_local_capture_cells) {
    for (std::uint32_t reg = 0; reg < code.reg_count; ++reg) {
      if (int_lane_used[reg]) {
        out << "  std::int64_t " << int_lane_expr(reg) << " = 0;\n";
      }
      if (float_lane_used[reg]) {
        out << "  double " << float_lane_expr(reg) << " = 0.0;\n";
      }
      if (bool_lane_used[reg]) {
        out << "  bool " << bool_lane_expr(reg) << " = false;\n";
      }
    }
  }
  if (code.instructions.empty()) {
    out << "  return NativeValue::nullv();\n";
    out << "}\n\n";
    return out.str();
  }
  const auto materialize_changed_scalars =
      [&](const std::vector<NativeScalarKind> &from,
          const std::vector<NativeScalarKind> &to,
          const std::vector<bool> &live_at_target) {
        if (uses_local_capture_cells) {
          return;
        }
        const std::size_t reg_count =
            std::min<std::size_t>(from.size(), to.size());
        for (std::size_t reg = 0; reg < reg_count; ++reg) {
          if (from[reg] == to[reg]) {
            continue;
          }
          if (reg >= live_at_target.size() || !live_at_target[reg]) {
            continue;
          }
          if (from[reg] == NativeScalarKind::Integer) {
            write_reg_stmt(static_cast<std::uint32_t>(reg),
                           "NativeValue::integer(" +
                               int_lane_expr(static_cast<std::uint32_t>(reg)) +
                               ")");
          } else if (from[reg] == NativeScalarKind::Float) {
            write_reg_stmt(
                static_cast<std::uint32_t>(reg),
                "NativeValue::floating(" +
                    float_lane_expr(static_cast<std::uint32_t>(reg)) + ")");
          } else if (from[reg] == NativeScalarKind::Bool) {
            write_reg_stmt(static_cast<std::uint32_t>(reg),
                           "NativeValue::boolean(" +
                               bool_lane_expr(static_cast<std::uint32_t>(reg)) +
                               ")");
          }
        }
      };
  const auto emit_goto_pc = [&](std::size_t target,
                                const std::vector<NativeScalarKind> &from) {
    if (target < code.instructions.size() && !uses_local_capture_cells) {
      materialize_changed_scalars(from, scalar_flow.at_pc[target],
                                  live_registers_at_pc[target]);
    }
    out << "  goto pc_" << target << ";\n";
  };
  const auto emit_next = [&](std::size_t pc,
                             const std::vector<NativeScalarKind> &from) {
    if (pc + 1U < code.instructions.size()) {
      emit_goto_pc(pc + 1U, from);
    } else {
      out << "  throw NativeBailout();\n";
    }
  };
  out << "  goto pc_0;\n";
  for (std::size_t pc = 0; pc < code.instructions.size(); ++pc) {
    const amber::bytecode::Instruction &instruction = code.instructions[pc];
    using amber::bytecode::Opcode;
    const std::vector<NativeScalarKind> empty_scalar_state;
    const std::vector<NativeScalarKind> &pc_scalar_state =
        uses_local_capture_cells ? empty_scalar_state : scalar_flow.at_pc[pc];
    const std::vector<NativeScalarKind> &next_scalar_state =
        uses_local_capture_cells ? empty_scalar_state
                                 : scalar_flow.after_pc[pc];
    const auto read_reg_expr = [&](std::uint32_t reg) {
      return boxed_reg_expr(reg, pc_scalar_state);
    };
    out << "pc_" << pc << ":\n";
    switch (instruction.opcode) {
    case Opcode::LoadK: {
      std::uint32_t dst = 0;
      std::uint32_t const_id = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &const_id);
      const amber::bytecode::Constant &constant = module.const_pool[const_id];
      if (constant.kind == amber::bytecode::ConstantKind::Integer) {
        write_int_reg_stmt(dst, cpp_decimal_i64(constant.int_value));
      } else if (constant.kind == amber::bytecode::ConstantKind::Float) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%a", constant.float_value);
        write_float_reg_stmt(dst, buffer);
      } else {
        write_reg_stmt(dst, native_cpp_constant_expr(constant));
      }
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::LoadNull: {
      std::uint32_t dst = 0;
      operand_u32_value(instruction, 0, &dst);
      write_reg_stmt(dst, "NativeValue::nullv()");
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::LoadBool: {
      std::uint32_t dst = 0;
      std::uint32_t value = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &value);
      write_bool_reg_stmt(dst, value != 0U ? "true" : "false");
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::Move: {
      std::uint32_t dst = 0;
      std::uint32_t src = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &src);
      const NativeScalarKind src_kind = scalar_kind(src, pc_scalar_state);
      if (src_kind == NativeScalarKind::Integer) {
        write_int_reg_stmt(dst, int_reg_expr(src, pc_scalar_state));
      } else if (src_kind == NativeScalarKind::Float) {
        write_float_reg_stmt(dst, double_reg_expr(src, pc_scalar_state));
      } else if (src_kind == NativeScalarKind::Bool &&
                 !uses_local_capture_cells) {
        write_bool_reg_stmt(dst, bool_lane_expr(src));
      } else {
        write_reg_stmt(dst, read_reg_expr(src));
      }
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::MakeList: {
      std::uint32_t dst = 0;
      std::uint32_t first_reg = 0;
      std::uint32_t count = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &first_reg);
      operand_u32_value(instruction, 2, &count);
      std::ostringstream expr;
      expr << "NativeValue::list(std::vector<NativeValue>{";
      for (std::uint32_t index = 0; index < count; ++index) {
        if (index != 0U) {
          expr << ", ";
        }
        expr << read_reg_expr(first_reg + index);
      }
      expr << "})";
      write_reg_stmt(dst, expr.str());
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::MakeSet: {
      std::uint32_t dst = 0;
      std::uint32_t first_reg = 0;
      std::uint32_t count = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &first_reg);
      operand_u32_value(instruction, 2, &count);
      std::ostringstream expr;
      expr << "native_set_from_items(std::vector<NativeValue>{";
      for (std::uint32_t index = 0; index < count; ++index) {
        if (index != 0U) {
          expr << ", ";
        }
        expr << read_reg_expr(first_reg + index);
      }
      expr << "})";
      write_reg_stmt(dst, expr.str());
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::MakeTuple: {
      std::uint32_t dst = 0;
      std::uint32_t first_reg = 0;
      std::uint32_t count = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &first_reg);
      operand_u32_value(instruction, 2, &count);
      std::ostringstream expr;
      expr << "NativeValue::tuple(std::vector<NativeValue>{";
      for (std::uint32_t index = 0; index < count; ++index) {
        if (index != 0U) {
          expr << ", ";
        }
        expr << read_reg_expr(first_reg + index);
      }
      expr << "})";
      write_reg_stmt(dst, expr.str());
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::MakeSetSpread: {
      std::uint32_t dst = 0;
      std::uint32_t count = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &count);
      out << "  {\n";
      out << "    std::vector<NativeValue> items;\n";
      std::size_t operand_index = 2U;
      for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t kind = 0;
        std::uint32_t value_reg = 0;
        operand_u32_value(instruction, operand_index++, &kind);
        operand_u32_value(instruction, operand_index++, &value_reg);
        if (kind == amber::bytecode::kSpreadOperandValue) {
          out << "    items.push_back(" << read_reg_expr(value_reg) << ");\n";
        } else if (kind == amber::bytecode::kSpreadOperandExpand) {
          out << "    native_set_spread_append(items, "
              << read_reg_expr(value_reg) << ");\n";
        } else {
          out << "    throw NativeBailout();\n";
        }
      }
      out << "    ";
      if (uses_local_capture_cells) {
        out << "write_reg(frame, " << dst
            << ", native_set_from_items(std::move(items)));\n";
      } else {
        out << "frame.regs[" << dst
            << "] = native_set_from_items(std::move(items));\n";
      }
      out << "  }\n";
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::MakeMap: {
      std::uint32_t dst = 0;
      std::uint32_t count = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &count);
      const bool strict = (count & amber::bytecode::kMapStrictCountFlag) != 0U;
      count &= amber::bytecode::kMapCountMask;
      out << "  {\n";
      out << "    std::vector<std::pair<NativeValue, NativeValue>> entries;\n";
      out << "    entries.reserve(" << count << ");\n";
      std::size_t operand_index = 2U;
      for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t symbol_id = 0;
        std::uint32_t value_reg = 0;
        operand_u32_value(instruction, operand_index++, &symbol_id);
        operand_u32_value(instruction, operand_index++, &value_reg);
        out << "    native_map_store(entries, NativeValue::symbol_ref("
            << symbol_id << "), " << read_reg_expr(value_reg) << ", "
            << (strict ? "true" : "false") << ");\n";
      }
      out << "    ";
      if (uses_local_capture_cells) {
        out << "write_reg(frame, " << dst
            << ", NativeValue::map_entries(std::move(entries), "
            << (strict ? "true" : "false") << "));\n";
      } else {
        out << "frame.regs[" << dst
            << "] = NativeValue::map_entries(std::move(entries), "
            << (strict ? "true" : "false") << ");\n";
      }
      out << "  }\n";
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::MakeMapDyn: {
      std::uint32_t dst = 0;
      std::uint32_t count = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &count);
      const bool strict = (count & amber::bytecode::kMapStrictCountFlag) != 0U;
      count &= amber::bytecode::kMapCountMask;
      out << "  {\n";
      out << "    std::vector<std::pair<NativeValue, NativeValue>> entries;\n";
      out << "    entries.reserve(" << count << ");\n";
      std::size_t operand_index = 2U;
      for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t key_reg = 0;
        std::uint32_t value_reg = 0;
        operand_u32_value(instruction, operand_index++, &key_reg);
        operand_u32_value(instruction, operand_index++, &value_reg);
        out << "    native_map_store(entries, " << read_reg_expr(key_reg)
            << ", " << read_reg_expr(value_reg) << ", "
            << (strict ? "true" : "false") << ");\n";
      }
      out << "    ";
      if (uses_local_capture_cells) {
        out << "write_reg(frame, " << dst
            << ", NativeValue::map_entries(std::move(entries), "
            << (strict ? "true" : "false") << "));\n";
      } else {
        out << "frame.regs[" << dst
            << "] = NativeValue::map_entries(std::move(entries), "
            << (strict ? "true" : "false") << ");\n";
      }
      out << "  }\n";
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::MakeMapSpread: {
      std::uint32_t dst = 0;
      std::uint32_t count = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &count);
      const bool strict = (count & amber::bytecode::kMapStrictCountFlag) != 0U;
      count &= amber::bytecode::kMapCountMask;
      out << "  {\n";
      out << "    std::vector<std::pair<NativeValue, NativeValue>> entries;\n";
      std::size_t operand_index = 2U;
      for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t kind = 0;
        std::uint32_t key_or_symbol = 0;
        std::uint32_t value_reg = 0;
        operand_u32_value(instruction, operand_index++, &kind);
        operand_u32_value(instruction, operand_index++, &key_or_symbol);
        operand_u32_value(instruction, operand_index++, &value_reg);
        if (kind == amber::bytecode::kMapSpreadEntrySymbol) {
          out << "    native_map_store(entries, NativeValue::symbol_ref("
              << key_or_symbol << "), " << read_reg_expr(value_reg) << ", "
              << (strict ? "true" : "false") << ");\n";
        } else if (kind == amber::bytecode::kMapSpreadEntryDynamic) {
          out << "    native_map_store(entries, "
              << read_reg_expr(key_or_symbol) << ", "
              << read_reg_expr(value_reg) << ", " << (strict ? "true" : "false")
              << ");\n";
        } else if (kind == amber::bytecode::kMapSpreadEntrySpread) {
          out << "    native_map_append_entries(entries, "
              << read_reg_expr(value_reg) << ", " << (strict ? "true" : "false")
              << ");\n";
        } else {
          out << "    throw NativeBailout();\n";
        }
      }
      out << "    ";
      if (uses_local_capture_cells) {
        out << "write_reg(frame, " << dst
            << ", NativeValue::map_entries(std::move(entries), "
            << (strict ? "true" : "false") << "));\n";
      } else {
        out << "frame.regs[" << dst
            << "] = NativeValue::map_entries(std::move(entries), "
            << (strict ? "true" : "false") << ");\n";
      }
      out << "  }\n";
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::LookupConst: {
      std::uint32_t dst = 0;
      std::uint32_t const_id = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &const_id);
      std::string native_module_expr;
      if (const_id < module.const_pool.size() &&
          module.const_pool[const_id].kind ==
              amber::bytecode::ConstantKind::Path &&
          module.const_pool[const_id].items.size() == 1U) {
        const std::uint32_t symbol_id = module.const_pool[const_id].items[0];
        if (symbol_id < module.symbols.size()) {
          const std::string &name = module.symbols[symbol_id];
          if (name == "Str") {
            native_module_expr = "NativeValue::str_type()";
          } else if (name == "Int") {
            native_module_expr = "NativeValue::int_type()";
          } else if (name == "BigInt") {
            native_module_expr = "NativeValue::bigint_type()";
          } else if (name == "Float") {
            native_module_expr = "NativeValue::float_type()";
          } else if (name == "Bool") {
            native_module_expr = "NativeValue::bool_type()";
          } else if (name == "Symbol") {
            native_module_expr = "NativeValue::symbol_type()";
          } else if (name == "Null") {
            native_module_expr = "NativeValue::null_type()";
          } else if (name == "Object") {
            native_module_expr = "NativeValue::object_type()";
          } else if (name == "Math") {
            native_module_expr = "NativeValue::math_module()";
          } else if (name == "Json") {
            native_module_expr = "NativeValue::json_module()";
          } else if (name == "Yaml") {
            native_module_expr = "NativeValue::yaml_module()";
          } else if (name == "Bytes") {
            native_module_expr = "NativeValue::bytes_module()";
          } else if (name == "Base64") {
            native_module_expr = "NativeValue::base64_module()";
          } else if (name == "Base64Url") {
            native_module_expr = "NativeValue::base64url_module()";
          } else if (name == "Hex") {
            native_module_expr = "NativeValue::hex_module()";
          } else if (name == "Digest") {
            native_module_expr = "NativeValue::digest_module()";
          } else if (name == "Benchmark") {
            native_module_expr = "NativeValue::benchmark_module()";
          } else if (name == "Url") {
            native_module_expr = "NativeValue::url_module()";
          } else if (name == "ArgParser") {
            native_module_expr = "NativeValue::argparser_module()";
          } else if (name == "Regexp") {
            native_module_expr = "NativeValue::regexp_module()";
          } else if (name == "fs") {
            native_module_expr = "NativeValue::fs_module()";
          } else if (name == "SecureRandom") {
            native_module_expr = "NativeValue::secure_random_module()";
          } else if (name == "Uuid" || name == "UUID") {
            native_module_expr = "NativeValue::uuid_module()";
          } else if (name == "Range") {
            native_module_expr = "NativeValue::range_module()";
          } else if (name == "Time") {
            native_module_expr = "NativeValue::time_module()";
          } else if (name == "TimePeriod") {
            native_module_expr = "NativeValue::time_period_module()";
          }
        }
      }
      if (!native_module_expr.empty()) {
        write_reg_stmt(dst, native_module_expr);
      } else {
        out << "  throw NativeBailout();\n";
      }
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::GetLast: {
      std::uint32_t dst = 0;
      operand_u32_value(instruction, 0, &dst);
      write_reg_stmt(dst, "frame.last");
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::SetLast: {
      std::uint32_t src = 0;
      operand_u32_value(instruction, 0, &src);
      out << "  frame.last = " << read_reg_expr(src) << ";\n";
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::LoadUpval: {
      std::uint32_t dst = 0;
      std::uint32_t slot = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &slot);
      write_reg_stmt(dst, "read_capture(frame, " + std::to_string(slot) + ")");
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::StoreUpval: {
      std::uint32_t slot = 0;
      std::uint32_t src = 0;
      operand_u32_value(instruction, 0, &slot);
      operand_u32_value(instruction, 1, &src);
      out << "  write_capture(frame, " << slot << ", " << read_reg_expr(src)
          << ");\n";
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::MakeClosure: {
      std::uint32_t dst = 0;
      std::uint32_t code_id = 0;
      std::uint32_t capture_count = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &code_id);
      operand_u32_value(instruction, 2, &capture_count);
      bool self_capture = false;
      std::size_t operand_index = 3U;
      for (std::uint32_t capture_i = 0; capture_i < capture_count;
           ++capture_i) {
        std::uint32_t kind = 0;
        std::uint32_t slot = 0;
        operand_u32_value(instruction, operand_index++, &kind);
        operand_u32_value(instruction, operand_index++, &slot);
        self_capture = self_capture || (kind == 0U && slot == dst);
      }
      out << "  {\n";
      out << "    NativeClosure *next_closure = make_native_closure();\n";
      out << "    next_closure->code_id = " << code_id << ";\n";
      out << "    next_closure->self = frame.self;\n";
      out << "    next_closure->captures.reserve(" << capture_count << ");\n";
      out << "    NativeValue closure_value = "
             "NativeValue::closure(next_closure);\n";
      if (self_capture) {
        if (uses_local_capture_cells) {
          out << "    write_reg(frame, " << dst << ", closure_value);\n";
        } else {
          out << "    frame.regs[" << dst << "] = closure_value;\n";
        }
      }
      operand_index = 3U;
      for (std::uint32_t capture_i = 0; capture_i < capture_count;
           ++capture_i) {
        std::uint32_t kind = 0;
        std::uint32_t slot = 0;
        operand_u32_value(instruction, operand_index++, &kind);
        operand_u32_value(instruction, operand_index++, &slot);
        if (kind == 0U) {
          out << "    next_closure->captures.push_back(local_cell(frame, "
              << slot << "));\n";
        } else {
          out << "    next_closure->captures.push_back(capture_cell(frame, "
              << slot << "));\n";
        }
      }
      if (uses_local_capture_cells) {
        out << "    write_reg(frame, " << dst << ", closure_value);\n";
      } else {
        out << "    frame.regs[" << dst << "] = closure_value;\n";
      }
      out << "  }\n";
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::Call: {
      std::uint32_t dst = 0;
      std::uint32_t callee = 0;
      std::uint32_t pos_count = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &callee);
      operand_u32_value(instruction, 2, &pos_count);
      std::ostringstream expr;
      expr << "amber_native_call_closure(" << read_reg_expr(callee) << ", {";
      for (std::uint32_t index = 0; index < pos_count; ++index) {
        std::uint32_t arg_reg = 0;
        operand_u32_value(instruction, 3U + index, &arg_reg);
        if (index != 0U) {
          expr << ", ";
        }
        expr << read_reg_expr(arg_reg);
      }
      expr << "})";
      write_reg_stmt(dst, expr.str());
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::Send: {
      std::uint32_t dst = 0;
      std::uint32_t recv = 0;
      std::uint32_t symbol_id = 0;
      std::uint32_t pos_count = 0;
      std::uint32_t arg = 0;
      std::uint32_t arg2 = 0;
      std::uint32_t arg3 = 0;
      std::uint32_t kw_count = 0;
      std::uint32_t kw_symbol_id = 0;
      std::uint32_t kw_value_reg = 0;
      std::uint32_t kw_symbol_id2 = 0;
      std::uint32_t kw_value_reg2 = 0;
      std::int64_t block_reg = -1;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &recv);
      operand_u32_value(instruction, 2, &symbol_id);
      operand_u32_value(instruction, 3, &pos_count);
      if (pos_count != 0U) {
        operand_u32_value(instruction, 4, &arg);
      }
      if (pos_count > 1U) {
        operand_u32_value(instruction, 5, &arg2);
      }
      if (pos_count > 2U) {
        operand_u32_value(instruction, 6, &arg3);
      }
      const std::size_t kw_index = 4U + pos_count;
      operand_u32_value(instruction, kw_index, &kw_count);
      if (kw_count >= 1U) {
        operand_u32_value(instruction, kw_index + 1U, &kw_symbol_id);
        operand_u32_value(instruction, kw_index + 2U, &kw_value_reg);
      }
      if (kw_count >= 2U) {
        operand_u32_value(instruction, kw_index + 3U, &kw_symbol_id2);
        operand_u32_value(instruction, kw_index + 4U, &kw_value_reg2);
      }
      const std::size_t block_index = kw_index + 1U + kw_count * 2U;
      const bool no_block = block_index >= instruction.operands.size() ||
                            operand_is_no_block(instruction, block_index);
      if (!no_block) {
        block_reg = instruction.operands[block_index].value;
      }
      const bool has_block = !no_block;
      const std::string selector = module.symbols[symbol_id];
      const auto pos_args_expr = [&](std::uint32_t first_index) {
        std::ostringstream args_expr;
        args_expr << "{";
        for (std::uint32_t index = first_index; index < pos_count; ++index) {
          std::uint32_t arg_reg = 0;
          operand_u32_value(instruction, 4U + index, &arg_reg);
          if (index != first_index) {
            args_expr << ", ";
          }
          args_expr << read_reg_expr(arg_reg);
        }
        args_expr << "}";
        return args_expr.str();
      };
      const auto kw_args_expr = [&]() {
        std::ostringstream args_expr;
        args_expr << "{";
        for (std::uint32_t index = 0; index < kw_count; ++index) {
          std::uint32_t kw_symbol = 0;
          std::uint32_t kw_value = 0;
          operand_u32_value(instruction, kw_index + 1U + index * 2U,
                            &kw_symbol);
          operand_u32_value(instruction, kw_index + 2U + index * 2U, &kw_value);
          if (index != 0U) {
            args_expr << ", ";
          }
          const std::string kw_name = kw_symbol < module.symbols.size()
                                          ? module.symbols[kw_symbol]
                                          : std::string{};
          args_expr << "std::pair<std::string, NativeValue>{"
                    << "native_hex_to_string(\"" << string_to_hex_text(kw_name)
                    << "\"), " << read_reg_expr(kw_value) << "}";
        }
        args_expr << "}";
        return args_expr.str();
      };
      if (native_cpp_math_selector(selector, pos_count)) {
        write_reg_stmt(dst, "native_math_send(" + read_reg_expr(recv) +
                                ", native_hex_to_string(\"" +
                                string_to_hex_text(selector) + "\"), " +
                                pos_args_expr(0U) + ")");
      } else if (selector == "+") {
        write_reg_stmt(dst, "native_numeric_fast_add(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "-") {
        write_reg_stmt(dst, "native_numeric_fast_sub(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "*") {
        write_reg_stmt(dst, "native_numeric_fast_mul(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "/") {
        write_reg_stmt(dst, "native_numeric_fast_div(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "%") {
        write_reg_stmt(dst, "native_numeric_fast_mod(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "//") {
        write_reg_stmt(dst, "native_numeric_fast_floor_div(" +
                                read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ")");
      } else if (selector == "<") {
        write_reg_stmt(dst, "native_numeric_fast_lt(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == ">") {
        write_reg_stmt(dst, "native_numeric_fast_gt(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "<=") {
        write_reg_stmt(dst, "native_numeric_fast_le(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == ">=") {
        write_reg_stmt(dst, "native_numeric_fast_ge(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "==") {
        write_reg_stmt(dst, "native_numeric_fast_eq(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ", false)");
      } else if (selector == "!=") {
        write_reg_stmt(dst, "native_numeric_fast_eq(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ", true)");
      } else if (selector == "=~") {
        write_reg_stmt(dst, "native_regexp_match_operator(" +
                                read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ", false)");
      } else if (selector == "!~") {
        write_reg_stmt(dst, "native_regexp_match_operator(" +
                                read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ", true)");
      } else if (selector == "<=>") {
        write_reg_stmt(dst, "native_numeric_fast_cmp(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "&") {
        write_reg_stmt(dst, "NativeValue::integer(bit_and_int64(as_int(" +
                                read_reg_expr(recv) + "), as_int(" +
                                read_reg_expr(arg) + ")))");
      } else if (selector == "|") {
        write_reg_stmt(dst, "NativeValue::integer(bit_or_int64(as_int(" +
                                read_reg_expr(recv) + "), as_int(" +
                                read_reg_expr(arg) + ")))");
      } else if (selector == "^") {
        write_reg_stmt(dst, "NativeValue::integer(bit_xor_int64(as_int(" +
                                read_reg_expr(recv) + "), as_int(" +
                                read_reg_expr(arg) + ")))");
      } else if (selector == "**") {
        write_reg_stmt(dst, "native_numeric_fast_pow(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "<<") {
        out << "  if (as_int(" << read_reg_expr(arg) << ") < 0 || as_int("
            << read_reg_expr(arg) << ") >= 64) throw NativeBailout();\n";
        write_reg_stmt(dst, "NativeValue::integer(profile_shl_int64(as_int(" +
                                read_reg_expr(recv) + "), as_int(" +
                                read_reg_expr(arg) + ")))");
      } else if (selector == ">>") {
        out << "  if (as_int(" << read_reg_expr(arg) << ") < 0 || as_int("
            << read_reg_expr(arg) << ") >= 64) throw NativeBailout();\n";
        write_reg_stmt(dst, "NativeValue::integer(shr_int64(as_int(" +
                                read_reg_expr(recv) + "), as_int(" +
                                read_reg_expr(arg) + ")))");
      } else if (selector == "u+") {
        write_reg_stmt(dst, "native_unary_plus(" + read_reg_expr(recv) + ")");
      } else if (selector == "u-") {
        write_reg_stmt(dst, "native_unary_minus(" + read_reg_expr(recv) + ")");
      } else if (selector == "abs") {
        write_reg_stmt(dst, "native_abs(" + read_reg_expr(recv) + ")");
      } else if (selector == "class") {
        write_reg_stmt(dst, "native_value_class(" + read_reg_expr(recv) + ")");
      } else if (native_cpp_benchmark_selector(selector, pos_count,
                                               has_block)) {
        write_reg_stmt(
            dst, "native_benchmark_send(" + read_reg_expr(recv) +
                     ", native_hex_to_string(\"" +
                     string_to_hex_text(selector) + "\"), " +
                     pos_args_expr(0U) + ", " + kw_args_expr() + ", " +
                     (has_block
                          ? read_reg_expr(static_cast<std::uint32_t>(block_reg))
                          : "NativeValue::nullv()") +
                     ", " + (has_block ? "true" : "false") + ")");
      } else if (selector == "Path") {
        write_reg_stmt(dst, "native_fs_path_new(" + read_reg_expr(recv) + ", " +
                                (pos_count == 1U ? read_reg_expr(arg)
                                                 : "NativeValue::nullv()") +
                                ", " + (pos_count == 1U ? "true" : "false") +
                                ")");
      } else if (selector == "/" || selector == "join") {
        write_reg_stmt(dst, "native_fs_path_join(" + read_reg_expr(recv) +
                                ", " + pos_args_expr(0U) + ")");
      } else if (selector == "basename" || selector == "extname" ||
                 selector == "parent" || selector == "absolute?" ||
                 selector == "normalize") {
        write_reg_stmt(dst, "native_fs_path_nullary(" + read_reg_expr(recv) +
                                ", native_hex_to_string(\"" +
                                string_to_hex_text(selector) + "\"))");
      } else if (native_cpp_regexp_selector(selector, pos_count) &&
                 selector != "[]" && selector != "count" &&
                 selector != "length" && selector != "size" &&
                 selector != "to_str" && selector != "=~" && selector != "!~") {
        write_reg_stmt(dst, "native_regexp_send(" + read_reg_expr(recv) +
                                ", native_hex_to_string(\"" +
                                string_to_hex_text(selector) + "\"), " +
                                pos_args_expr(0U) + ", " + kw_args_expr() +
                                ")");
      } else if (selector == "[]") {
        write_reg_stmt(dst, "native_index(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ")");
      } else if (selector == "[]?") {
        write_reg_stmt(dst, "native_optional_index(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "[]=") {
        write_reg_stmt(dst, "native_index_set(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ", " +
                                read_reg_expr(arg2) + ")");
      } else if (selector == "has_index?") {
        write_reg_stmt(dst, "native_has_index(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ")");
      } else if (selector == "each") {
        if (!has_block) {
          out << "  throw NativeBailout();\n";
        } else {
          write_reg_stmt(
              dst, "native_each(" + read_reg_expr(recv) + ", " +
                       read_reg_expr(static_cast<std::uint32_t>(block_reg)) +
                       ")");
        }
      } else if (selector == "map" || selector == "select" ||
                 selector == "reject" || selector == "find" ||
                 selector == "transform_keys" ||
                 selector == "transform_values") {
        if (!has_block) {
          out << "  throw NativeBailout();\n";
        } else {
          write_reg_stmt(
              dst, "native_sequence_higher_order(" + read_reg_expr(recv) +
                       ", " +
                       read_reg_expr(static_cast<std::uint32_t>(block_reg)) +
                       ", native_hex_to_string(\"" +
                       string_to_hex_text(selector) + "\"))");
        }
      } else if (selector == "any?" || selector == "all?" ||
                 selector == "none?") {
        write_reg_stmt(
            dst, "native_sequence_predicate(" + read_reg_expr(recv) + ", " +
                     (has_block
                          ? read_reg_expr(static_cast<std::uint32_t>(block_reg))
                          : "NativeValue::nullv()") +
                     ", " + (has_block ? "true" : "false") +
                     ", native_hex_to_string(\"" +
                     string_to_hex_text(selector) + "\"))");
      } else if (selector == "reduce") {
        if (!has_block) {
          out << "  throw NativeBailout();\n";
        } else {
          write_reg_stmt(
              dst, "native_sequence_reduce(" + read_reg_expr(recv) + ", " +
                       (pos_count == 1U ? read_reg_expr(arg)
                                        : "NativeValue::nullv()") +
                       ", " + (pos_count == 1U ? "true" : "false") + ", " +
                       read_reg_expr(static_cast<std::uint32_t>(block_reg)) +
                       ")");
        }
      } else if (selector == "count") {
        write_reg_stmt(dst, "native_count(" + read_reg_expr(recv) + ")");
      } else if (selector == "empty?") {
        write_reg_stmt(dst, "native_empty(" + read_reg_expr(recv) + ")");
      } else if (selector == "length" || selector == "size") {
        write_reg_stmt(dst, "native_length(" + read_reg_expr(recv) + ")");
      } else if (selector == "bytesize") {
        write_reg_stmt(dst, "native_bytesize(" + read_reg_expr(recv) + ")");
      } else if (selector == "first") {
        write_reg_stmt(dst, "native_list_first(" + read_reg_expr(recv) + ", " +
                                (pos_count == 1U ? read_reg_expr(arg)
                                                 : "NativeValue::nullv()") +
                                ", " + (pos_count == 1U ? "true" : "false") +
                                ")");
      } else if (selector == "deconstruct") {
        write_reg_stmt(dst, "native_deconstruct(" + read_reg_expr(recv) + ")");
      } else if (selector == "to_array") {
        write_reg_stmt(dst, "native_to_array(" + read_reg_expr(recv) + ")");
      } else if (selector == "keys") {
        write_reg_stmt(dst, "native_map_keys(" + read_reg_expr(recv) + ")");
      } else if (selector == "values") {
        write_reg_stmt(dst, "native_map_values(" + read_reg_expr(recv) + ")");
      } else if (selector == "entries") {
        write_reg_stmt(dst,
                       "native_map_entries_array(" + read_reg_expr(recv) + ")");
      } else if (selector == "bytes" && pos_count == 0U) {
        write_reg_stmt(dst, "native_bytes_new(" + read_reg_expr(recv) + ")");
      } else if (selector == "contains?" || selector == "includes?" ||
                 selector == "include?" || selector == "member?" ||
                 selector == "has_key?" || selector == "key?") {
        write_reg_stmt(dst, "native_contains(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ")");
      } else if (selector == "value?" || selector == "has_value?") {
        write_reg_stmt(dst, "native_map_has_value(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "union" || selector == "intersection" ||
                 selector == "difference" || selector == "left_difference" ||
                 selector == "symmetric_difference" || selector == "subset?" ||
                 selector == "proper_subset?" || selector == "superset?" ||
                 selector == "proper_superset?" || selector == "disjoint?") {
        write_reg_stmt(dst, "native_set_operation(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) +
                                ", native_hex_to_string(\"" +
                                string_to_hex_text(selector) + "\"))");
      } else if (selector == "starts_with?") {
        write_reg_stmt(dst, "native_string_starts_with(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "ends_with?") {
        write_reg_stmt(dst, "native_string_ends_with(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "split") {
        write_reg_stmt(dst, "native_string_split(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "replace" || selector == "replaced") {
        write_reg_stmt(
            dst, "native_string_replace(" + read_reg_expr(recv) + ", " +
                     read_reg_expr(arg) + ", " +
                     (pos_count >= 2U ? read_reg_expr(arg2)
                                      : "NativeValue::nullv()") +
                     ", " + (pos_count >= 2U ? "true" : "false") + ", " +
                     (has_block
                          ? read_reg_expr(static_cast<std::uint32_t>(block_reg))
                          : "NativeValue::nullv()") +
                     ", " + (has_block ? "true" : "false") + ", " +
                     kw_args_expr() + ")");
      } else if (selector == "slice") {
        write_reg_stmt(dst, "native_slice(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ", " +
                                (pos_count == 2U ? read_reg_expr(arg2)
                                                 : "NativeValue::nullv()") +
                                ", " + (pos_count == 2U ? "true" : "false") +
                                ")");
      } else if (selector == "appended") {
        write_reg_stmt(dst, "native_sequence_appended(" + read_reg_expr(recv) +
                                ", " + pos_args_expr(0U) + ")");
      } else if (selector == "added") {
        write_reg_stmt(dst, "native_sequence_added(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "add!") {
        write_reg_stmt(dst, "native_set_add_mut(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ")");
      } else if (selector == "subtract!") {
        write_reg_stmt(dst, "native_set_subtract_mut(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "with") {
        write_reg_stmt(dst, "native_map_with(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ", " +
                                read_reg_expr(arg2) + ")");
      } else if (selector == "without" || selector == "except") {
        write_reg_stmt(dst, "native_map_except(" + read_reg_expr(recv) + ", " +
                                pos_args_expr(0U) + ")");
      } else if (selector == "merge") {
        write_reg_stmt(dst, "native_map_merge(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ")");
      } else if (selector == "compact") {
        write_reg_stmt(dst, "native_map_compact(" + read_reg_expr(recv) + ")");
      } else if (selector == "inserted") {
        write_reg_stmt(dst, "native_sequence_inserted(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ", " +
                                pos_args_expr(1U) + ")");
      } else if (selector == "deleted") {
        write_reg_stmt(dst, "native_sequence_deleted(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "reversed") {
        write_reg_stmt(dst,
                       "native_sequence_reversed(" + read_reg_expr(recv) + ")");
      } else if (selector == "init") {
        write_reg_stmt(dst,
                       "native_sequence_init(" + read_reg_expr(recv) + ")");
      } else if (selector == "tail") {
        write_reg_stmt(dst,
                       "native_sequence_tail(" + read_reg_expr(recv) + ")");
      } else if (selector == "take") {
        write_reg_stmt(dst, "native_sequence_take(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "drop") {
        write_reg_stmt(dst, "native_sequence_drop(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "sorted") {
        write_reg_stmt(dst,
                       "native_sequence_sorted(" + read_reg_expr(recv) + ")");
      } else if (selector == "min" || selector == "max" ||
                 selector == "minmax") {
        write_reg_stmt(dst, "native_sequence_extreme(" + read_reg_expr(recv) +
                                ", native_hex_to_string(\"" +
                                string_to_hex_text(selector) + "\"))");
      } else if (selector == "upcase") {
        write_reg_stmt(dst,
                       "native_string_case(" + read_reg_expr(recv) + ", true)");
      } else if (selector == "downcase") {
        write_reg_stmt(dst, "native_string_case(" + read_reg_expr(recv) +
                                ", false)");
      } else if (selector == "trim" || selector == "strip") {
        write_reg_stmt(dst, "native_string_trim(" + read_reg_expr(recv) + ")");
      } else if (selector == "reverse") {
        write_reg_stmt(dst,
                       "native_string_reverse(" + read_reg_expr(recv) + ")");
      } else if (selector == "chars") {
        write_reg_stmt(dst, "native_string_chars(" + read_reg_expr(recv) + ")");
      } else if (selector == "concat") {
        write_reg_stmt(dst, "native_concat(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ")");
      } else if (selector == "to_str") {
        write_reg_stmt(dst, "native_to_str(" + read_reg_expr(recv) + ")");
      } else if (selector == "to_int") {
        write_reg_stmt(dst, "native_to_int(" + read_reg_expr(recv) + ")");
      } else if (selector == "to_float") {
        write_reg_stmt(dst, "native_to_float(" + read_reg_expr(recv) + ")");
      } else if (selector == "to_bool") {
        write_reg_stmt(dst, "native_to_bool(" + read_reg_expr(recv) + ")");
      } else if (selector == "to_symbol") {
        write_reg_stmt(dst, "native_to_symbol(" + read_reg_expr(recv) + ")");
      } else if (selector == "version") {
        write_reg_stmt(dst, "native_uuid_nullary(" + read_reg_expr(recv) +
                                ", native_hex_to_string(\"" +
                                string_to_hex_text(selector) + "\"))");
      } else if (selector == "v4") {
        write_reg_stmt(dst, "native_uuid_v4(" + read_reg_expr(recv) + ")");
      } else if (selector == "v7") {
        write_reg_stmt(dst, "native_uuid_v7(" + read_reg_expr(recv) + ")");
      } else if (selector == "===") {
        write_reg_stmt(dst, "native_type_matches(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "to_json") {
        write_reg_stmt(dst, "native_benchmark_send(" + read_reg_expr(recv) +
                                ", \"to_json\", " + pos_args_expr(0U) + ", " +
                                kw_args_expr() +
                                ", NativeValue::nullv(), false)");
      } else if (selector == "utc") {
        const auto kw_name = [&](std::uint32_t symbol_id) -> std::string {
          return symbol_id < module.symbols.size() ? module.symbols[symbol_id]
                                                   : std::string{};
        };
        std::string hour_expr = "NativeValue::nullv()";
        std::string minute_expr = "NativeValue::nullv()";
        std::string second_expr = "NativeValue::nullv()";
        std::string nanosecond_expr = "NativeValue::nullv()";
        bool has_hour = false;
        bool has_minute = false;
        bool has_second = false;
        bool has_nanosecond = false;
        auto apply_time_kw = [&](const std::string &name,
                                 const std::string &expr) {
          if (name == "hour") {
            hour_expr = expr;
            has_hour = true;
          } else if (name == "minute") {
            minute_expr = expr;
            has_minute = true;
          } else if (name == "second") {
            second_expr = expr;
            has_second = true;
          } else if (name == "nanosecond") {
            nanosecond_expr = expr;
            has_nanosecond = true;
          }
        };
        if (kw_count >= 1U) {
          apply_time_kw(kw_name(kw_symbol_id), read_reg_expr(kw_value_reg));
        }
        if (kw_count >= 2U) {
          apply_time_kw(kw_name(kw_symbol_id2), read_reg_expr(kw_value_reg2));
        }
        if (kw_count > 2U) {
          std::uint32_t kw_symbol_id3 = 0;
          std::uint32_t kw_value_reg3 = 0;
          std::uint32_t kw_symbol_id4 = 0;
          std::uint32_t kw_value_reg4 = 0;
          if (kw_count >= 3U) {
            operand_u32_value(instruction, kw_index + 5U, &kw_symbol_id3);
            operand_u32_value(instruction, kw_index + 6U, &kw_value_reg3);
            apply_time_kw(kw_name(kw_symbol_id3), read_reg_expr(kw_value_reg3));
          }
          if (kw_count >= 4U) {
            operand_u32_value(instruction, kw_index + 7U, &kw_symbol_id4);
            operand_u32_value(instruction, kw_index + 8U, &kw_value_reg4);
            apply_time_kw(kw_name(kw_symbol_id4), read_reg_expr(kw_value_reg4));
          }
        }
        write_reg_stmt(
            dst, "native_time_utc(" + read_reg_expr(recv) + ", " +
                     read_reg_expr(arg) + ", " + read_reg_expr(arg2) + ", " +
                     read_reg_expr(arg3) + ", " + hour_expr + ", " +
                     (has_hour ? "true" : "false") + ", " + minute_expr + ", " +
                     (has_minute ? "true" : "false") + ", " + second_expr +
                     ", " + (has_second ? "true" : "false") + ", " +
                     nanosecond_expr + ", " +
                     (has_nanosecond ? "true" : "false") + ")");
      } else if (selector == "from_unix") {
        write_reg_stmt(dst, "native_time_from_unix(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ", " +
                                (kw_count == 1U ? read_reg_expr(kw_value_reg)
                                                : "NativeValue::nullv()") +
                                ", " + (kw_count == 1U ? "true" : "false") +
                                ")");
      } else if (selector == "from_unix_ms") {
        write_reg_stmt(dst, "native_time_from_unix_ms(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "from_unix_ns") {
        write_reg_stmt(dst, "native_time_from_unix_ns(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "parse") {
        write_reg_stmt(dst, "native_parse_value(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ")");
      } else if (selector == "build") {
        write_reg_stmt(dst, "native_url_build(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ")");
      } else if (selector == "percent_encode") {
        write_reg_stmt(dst, "native_url_percent_encode(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "percent_decode") {
        write_reg_stmt(dst, "native_url_percent_decode(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "parse_query") {
        write_reg_stmt(dst, "native_url_parse_query(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "build_query") {
        write_reg_stmt(dst, "native_url_build_query(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (selector == "generate" || selector == "pretty_generate") {
        write_reg_stmt(
            dst, "native_generate_value(" + read_reg_expr(recv) + ", " +
                     read_reg_expr(arg) + ", " +
                     (selector == "pretty_generate" ? "true" : "false") + ")");
      } else if (selector == "stream_parse_file") {
        if (!has_block) {
          out << "  throw NativeBailout();\n";
        } else {
          write_reg_stmt(
              dst, "native_json_stream_parse_file(" + read_reg_expr(arg) +
                       ", " + read_reg_expr(kw_value_reg) + ", " +
                       read_reg_expr(static_cast<std::uint32_t>(block_reg)) +
                       ")");
        }
      } else if (selector == "bytes") {
        write_reg_stmt(dst, "native_secure_random_bytes_value(" +
                                read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) + ")");
      } else if (selector == "base64") {
        write_reg_stmt(
            dst, "native_secure_random_base64(" + read_reg_expr(recv) + ", " +
                     read_reg_expr(arg) + ", " +
                     (kw_count == 1U ? read_reg_expr(kw_value_reg)
                                     : "NativeValue::nullv()") +
                     ", " + (kw_count == 1U ? "true" : "false") + ", false)");
      } else if (selector == "base64url") {
        write_reg_stmt(
            dst, "native_secure_random_base64(" + read_reg_expr(recv) + ", " +
                     read_reg_expr(arg) + ", " +
                     (kw_count == 1U ? read_reg_expr(kw_value_reg)
                                     : "NativeValue::nullv()") +
                     ", " + (kw_count == 1U ? "true" : "false") + ", true)");
      } else if (selector == "uuid") {
        write_reg_stmt(dst, "native_secure_random_uuid(" + read_reg_expr(recv) +
                                ")");
      } else if (selector == "new" && pos_count == 0U) {
        write_reg_stmt(dst, "native_argparser_new(" + read_reg_expr(recv) +
                                ", " + kw_args_expr() + ")");
      } else if ((selector == "name" || selector == "about") &&
                 pos_count == 1U) {
        write_reg_stmt(dst, "native_argparser_named(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) +
                                ", native_hex_to_string(\"" +
                                string_to_hex_text(selector) + "\"))");
      } else if (selector == "arg" || selector == "flag") {
        write_reg_stmt(dst, "native_argparser_option(" + read_reg_expr(recv) +
                                ", native_hex_to_string(\"" +
                                string_to_hex_text(selector) + "\"), " +
                                pos_args_expr(0U) + ", " + kw_args_expr() +
                                ")");
      } else if (selector == "pos" || selector == "rest") {
        write_reg_stmt(dst,
                       "native_argparser_positional(" + read_reg_expr(recv) +
                           ", native_hex_to_string(\"" +
                           string_to_hex_text(selector) + "\"), " +
                           read_reg_expr(arg) + ", " + kw_args_expr() + ")");
      } else if (selector == "parse_or_raise") {
        write_reg_stmt(dst, "native_argparser_parse_or_raise(" +
                                read_reg_expr(recv) + ", " + kw_args_expr() +
                                ")");
      } else if (selector == "new") {
        if (pos_count == 1U) {
          write_reg_stmt(dst, "native_bytes_new(" + read_reg_expr(arg) + ")");
        } else {
          const auto kw_name = [&](std::uint32_t symbol_id) -> std::string {
            return symbol_id < module.symbols.size() ? module.symbols[symbol_id]
                                                     : std::string{};
          };
          const bool first_inclusive =
              kw_count >= 1U && kw_name(kw_symbol_id) == "inclusive_end";
          const bool first_step =
              kw_count >= 1U && kw_name(kw_symbol_id) == "step";
          const bool second_inclusive =
              kw_count >= 2U && kw_name(kw_symbol_id2) == "inclusive_end";
          const bool second_step =
              kw_count >= 2U && kw_name(kw_symbol_id2) == "step";
          const std::string inclusive_expr =
              first_inclusive ? read_reg_expr(kw_value_reg)
                              : (second_inclusive ? read_reg_expr(kw_value_reg2)
                                                  : "NativeValue::nullv()");
          const std::string step_expr =
              first_step ? read_reg_expr(kw_value_reg)
                         : (second_step ? read_reg_expr(kw_value_reg2)
                                        : "NativeValue::nullv()");
          write_reg_stmt(
              dst,
              "native_range_new(" + read_reg_expr(recv) + ", " +
                  read_reg_expr(arg) + ", " + read_reg_expr(arg2) + ", " +
                  inclusive_expr + ", " +
                  (first_inclusive || second_inclusive ? "true" : "false") +
                  ", " + step_expr + ", " +
                  (first_step || second_step ? "true" : "false") + ")");
        }
      } else if (selector == "encode") {
        write_reg_stmt(dst, "native_codec_encode(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ", " +
                                (kw_count == 1U ? read_reg_expr(kw_value_reg)
                                                : "NativeValue::nullv()") +
                                ", " + (kw_count == 1U ? "true" : "false") +
                                ")");
      } else if (selector == "decode") {
        write_reg_stmt(dst, "native_codec_decode(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ", " +
                                (kw_count == 1U ? read_reg_expr(kw_value_reg)
                                                : "NativeValue::nullv()") +
                                ", " + (kw_count == 1U ? "true" : "false") +
                                ")");
      } else if (selector == "hmac_sha256") {
        write_reg_stmt(dst, "native_digest_hmac_sha256(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ", " +
                                read_reg_expr(arg2) + ")");
      } else if (selector == "crc32" || selector == "md5" ||
                 selector == "sha1" || selector == "sha256" ||
                 selector == "streebog256" || selector == "streebog512" ||
                 selector == "gost256" || selector == "gost512" ||
                 selector == "гост256" || selector == "гост512" ||
                 selector == "стрибог256" || selector == "стрибог512") {
        write_reg_stmt(dst, "native_digest_one(" + read_reg_expr(recv) + ", " +
                                read_reg_expr(arg) +
                                ", native_hex_to_string(\"" +
                                string_to_hex_text(selector) + "\"))");
      } else if (selector == "hex") {
        if (pos_count == 0U) {
          write_reg_stmt(dst, "native_bytes_hex(" + read_reg_expr(recv) + ")");
        } else {
          write_reg_stmt(dst, "native_secure_random_hex(" +
                                  read_reg_expr(recv) + ", " +
                                  read_reg_expr(arg) + ")");
        }
      } else if (selector == "int") {
        write_reg_stmt(dst, "native_secure_random_int(" + read_reg_expr(recv) +
                                ", " + read_reg_expr(arg) + ")");
      } else if (native_cpp_time_nullary_selector(selector)) {
        const std::string selector_enum =
            native_cpp_time_selector_enum(selector);
        write_reg_stmt(dst, "native_time_nullary(" + read_reg_expr(recv) +
                                ", " + selector_enum + ")");
      }
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::IAdd:
    case Opcode::ISub:
    case Opcode::ILt:
    case Opcode::IGt:
    case Opcode::IMul:
    case Opcode::IDiv:
    case Opcode::IMod:
    case Opcode::IFloorDiv:
    case Opcode::ILe:
    case Opcode::IGe:
    case Opcode::IEq:
    case Opcode::INe:
    case Opcode::ICmp:
    case Opcode::IBitAnd:
    case Opcode::IBitOr:
    case Opcode::IBitXor:
    case Opcode::IShl:
    case Opcode::IShr: {
      std::uint32_t dst = 0;
      std::uint32_t lhs = 0;
      std::uint32_t rhs = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &lhs);
      operand_u32_value(instruction, 2, &rhs);
      const NativeScalarKind lhs_kind = scalar_kind(lhs, pc_scalar_state);
      const NativeScalarKind rhs_kind = scalar_kind(rhs, pc_scalar_state);
      const bool proven_numeric_operands =
          native_cpp_scalar_numeric(lhs_kind) &&
          native_cpp_scalar_numeric(rhs_kind);
      const bool proven_int_operands = lhs_kind == NativeScalarKind::Integer &&
                                       rhs_kind == NativeScalarKind::Integer;
      const std::string lhs_value = read_reg_expr(lhs);
      const std::string rhs_value = read_reg_expr(rhs);
      const std::string lhs_int = int_reg_expr(lhs, pc_scalar_state);
      const std::string rhs_int = int_reg_expr(rhs, pc_scalar_state);
      const std::string lhs_double = double_reg_expr(lhs, pc_scalar_state);
      const std::string rhs_double = double_reg_expr(rhs, pc_scalar_state);
      const auto write_fallback = [&]() {
        if (instruction.opcode == Opcode::IAdd) {
          write_reg_stmt(dst, "native_numeric_fast_add(" + lhs_value + ", " +
                                  rhs_value + ")");
        } else if (instruction.opcode == Opcode::ISub) {
          write_reg_stmt(dst, "native_numeric_fast_sub(" + lhs_value + ", " +
                                  rhs_value + ")");
        } else if (instruction.opcode == Opcode::IMul) {
          write_reg_stmt(dst, "native_numeric_fast_mul(" + lhs_value + ", " +
                                  rhs_value + ")");
        } else if (instruction.opcode == Opcode::IDiv) {
          write_reg_stmt(dst, "native_numeric_fast_div(" + lhs_value + ", " +
                                  rhs_value + ")");
        } else if (instruction.opcode == Opcode::IMod) {
          write_reg_stmt(dst, "native_numeric_fast_mod(" + lhs_value + ", " +
                                  rhs_value + ")");
        } else if (instruction.opcode == Opcode::IFloorDiv) {
          write_reg_stmt(dst, "native_numeric_fast_floor_div(" + lhs_value +
                                  ", " + rhs_value + ")");
        } else if (instruction.opcode == Opcode::ILe) {
          write_reg_stmt(dst, "native_numeric_fast_le(" + lhs_value + ", " +
                                  rhs_value + ")");
        } else if (instruction.opcode == Opcode::IGe) {
          write_reg_stmt(dst, "native_numeric_fast_ge(" + lhs_value + ", " +
                                  rhs_value + ")");
        } else if (instruction.opcode == Opcode::IEq) {
          write_reg_stmt(dst, "native_numeric_fast_eq(" + lhs_value + ", " +
                                  rhs_value + ", false)");
        } else if (instruction.opcode == Opcode::INe) {
          write_reg_stmt(dst, "native_numeric_fast_eq(" + lhs_value + ", " +
                                  rhs_value + ", true)");
        } else if (instruction.opcode == Opcode::ICmp) {
          write_reg_stmt(dst, "native_numeric_fast_cmp(" + lhs_value + ", " +
                                  rhs_value + ")");
        } else if (instruction.opcode == Opcode::ILt) {
          write_reg_stmt(dst, "native_numeric_fast_lt(" + lhs_value + ", " +
                                  rhs_value + ")");
        } else if (instruction.opcode == Opcode::IGt) {
          write_reg_stmt(dst, "native_numeric_fast_gt(" + lhs_value + ", " +
                                  rhs_value + ")");
        } else if (instruction.opcode == Opcode::IBitAnd) {
          write_reg_stmt(dst, "NativeValue::integer(bit_and_int64(as_int(" +
                                  lhs_value + "), as_int(" + rhs_value + ")))");
        } else if (instruction.opcode == Opcode::IBitOr) {
          write_reg_stmt(dst, "NativeValue::integer(bit_or_int64(as_int(" +
                                  lhs_value + "), as_int(" + rhs_value + ")))");
        } else if (instruction.opcode == Opcode::IBitXor) {
          write_reg_stmt(dst, "NativeValue::integer(bit_xor_int64(as_int(" +
                                  lhs_value + "), as_int(" + rhs_value + ")))");
        } else if (instruction.opcode == Opcode::IShl) {
          out << "  if (as_int(" << rhs_value << ") < 0 || as_int(" << rhs_value
              << ") >= 64) throw NativeBailout();\n";
          write_reg_stmt(dst, "NativeValue::integer(profile_shl_int64(as_int(" +
                                  lhs_value + "), as_int(" + rhs_value + ")))");
        } else {
          out << "  if (as_int(" << rhs_value << ") < 0 || as_int(" << rhs_value
              << ") >= 64) throw NativeBailout();\n";
          write_reg_stmt(dst, "NativeValue::integer(shr_int64(as_int(" +
                                  lhs_value + "), as_int(" + rhs_value + ")))");
        }
      };

      if (!proven_numeric_operands) {
        write_fallback();
      } else if (native_cpp_opcode_bool_compare(instruction.opcode)) {
        const std::string lhs_cmp = proven_int_operands ? lhs_int : lhs_double;
        const std::string rhs_cmp = proven_int_operands ? rhs_int : rhs_double;
        if (instruction.opcode == Opcode::ILe) {
          write_bool_reg_stmt(dst, lhs_cmp + " <= " + rhs_cmp);
        } else if (instruction.opcode == Opcode::IGe) {
          write_bool_reg_stmt(dst, lhs_cmp + " >= " + rhs_cmp);
        } else if (instruction.opcode == Opcode::IEq) {
          write_bool_reg_stmt(dst, lhs_cmp + " == " + rhs_cmp);
        } else if (instruction.opcode == Opcode::INe) {
          write_bool_reg_stmt(dst, lhs_cmp + " != " + rhs_cmp);
        } else if (instruction.opcode == Opcode::ILt) {
          write_bool_reg_stmt(dst, lhs_cmp + " < " + rhs_cmp);
        } else {
          write_bool_reg_stmt(dst, lhs_cmp + " > " + rhs_cmp);
        }
      } else if (instruction.opcode == Opcode::ICmp) {
        if (proven_int_operands) {
          write_int_reg_stmt(dst,
                             "compare_int64(" + lhs_int + ", " + rhs_int + ")");
        } else {
          write_int_reg_stmt(dst, "compare_double_native(" + lhs_double + ", " +
                                      rhs_double + ")");
        }
      } else if (!proven_int_operands &&
                 native_cpp_opcode_numeric_arithmetic(instruction.opcode)) {
        if (instruction.opcode == Opcode::IAdd) {
          write_float_reg_stmt(dst, lhs_double + " + " + rhs_double);
        } else if (instruction.opcode == Opcode::ISub) {
          write_float_reg_stmt(dst, lhs_double + " - " + rhs_double);
        } else if (instruction.opcode == Opcode::IMul) {
          write_float_reg_stmt(dst, lhs_double + " * " + rhs_double);
        } else if (instruction.opcode == Opcode::IDiv) {
          out << "  if (" << rhs_double << " == 0.0) throw NativeBailout();\n";
          write_float_reg_stmt(dst, lhs_double + " / " + rhs_double);
        } else if (instruction.opcode == Opcode::IMod) {
          out << "  if (" << rhs_double << " == 0.0) throw NativeBailout();\n";
          write_float_reg_stmt(dst, "floor_mod_double_native(" + lhs_double +
                                        ", " + rhs_double + ")");
        } else {
          out << "  if (" << rhs_double << " == 0.0) throw NativeBailout();\n";
          write_float_reg_stmt(dst, "std::floor(" + lhs_double + " / " +
                                        rhs_double + ")");
        }
      } else if (instruction.opcode == Opcode::IAdd) {
        write_int_reg_stmt(dst, "profile_add_int64(" + lhs_int + ", " +
                                    rhs_int + ")");
      } else if (instruction.opcode == Opcode::ISub) {
        write_int_reg_stmt(dst, "profile_sub_int64(" + lhs_int + ", " +
                                    rhs_int + ")");
      } else if (instruction.opcode == Opcode::IMul) {
        write_int_reg_stmt(dst, "profile_mul_int64(" + lhs_int + ", " +
                                    rhs_int + ")");
      } else if (instruction.opcode == Opcode::IDiv) {
        write_int_reg_stmt(dst, "profile_div_int64(" + lhs_int + ", " +
                                    rhs_int + ")");
      } else if (instruction.opcode == Opcode::IMod) {
        out << "  if (" << rhs_int << " == 0) throw NativeBailout();\n";
        write_int_reg_stmt(dst,
                           "floor_mod_int64(" + lhs_int + ", " + rhs_int + ")");
      } else if (instruction.opcode == Opcode::IFloorDiv) {
        write_int_reg_stmt(dst, "profile_floor_div_int64(" + lhs_int + ", " +
                                    rhs_int + ")");
      } else if (instruction.opcode == Opcode::IBitAnd) {
        write_int_reg_stmt(dst,
                           "bit_and_int64(" + lhs_int + ", " + rhs_int + ")");
      } else if (instruction.opcode == Opcode::IBitOr) {
        write_int_reg_stmt(dst,
                           "bit_or_int64(" + lhs_int + ", " + rhs_int + ")");
      } else if (instruction.opcode == Opcode::IBitXor) {
        write_int_reg_stmt(dst,
                           "bit_xor_int64(" + lhs_int + ", " + rhs_int + ")");
      } else if (instruction.opcode == Opcode::IShl) {
        out << "  if (" << rhs_int << " < 0 || " << rhs_int
            << " >= 64) throw NativeBailout();\n";
        write_int_reg_stmt(dst, "profile_shl_int64(" + lhs_int + ", " +
                                    rhs_int + ")");
      } else if (instruction.opcode == Opcode::IShr) {
        out << "  if (" << rhs_int << " < 0 || " << rhs_int
            << " >= 64) throw NativeBailout();\n";
        write_int_reg_stmt(dst, "shr_int64(" + lhs_int + ", " + rhs_int + ")");
      } else {
        write_fallback();
      }
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::IAddK:
    case Opcode::ISubK:
    case Opcode::ILtK:
    case Opcode::IGtK:
    case Opcode::IMulK:
    case Opcode::IDivK:
    case Opcode::IModK:
    case Opcode::IFloorDivK:
    case Opcode::ILeK:
    case Opcode::IGeK:
    case Opcode::IEqK:
    case Opcode::INeK:
    case Opcode::ICmpK:
    case Opcode::IBitAndK:
    case Opcode::IBitOrK:
    case Opcode::IBitXorK:
    case Opcode::IShlK:
    case Opcode::IShrK: {
      std::uint32_t dst = 0;
      std::uint32_t lhs = 0;
      std::uint32_t const_id = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &lhs);
      operand_u32_value(instruction, 2, &const_id);
      const std::int64_t rhs = module.const_pool[const_id].int_value;
      const NativeScalarKind lhs_kind = scalar_kind(lhs, pc_scalar_state);
      const bool proven_numeric_lhs = native_cpp_scalar_numeric(lhs_kind);
      const bool proven_int_lhs = lhs_kind == NativeScalarKind::Integer;
      const std::string lhs_value = read_reg_expr(lhs);
      const std::string lhs_int = int_reg_expr(lhs, pc_scalar_state);
      const std::string lhs_double = double_reg_expr(lhs, pc_scalar_state);
      const std::string rhs_int = cpp_decimal_i64(rhs);
      const std::string rhs_double = "static_cast<double>(" + rhs_int + ")";
      const auto write_fallback = [&]() {
        if (instruction.opcode == Opcode::IAddK) {
          write_reg_stmt(dst, "native_numeric_fast_add_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ")");
        } else if (instruction.opcode == Opcode::ISubK) {
          write_reg_stmt(dst, "native_numeric_fast_sub_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ")");
        } else if (instruction.opcode == Opcode::IMulK) {
          write_reg_stmt(dst, "native_numeric_fast_mul_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ")");
        } else if (instruction.opcode == Opcode::IDivK) {
          write_reg_stmt(dst, "native_numeric_fast_div_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ")");
        } else if (instruction.opcode == Opcode::IModK) {
          write_reg_stmt(dst, "native_numeric_fast_mod_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ")");
        } else if (instruction.opcode == Opcode::IFloorDivK) {
          write_reg_stmt(dst, "native_numeric_fast_floor_div_int_rhs(" +
                                  lhs_value + ", " + rhs_int + ")");
        } else if (instruction.opcode == Opcode::ILeK) {
          write_reg_stmt(dst, "native_numeric_fast_le_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ")");
        } else if (instruction.opcode == Opcode::IGeK) {
          write_reg_stmt(dst, "native_numeric_fast_ge_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ")");
        } else if (instruction.opcode == Opcode::IEqK) {
          write_reg_stmt(dst, "native_numeric_fast_eq_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ", false)");
        } else if (instruction.opcode == Opcode::INeK) {
          write_reg_stmt(dst, "native_numeric_fast_eq_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ", true)");
        } else if (instruction.opcode == Opcode::ICmpK) {
          write_reg_stmt(dst, "native_numeric_fast_cmp_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ")");
        } else if (instruction.opcode == Opcode::ILtK) {
          write_reg_stmt(dst, "native_numeric_fast_lt_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ")");
        } else if (instruction.opcode == Opcode::IGtK) {
          write_reg_stmt(dst, "native_numeric_fast_gt_int_rhs(" + lhs_value +
                                  ", " + rhs_int + ")");
        } else if (instruction.opcode == Opcode::IBitAndK) {
          write_reg_stmt(dst, "NativeValue::integer(bit_and_int64(as_int(" +
                                  lhs_value + "), " + rhs_int + "))");
        } else if (instruction.opcode == Opcode::IBitOrK) {
          write_reg_stmt(dst, "NativeValue::integer(bit_or_int64(as_int(" +
                                  lhs_value + "), " + rhs_int + "))");
        } else if (instruction.opcode == Opcode::IBitXorK) {
          write_reg_stmt(dst, "NativeValue::integer(bit_xor_int64(as_int(" +
                                  lhs_value + "), " + rhs_int + "))");
        } else if (instruction.opcode == Opcode::IShlK) {
          if (rhs < 0 || rhs >= 64) {
            out << "  throw NativeBailout();\n";
          } else {
            write_reg_stmt(dst,
                           "NativeValue::integer(profile_shl_int64(as_int(" +
                               lhs_value + "), " + rhs_int + "))");
          }
        } else if (rhs < 0 || rhs >= 64) {
          out << "  throw NativeBailout();\n";
        } else {
          write_reg_stmt(dst, "NativeValue::integer(shr_int64(as_int(" +
                                  lhs_value + "), " + rhs_int + "))");
        }
      };

      if (!proven_numeric_lhs) {
        write_fallback();
      } else if (native_cpp_opcode_bool_compare(instruction.opcode)) {
        const std::string lhs_cmp = proven_int_lhs ? lhs_int : lhs_double;
        const std::string rhs_cmp = proven_int_lhs ? rhs_int : rhs_double;
        if (instruction.opcode == Opcode::ILeK) {
          write_bool_reg_stmt(dst, lhs_cmp + " <= " + rhs_cmp);
        } else if (instruction.opcode == Opcode::IGeK) {
          write_bool_reg_stmt(dst, lhs_cmp + " >= " + rhs_cmp);
        } else if (instruction.opcode == Opcode::IEqK) {
          write_bool_reg_stmt(dst, lhs_cmp + " == " + rhs_cmp);
        } else if (instruction.opcode == Opcode::INeK) {
          write_bool_reg_stmt(dst, lhs_cmp + " != " + rhs_cmp);
        } else if (instruction.opcode == Opcode::ILtK) {
          write_bool_reg_stmt(dst, lhs_cmp + " < " + rhs_cmp);
        } else {
          write_bool_reg_stmt(dst, lhs_cmp + " > " + rhs_cmp);
        }
      } else if (instruction.opcode == Opcode::ICmpK) {
        if (proven_int_lhs) {
          write_int_reg_stmt(dst,
                             "compare_int64(" + lhs_int + ", " + rhs_int + ")");
        } else {
          write_int_reg_stmt(dst, "compare_double_native(" + lhs_double + ", " +
                                      rhs_double + ")");
        }
      } else if (!proven_int_lhs &&
                 native_cpp_opcode_numeric_arithmetic(instruction.opcode)) {
        if (instruction.opcode == Opcode::IAddK) {
          write_float_reg_stmt(dst, lhs_double + " + " + rhs_double);
        } else if (instruction.opcode == Opcode::ISubK) {
          write_float_reg_stmt(dst, lhs_double + " - " + rhs_double);
        } else if (instruction.opcode == Opcode::IMulK) {
          write_float_reg_stmt(dst, lhs_double + " * " + rhs_double);
        } else if (instruction.opcode == Opcode::IDivK) {
          if (rhs == 0) {
            out << "  throw NativeBailout();\n";
          } else {
            write_float_reg_stmt(dst, lhs_double + " / " + rhs_double);
          }
        } else if (instruction.opcode == Opcode::IModK) {
          if (rhs == 0) {
            out << "  throw NativeBailout();\n";
          } else {
            write_float_reg_stmt(dst, "floor_mod_double_native(" + lhs_double +
                                          ", " + rhs_double + ")");
          }
        } else if (rhs == 0) {
          out << "  throw NativeBailout();\n";
        } else {
          write_float_reg_stmt(dst, "std::floor(" + lhs_double + " / " +
                                        rhs_double + ")");
        }
      } else if (instruction.opcode == Opcode::IAddK) {
        write_int_reg_stmt(dst, "profile_add_int64(" + lhs_int + ", " +
                                    rhs_int + ")");
      } else if (instruction.opcode == Opcode::ISubK) {
        write_int_reg_stmt(dst, "profile_sub_int64(" + lhs_int + ", " +
                                    rhs_int + ")");
      } else if (instruction.opcode == Opcode::IMulK) {
        write_int_reg_stmt(dst, "profile_mul_int64(" + lhs_int + ", " +
                                    rhs_int + ")");
      } else if (instruction.opcode == Opcode::IDivK) {
        write_int_reg_stmt(dst, "profile_div_int64(" + lhs_int + ", " +
                                    rhs_int + ")");
      } else if (instruction.opcode == Opcode::IModK) {
        if (rhs == 0) {
          out << "  throw NativeBailout();\n";
        } else {
          write_int_reg_stmt(dst, "floor_mod_int64(" + lhs_int + ", " +
                                      rhs_int + ")");
        }
      } else if (instruction.opcode == Opcode::IFloorDivK) {
        write_int_reg_stmt(dst, "profile_floor_div_int64(" + lhs_int + ", " +
                                    rhs_int + ")");
      } else if (instruction.opcode == Opcode::IBitAndK) {
        write_int_reg_stmt(dst,
                           "bit_and_int64(" + lhs_int + ", " + rhs_int + ")");
      } else if (instruction.opcode == Opcode::IBitOrK) {
        write_int_reg_stmt(dst,
                           "bit_or_int64(" + lhs_int + ", " + rhs_int + ")");
      } else if (instruction.opcode == Opcode::IBitXorK) {
        write_int_reg_stmt(dst,
                           "bit_xor_int64(" + lhs_int + ", " + rhs_int + ")");
      } else if (instruction.opcode == Opcode::IShlK) {
        if (rhs < 0 || rhs >= 64) {
          out << "  throw NativeBailout();\n";
        } else {
          write_int_reg_stmt(dst, "profile_shl_int64(" + lhs_int + ", " +
                                      rhs_int + ")");
        }
      } else if (instruction.opcode == Opcode::IShrK) {
        if (rhs < 0 || rhs >= 64) {
          out << "  throw NativeBailout();\n";
        } else {
          write_int_reg_stmt(dst,
                             "shr_int64(" + lhs_int + ", " + rhs_int + ")");
        }
      } else {
        write_fallback();
      }
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::Jump: {
      std::uint32_t target = 0;
      operand_u32_value(instruction, 0, &target);
      emit_goto_pc(target, next_scalar_state);
      break;
    }
    case Opcode::JumpIfTrue:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull: {
      std::uint32_t cond = 0;
      std::uint32_t target = 0;
      operand_u32_value(instruction, 0, &cond);
      operand_u32_value(instruction, 1, &target);
      if (instruction.opcode == Opcode::JumpIfTrue) {
        out << "  if (" << truthy_reg_expr(cond, pc_scalar_state) << ") {\n";
        emit_goto_pc(target, next_scalar_state);
        out << "  }\n";
      } else if (instruction.opcode == Opcode::JumpIfFalse) {
        out << "  if (!" << truthy_reg_expr(cond, pc_scalar_state) << ") {\n";
        emit_goto_pc(target, next_scalar_state);
        out << "  }\n";
      } else {
        out << "  if (" << read_reg_expr(cond)
            << ".tag == NativeValue::Tag::Null) {\n";
        emit_goto_pc(target, next_scalar_state);
        out << "  }\n";
      }
      emit_next(pc, next_scalar_state);
      break;
    }
    case Opcode::CloseUpvalues:
    case Opcode::Safepoint:
    case Opcode::TypeCheck:
    case Opcode::PBind:
    case Opcode::PCommit:
      emit_next(pc, next_scalar_state);
      break;
    case Opcode::Return: {
      std::uint32_t src = 0;
      operand_u32_value(instruction, 0, &src);
      out << "  return " << read_reg_expr(src) << ";\n";
      break;
    }
    default:
      out << "  throw NativeBailout();\n";
      break;
    }
  }
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  return out.str();
}

std::string emit_embedded_hex_cpp(const std::vector<std::uint8_t> &bytes) {
  const std::string hex = bytes_to_hex_text(bytes);
  std::ostringstream out;
  out << "static const char *kEmbeddedBytecodeHex =\n";
  for (std::size_t i = 0; i < hex.size(); i += 96U) {
    out << "  \"" << hex.substr(i, 96U) << "\"\n";
  }
  out << ";\n\n";
  return out.str();
}

std::string emit_embedded_capability_grants_cpp(
    const std::vector<amber::capability::CapabilityRequest> &grants) {
  std::ostringstream out;
  out << "static std::vector<amber::runtime::RuntimeCapabilityGrant> "
         "embedded_capability_grants() {\n";
  out << "  std::vector<amber::runtime::RuntimeCapabilityGrant> grants;\n";
  for (const amber::capability::CapabilityRequest &grant : grants) {
    out << "  grants.push_back(amber::capability::make_capability("
        << "native_hex_to_string(\"" << string_to_hex_text(grant.name)
        << "\"), native_hex_to_string(\"" << string_to_hex_text(grant.target)
        << "\"), native_hex_to_string(\"" << string_to_hex_text(grant.reason)
        << "\"), " << grant.flags << "U));\n";
  }
  out << "  return grants;\n";
  out << "}\n\n";
  return out.str();
}

// The native string table is seeded with the module's interned strings so
// LoadK string constants keep their bytecode ids. Hex encoding sidesteps
// C++ literal escaping for arbitrary byte content.
std::string emit_module_strings_cpp(const amber::bytecode::BcModule &module) {
  std::ostringstream out;
  out << "static const char *kModuleStringHex[] = {\n";
  for (const std::string &text : module.strings) {
    out << "  \"" << string_to_hex_text(text) << "\",\n";
  }
  if (module.strings.empty()) {
    out << "  \"\",\n";
  }
  out << "};\n";
  out << "static const std::size_t kModuleStringCount = "
      << module.strings.size() << "U;\n\n";
  out << "static std::string native_hex_to_string(const char *hex) {\n";
  out << "  std::string text;\n";
  out << "  auto digit = [](char c) -> int {\n";
  out << "    if (c >= '0' && c <= '9') return c - '0';\n";
  out << "    if (c >= 'a' && c <= 'f') return 10 + c - 'a';\n";
  out << "    if (c >= 'A' && c <= 'F') return 10 + c - 'A';\n";
  out << "    return -1;\n";
  out << "  };\n";
  out << "  for (std::size_t i = 0; hex[i] != '\\0' && hex[i + 1U] != '\\0'; "
         "i += 2U) {\n";
  out << "    text.push_back(static_cast<char>((digit(hex[i]) << 4) | "
         "digit(hex[i + 1U])));\n";
  out << "  }\n";
  out << "  return text;\n";
  out << "}\n\n";
  out << "static std::vector<std::string> &native_strings() {\n";
  out << "  static std::vector<std::string> *table = [] {\n";
  out << "    auto *out_table = new std::vector<std::string>();\n";
  out << "    out_table->reserve(kModuleStringCount);\n";
  out << "    for (std::size_t i = 0; i < kModuleStringCount; ++i) {\n";
  out << "      out_table->push_back(native_hex_to_string("
         "kModuleStringHex[i]));\n";
  out << "    }\n";
  out << "    return out_table;\n";
  out << "  }();\n";
  out << "  return *table;\n";
  out << "}\n\n";
  out << "static std::unordered_map<std::size_t, std::vector<std::int64_t>> &"
         "native_string_index() {\n";
  out << "  static std::unordered_map<std::size_t, "
         "std::vector<std::int64_t>> *index = [] {\n";
  out << "    auto *out_index = new std::unordered_map<std::size_t, "
         "std::vector<std::int64_t>>();\n";
  out << "    const std::vector<std::string> &table = native_strings();\n";
  out << "    for (std::size_t i = 0; i < table.size(); ++i) {\n";
  out << "      std::vector<std::int64_t> &bucket = "
         "(*out_index)[std::hash<std::string>{}(table[i])];\n";
  out << "      bool duplicate = false;\n";
  out << "      for (const std::int64_t existing_id : bucket) {\n";
  out << "        if (existing_id >= 0 && "
         "static_cast<std::size_t>(existing_id) < table.size() && "
         "table[static_cast<std::size_t>(existing_id)] == table[i]) {\n";
  out << "          duplicate = true;\n";
  out << "          break;\n";
  out << "        }\n";
  out << "      }\n";
  out << "      if (!duplicate) "
         "bucket.push_back(static_cast<std::int64_t>(i));\n";
  out << "    }\n";
  out << "    return out_index;\n";
  out << "  }();\n";
  out << "  return *index;\n";
  out << "}\n\n";
  out << "static std::int64_t native_intern_string(const std::string &text) "
         "{\n";
  out << "  auto &index = native_string_index();\n";
  out << "  const std::size_t hash = std::hash<std::string>{}(text);\n";
  out << "  std::vector<std::string> &table = native_strings();\n";
  out << "  const auto found = index.find(hash);\n";
  out << "  if (found != index.end()) {\n";
  out << "    for (const std::int64_t id : found->second) {\n";
  out << "      if (id >= 0 && static_cast<std::size_t>(id) < table.size() && "
         "table[static_cast<std::size_t>(id)] == text) return id;\n";
  out << "    }\n";
  out << "  }\n";
  out << "  const std::int64_t id = "
         "static_cast<std::int64_t>(table.size());\n";
  out << "  table.push_back(text);\n";
  out << "  index[hash].push_back(id);\n";
  out << "  return id;\n";
  out << "}\n\n";
  // No-insert probe used by map read paths so absent lookups neither grow
  // the table nor pay the insert.
  out << "static std::optional<std::int64_t> native_intern_string_lookup("
         "const std::string &text) {\n";
  out << "  auto &index = native_string_index();\n";
  out << "  const std::size_t hash = std::hash<std::string>{}(text);\n";
  out << "  std::vector<std::string> &table = native_strings();\n";
  out << "  const auto found = index.find(hash);\n";
  out << "  if (found != index.end()) {\n";
  out << "    for (const std::int64_t id : found->second) {\n";
  out << "      if (id >= 0 && static_cast<std::size_t>(id) < table.size() && "
         "table[static_cast<std::size_t>(id)] == text) return id;\n";
  out << "    }\n";
  out << "  }\n";
  out << "  return std::nullopt;\n";
  out << "}\n\n";
  out << "static const char *kModuleSymbolHex[] = {\n";
  for (const std::string &text : module.symbols) {
    out << "  \"" << string_to_hex_text(text) << "\",\n";
  }
  if (module.symbols.empty()) {
    out << "  \"\",\n";
  }
  out << "};\n";
  out << "static const std::size_t kModuleSymbolCount = "
      << module.symbols.size() << "U;\n\n";
  out << "static std::vector<std::string> &native_symbols() {\n";
  out << "  static std::vector<std::string> *table = [] {\n";
  out << "    auto *out_table = new std::vector<std::string>();\n";
  out << "    out_table->reserve(kModuleSymbolCount);\n";
  out << "    for (std::size_t i = 0; i < kModuleSymbolCount; ++i) {\n";
  out << "      out_table->push_back(native_hex_to_string("
         "kModuleSymbolHex[i]));\n";
  out << "    }\n";
  out << "    return out_table;\n";
  out << "  }();\n";
  out << "  return *table;\n";
  out << "}\n\n";
  out << "static std::unordered_map<std::string, std::int64_t> &"
         "native_symbol_index() {\n";
  out << "  static std::unordered_map<std::string, std::int64_t> *index = "
         "[] {\n";
  out << "    auto *out_index = new std::unordered_map<std::string, "
         "std::int64_t>();\n";
  out << "    const std::vector<std::string> &table = native_symbols();\n";
  out << "    for (std::size_t i = 0; i < table.size(); ++i) {\n";
  out << "      out_index->emplace(table[i], "
         "static_cast<std::int64_t>(i));\n";
  out << "    }\n";
  out << "    return out_index;\n";
  out << "  }();\n";
  out << "  return *index;\n";
  out << "}\n\n";
  out << "static std::int64_t native_intern_symbol(const std::string &text) "
         "{\n";
  out << "  auto &index = native_symbol_index();\n";
  out << "  const auto found = index.find(text);\n";
  out << "  if (found != index.end()) return found->second;\n";
  out << "  std::vector<std::string> &table = native_symbols();\n";
  out << "  const std::int64_t id = "
         "static_cast<std::int64_t>(table.size());\n";
  out << "  table.push_back(text);\n";
  out << "  index.emplace(text, id);\n";
  out << "  return id;\n";
  out << "}\n\n";
  out << "static const std::string &native_symbol_text(std::int64_t id) {\n";
  out << "  const auto &symbols = native_symbols();\n";
  out << "  if (id < 0 || static_cast<std::size_t>(id) >= symbols.size()) "
         "throw std::out_of_range(\"native symbol id\");\n";
  out << "  return symbols[static_cast<std::size_t>(id)];\n";
  out << "}\n\n";
  return out.str();
}

bool is_c_symbol_name(const std::string &name) {
  if (name.empty()) {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(name.front());
  if (!(std::isalpha(first) || first == '_')) {
    return false;
  }
  for (const char c : name) {
    const unsigned char ch = static_cast<unsigned char>(c);
    if (!(std::isalnum(ch) || ch == '_')) {
      return false;
    }
  }
  return true;
}

std::vector<std::string>
native_binding_logicals(const amber::bytecode::BcModule &module) {
  const std::string bind_prefix = "amber.native.bind:";
  const std::string method_prefix = "amber.native.method:";
  std::vector<std::string> out;
  std::set<std::string> seen;
  const auto string_or_empty = [&](std::uint32_t id) {
    return id < module.strings.size() ? module.strings[id] : std::string();
  };
  const auto add = [&](const std::string &logical) {
    if (!logical.empty() && seen.insert(logical).second) {
      out.push_back(logical);
    }
  };
  for (const amber::bytecode::AttrEntry &attr : module.attrs) {
    const std::string key = string_or_empty(attr.key_str_id);
    const std::string value = string_or_empty(attr.value_str_id);
    if (key.compare(0, bind_prefix.size(), bind_prefix) == 0) {
      if (value.size() >= 2U && value[1] == ':') {
        add(value.substr(2));
      }
    } else if (key.compare(0, method_prefix.size(), method_prefix) == 0) {
      add(value);
    }
  }
  return out;
}

std::vector<std::string> split_native_attr_fields(const std::string &text) {
  std::vector<std::string> fields;
  std::string field;
  for (const char c : text) {
    if (c == '\t') {
      fields.push_back(field);
      field.clear();
    } else {
      field.push_back(c);
    }
  }
  fields.push_back(field);
  return fields;
}

std::vector<amber::pkg::PackageNativeType>
source_declared_native_types(const amber::bytecode::BcModule &module) {
  static const std::string kTypePrefix = "amber.native.type:";
  std::vector<amber::pkg::PackageNativeType> out;
  const auto string_or_empty = [&](std::uint32_t id) {
    return id < module.strings.size() ? module.strings[id] : std::string();
  };
  for (const amber::bytecode::AttrEntry &attr : module.attrs) {
    const std::string key = string_or_empty(attr.key_str_id);
    if (key.compare(0, kTypePrefix.size(), kTypePrefix) != 0) {
      continue;
    }
    const std::vector<std::string> fields =
        split_native_attr_fields(string_or_empty(attr.value_str_id));
    if (fields.size() < 2U || fields[0].empty() || fields[1].empty()) {
      continue;
    }
    amber::pkg::PackageNativeType type;
    type.amber = key.substr(kTypePrefix.size());
    type.tag = fields[0];
    type.ownership = fields[1];
    if (fields.size() >= 3U) {
      type.destructor = fields[2];
    }
    out.push_back(std::move(type));
  }
  return out;
}

std::vector<amber::pkg::PackageNativeError>
source_declared_native_errors(const amber::bytecode::BcModule &module) {
  static const std::string kErrorPrefix = "amber.native.error:";
  std::vector<amber::pkg::PackageNativeError> out;
  const auto string_or_empty = [&](std::uint32_t id) {
    return id < module.strings.size() ? module.strings[id] : std::string();
  };
  for (const amber::bytecode::AttrEntry &attr : module.attrs) {
    const std::string key = string_or_empty(attr.key_str_id);
    if (key.compare(0, kErrorPrefix.size(), kErrorPrefix) != 0) {
      continue;
    }
    amber::pkg::PackageNativeError error;
    error.name = key.substr(kErrorPrefix.size());
    error.parent = string_or_empty(attr.value_str_id);
    if (error.parent.empty()) {
      error.parent = "NativeError";
    }
    out.push_back(std::move(error));
  }
  return out;
}

std::vector<amber::pkg::PackageNativeExtension>
augment_native_extensions_from_source(
    std::vector<amber::pkg::PackageNativeExtension> extensions,
    const amber::bytecode::BcModule &module) {
  std::vector<amber::pkg::PackageNativeType> types =
      source_declared_native_types(module);
  std::vector<amber::pkg::PackageNativeError> errors =
      source_declared_native_errors(module);
  if (types.empty() && errors.empty()) {
    return extensions;
  }
  if (extensions.empty()) {
    amber::pkg::PackageNativeExtension extension;
    extension.name = "source";
    extensions.push_back(std::move(extension));
  }
  amber::pkg::PackageNativeExtension &target = extensions.front();
  for (amber::pkg::PackageNativeType &type : types) {
    const bool exists = std::any_of(
        target.types.begin(), target.types.end(),
        [&](const amber::pkg::PackageNativeType &registered) {
          return registered.amber == type.amber || registered.tag == type.tag;
        });
    if (!exists) {
      target.types.push_back(std::move(type));
    }
  }
  for (amber::pkg::PackageNativeError &error : errors) {
    const bool exists =
        std::any_of(target.errors.begin(), target.errors.end(),
                    [&](const amber::pkg::PackageNativeError &registered) {
                      return registered.name == error.name;
                    });
    if (!exists) {
      target.errors.push_back(std::move(error));
    }
  }
  return extensions;
}

std::set<std::string> manifest_native_logicals(
    const std::vector<amber::pkg::PackageNativeExtension> &native_extensions) {
  std::set<std::string> out;
  for (const amber::pkg::PackageNativeExtension &extension :
       native_extensions) {
    for (const amber::pkg::PackageNativeSymbol &symbol : extension.symbols) {
      out.insert(symbol.logical);
    }
  }
  return out;
}

std::vector<std::string> direct_native_symbols(
    const amber::bytecode::BcModule &module,
    const std::vector<amber::pkg::PackageNativeExtension> &native_extensions) {
  const std::set<std::string> manifest_logicals =
      manifest_native_logicals(native_extensions);
  std::vector<std::string> out;
  for (const std::string &logical : native_binding_logicals(module)) {
    if (manifest_logicals.find(logical) == manifest_logicals.end() &&
        is_c_symbol_name(logical)) {
      out.push_back(logical);
    }
  }
  return out;
}

NativeCppBuildPlan
build_native_cpp_plan(const RunnableModuleArtifact &artifact,
                      const amber::bytecode::BcModule &module,
                      const std::vector<amber::pkg::PackageNativeExtension>
                          &native_extensions = {}) {
  NativeCppBuildPlan plan;
  std::string first_reason;
  const NativeCppNumericProfile numeric_profile =
      native_cpp_numeric_profile(module);
  plan.numeric_int_type = numeric_profile.int_type;
  plan.numeric_overflow = numeric_profile.overflow;
  plan.numeric_policy = numeric_profile.policy;
  if (!numeric_profile.supported) {
    first_reason = numeric_profile.reason;
  }
  // A `*name` / `**name` rest parameter packs surplus positional arguments into
  // an immutable Tuple and unmatched keywords into a frozen Map in the VM frame
  // prologue; the direct native C++ calling convention has no equivalent
  // packing. Rather than dropping the whole module to the VM, keep only the
  // rest/keyword-rest method BODIES on the per-function VM bridge -- a native
  // caller invokes them through amber_vm_fallback_call -> execute(code_id,
  // args), which shapes the Tuple/Map exactly like a normal call -- and let the
  // rest of the module compile native. A rest body that is not bridge-eligible
  // (its body is effectful) still forces the whole-module fallback via
  // first_reason below.
  std::set<std::uint32_t> rest_body_code_ids;
  for (const amber::bytecode::BcMethod &method : module.methods) {
    for (const amber::bytecode::MethodParamEntry &param : method.params) {
      if ((param.flags & (amber::bytecode::kMethodParamFlagRest |
                          amber::bytecode::kMethodParamFlagKwRest)) != 0U) {
        rest_body_code_ids.insert(method.entry_code_id);
        break;
      }
    }
  }
  if (first_reason.empty()) {
    for (const amber::bytecode::BcCode &code : module.code_objects) {
      const bool is_rest_body =
          rest_body_code_ids.find(code.code_id) != rest_body_code_ids.end();
      std::string reason;
      // A rest body must never be native-compiled: the native calling
      // convention would copy positionals straight into registers without
      // packing the Tuple/Map. Skip the native-supported check and route it to
      // the bridge (or, if effectful, to the whole-module fallback).
      if (!is_rest_body && native_cpp_code_supported(module, code, &reason)) {
        plan.native_code_ids.insert(code.code_id);
        continue;
      }
      std::string vm_callable_reason;
      if (native_cpp_code_vm_callable(module, code, &vm_callable_reason)) {
        plan.vm_callable_code_ids.insert(code.code_id);
        continue;
      }
      if (first_reason.empty()) {
        first_reason =
            "c" + std::to_string(code.code_id) + ": " +
            (is_rest_body ? "rest/keyword-rest body not bridge-eligible: " +
                                vm_callable_reason
                          : reason);
      }
    }
  }

  // A native-bound code object (a `native def` / native class method) must
  // reach the VM so the SEND path can route it to its registered C thunk
  // (5c-ii); pull it onto the per-function VM bridge instead of
  // native-compiling the Amber body. The bridge calls execute(code_id, args),
  // which dispatches to the thunk before the body would run, so even a
  // native-only leaf's NativeRequiredError body is never executed in a native
  // build.
  if (!native_extensions.empty()) {
    const std::string prefix = "amber.native.bind:";
    for (const amber::bytecode::AttrEntry &attr : module.attrs) {
      const std::string key = attr.key_str_id < module.strings.size()
                                  ? module.strings[attr.key_str_id]
                                  : std::string();
      if (key.compare(0, prefix.size(), prefix) != 0) {
        continue;
      }
      std::uint32_t code_id = 0;
      bool parsed = true;
      for (std::size_t i = prefix.size(); i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') {
          parsed = false;
          break;
        }
        code_id = code_id * 10U + static_cast<std::uint32_t>(key[i] - '0');
      }
      if (!parsed || code_id == 0U) {
        continue;
      }
      plan.native_code_ids.erase(code_id);
      plan.vm_callable_code_ids.insert(code_id);
    }
  }

  const std::uint32_t init_code_id =
      artifact.has_entry_init_code_id
          ? artifact.entry_init_code_id
          : (module.init.has_entry_code_id ? module.init.entry_code_id : 0U);
  std::uint32_t main_code_id = 0;
  bool has_main = artifact.has_entry_main_code_id;
  if (artifact.has_entry_main_code_id) {
    main_code_id = artifact.entry_main_code_id;
  } else if (const amber::bytecode::BcMethod *main_method =
                 zero_arg_method_by_name(module, "main")) {
    main_code_id = main_method->entry_code_id;
    has_main = true;
  }
  const auto direct_entry_native = [&](std::uint32_t code_id) {
    if (plan.native_code_ids.find(code_id) == plan.native_code_ids.end()) {
      return false;
    }
    for (const amber::bytecode::BcCode &code : module.code_objects) {
      if (code.code_id == code_id) {
        return code.capture_layout.empty();
      }
    }
    return false;
  };
  const auto direct_entry_has_captures = [&](std::uint32_t code_id) {
    for (const amber::bytecode::BcCode &code : module.code_objects) {
      if (code.code_id == code_id) {
        return !code.capture_layout.empty();
      }
    }
    return false;
  };
  const bool init_native =
      artifact.entry_mode == EntryExecutionMode::MainOnly ||
      !artifact.has_entry_init_code_id || direct_entry_native(init_code_id);
  const bool main_native =
      (artifact.entry_mode != EntryExecutionMode::MainAfterInit &&
       artifact.entry_mode != EntryExecutionMode::MainOnly) ||
      (has_main && direct_entry_native(main_code_id));
  plan.entry_native = init_native && main_native;
  if (!plan.entry_native && first_reason.empty()) {
    const bool init_requires_captures =
        artifact.entry_mode != EntryExecutionMode::MainOnly &&
        artifact.has_entry_init_code_id &&
        direct_entry_has_captures(init_code_id);
    const bool main_requires_captures =
        (artifact.entry_mode == EntryExecutionMode::MainAfterInit ||
         artifact.entry_mode == EntryExecutionMode::MainOnly) &&
        has_main && direct_entry_has_captures(main_code_id);
    first_reason =
        init_requires_captures || main_requires_captures
            ? "direct entry with captures requires VM materialization"
            : "entry code is not native eligible";
  }
  plan.fallback_reason = first_reason;
  for (const amber::bytecode::BcCode &code : module.code_objects) {
    NativeCoverageRecord record;
    record.code_id = code.code_id;
    record.code_kind = amber::bytecode::code_kind_name(code.kind);
    if (plan.native_code_ids.find(code.code_id) != plan.native_code_ids.end()) {
      record.mode = "direct-native";
      record.reason = "direct C++ native codegen";
    } else if (plan.vm_callable_code_ids.find(code.code_id) !=
               plan.vm_callable_code_ids.end()) {
      record.mode = "vm-bridge";
      record.reason = "per-function VM bridge";
    } else {
      record.mode = "fallback";
      record.reason = first_reason.empty()
                          ? "not classified for native execution"
                          : first_reason;
    }
    plan.coverage.push_back(std::move(record));
  }
  plan.uses_bytecode_fallback =
      !plan.entry_native || !plan.vm_callable_code_ids.empty() ||
      plan.native_code_ids.size() != module.code_objects.size();

  std::ostringstream out;
  out << "#include \"bytecode/format.h\"\n";
  out << "#include \"runtime/amber_ext.h\"\n";
  out << "#include \"runtime/amber_ext_runtime.h\"\n";
  out << "#include \"runtime/digest.h\"\n";
  out << "#include \"runtime/stdlib_url.h\"\n";
  out << "#include \"runtime/vm.h\"\n\n";
  out << "#include <algorithm>\n";
  out << "#include <array>\n";
  out << "#include <chrono>\n";
  out << "#include <cerrno>\n";
  out << "#include <cctype>\n";
  out << "#include <cmath>\n";
  out << "#include <cstdint>\n";
  out << "#include <cstdio>\n";
  out << "#include <cstdlib>\n";
  out << "#include <cstring>\n";
  out << "#include <exception>\n";
  out << "#include <filesystem>\n";
  out << "#include <fstream>\n";
  out << "#include <functional>\n";
  out << "#include <iomanip>\n";
  out << "#include <initializer_list>\n";
  out << "#include <iostream>\n";
  out << "#include <limits>\n";
  out << "#include <memory>\n";
  out << "#include <optional>\n";
  out << "#include <charconv>\n";
  out << "#include <regex>\n";
  out << "#include <sstream>\n";
  out << "#include <stdexcept>\n";
  out << "#include <string>\n";
  out << "#include <string_view>\n";
  out << "#include <unordered_map>\n";
  out << "#include <utility>\n";
  out << "#include <vector>\n\n";
  out << "#if defined(__linux__)\n";
  out << "#include <sys/random.h>\n";
  out << "#include <sys/types.h>\n";
  out << "#elif defined(__APPLE__) || defined(__FreeBSD__) || "
         "defined(__OpenBSD__) || defined(__NetBSD__)\n";
  out << "#include <stdlib.h>\n";
  out << "#endif\n\n";
  out << "namespace {\n\n";
  out << "#if defined(__GNUC__) || defined(__clang__)\n";
  out << "#define AMBER_NATIVE_ALWAYS_INLINE inline "
         "__attribute__((always_inline))\n";
  out << "#else\n";
  out << "#define AMBER_NATIVE_ALWAYS_INLINE inline\n";
  out << "#endif\n\n";
  out << emit_embedded_hex_cpp(artifact.bytes);
  out << emit_module_strings_cpp(module);
  out << emit_embedded_capability_grants_cpp(artifact.capability_grants);
  out << "struct NativeBailout : public std::exception {\n";
  out << "  const char *what() const noexcept override { return "
         "\"native bailout\"; }\n";
  out << "};\n\n";
  out << "enum class NativeNumericOverflowMode { Checked, Wrapping, "
         "Saturating };\n";
  out << "struct NativeNumericPolicy {\n";
  out << "  NativeNumericOverflowMode mode = "
         "NativeNumericOverflowMode::Checked;\n";
  out << "  std::int64_t min = std::numeric_limits<std::int64_t>::min();\n";
  out << "  std::int64_t max = std::numeric_limits<std::int64_t>::max();\n";
  out << "  std::uint32_t bits = 64;\n";
  out << "};\n";
  out << "static constexpr NativeNumericPolicy kNativeNumericPolicy{"
      << native_cpp_numeric_mode_expr(plan.numeric_policy.mode) << ", "
      << cpp_native_i64_expr(plan.numeric_policy.min) << ", "
      << cpp_native_i64_expr(plan.numeric_policy.max) << ", "
      << plan.numeric_policy.bits << "U};\n\n";
  out << "struct NativeList;\n";
  out << "struct NativeTuple;\n";
  out << "struct NativeSet;\n";
  out << "struct NativeMap;\n";
  out << "struct NativeRange;\n";
  out << "struct NativeArgParser;\n";
  out << "struct NativeFsPath;\n";
  out << "struct NativeRegexp;\n";
  out << "struct NativeRegexpMatch;\n";
  out << "using NativeTime = amber::runtime::RuntimeTimeValue;\n";
  out << "using NativeTimePeriod = amber::runtime::RuntimeTimePeriodValue;\n";
  out << "using NativeUuid = amber::runtime::RuntimeUuidValue;\n";
  out << "struct NativeClosure;\n";
  out << "struct NativeCell;\n\n";
  out << "struct NativeValue {\n";
  out << "  enum class Tag { Null, Bool, Integer, Float, String, Symbol, "
         "StrType, IntType, BigIntType, FloatType, BoolType, SymbolType, "
         "NullType, ObjectType, MathModule, JsonModule, YamlModule, "
         "BytesModule, "
         "Base64Module, Base64UrlModule, HexModule, "
         "DigestModule, BenchmarkModule, UrlModule, ArgParserModule, "
         "RegexpModule, FsModule, SecureRandomModule, UuidModule, "
         "RangeModule, TimeModule, "
         "TimePeriodModule, Bytes, List, Tuple, Set, Map, Range, ArgParser, "
         "FsPath, Regexp, RegexpMatch, Uuid, Time, TimePeriod, HeapString, "
         "Closure };\n";
  out << "  Tag tag;\n";
  // String payloads are ids into the native string table; interning keeps
  // id equality equivalent to content equality, like the VM.
  out << "  union { std::int64_t scalar_value; double float_value; "
         "void *heap_value; };\n";
  out << "  NativeValue() : tag(Tag::Null), scalar_value(0) {}\n";
  // Heap payloads are intrusively refcounted; copies retain, moves steal,
  // destruction releases. Definitions follow the payload struct definitions.
  out << "  NativeValue(const NativeValue &other);\n";
  out << "  NativeValue(NativeValue &&other) noexcept;\n";
  out << "  NativeValue &operator=(const NativeValue &other);\n";
  out << "  NativeValue &operator=(NativeValue &&other) noexcept;\n";
  out << "  ~NativeValue();\n";
  out << "  void copy_from(const NativeValue &other);\n";
  out << "  void take_from(NativeValue &other);\n";
  out << "  void destroy();\n";
  out << "  static NativeValue nullv() { return {}; }\n";
  out << "  static NativeValue boolean(bool value) { NativeValue out; out.tag "
         "= "
         "Tag::Bool; out.scalar_value = value ? 1 : 0; return out; }\n";
  out << "  static NativeValue integer(std::int64_t value) { NativeValue out; "
         "out.tag = Tag::Integer; out.scalar_value = value; return out; }\n";
  out << "  static NativeValue floating(double value) { NativeValue out; "
         "out.tag = Tag::Float; out.float_value = value; return out; }\n";
  out << "  static NativeValue string_ref(std::int64_t string_id) { "
         "NativeValue out; out.tag = Tag::String; out.scalar_value = "
         "string_id; return out; }\n";
  out << "  static NativeValue symbol_ref(std::int64_t symbol_id) { "
         "NativeValue out; out.tag = Tag::Symbol; out.scalar_value = "
         "symbol_id; return out; }\n";
  out << "  static NativeValue str_type() { NativeValue out; out.tag = "
         "Tag::StrType; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue int_type() { NativeValue out; out.tag = "
         "Tag::IntType; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue bigint_type() { NativeValue out; out.tag = "
         "Tag::BigIntType; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue float_type() { NativeValue out; out.tag = "
         "Tag::FloatType; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue bool_type() { NativeValue out; out.tag = "
         "Tag::BoolType; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue symbol_type() { NativeValue out; out.tag = "
         "Tag::SymbolType; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue null_type() { NativeValue out; out.tag = "
         "Tag::NullType; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue object_type() { NativeValue out; out.tag = "
         "Tag::ObjectType; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue math_module() { NativeValue out; out.tag = "
         "Tag::MathModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue json_module() { NativeValue out; out.tag = "
         "Tag::JsonModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue yaml_module() { NativeValue out; out.tag = "
         "Tag::YamlModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue bytes_module() { NativeValue out; out.tag = "
         "Tag::BytesModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue base64_module() { NativeValue out; out.tag = "
         "Tag::Base64Module; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue base64url_module() { NativeValue out; out.tag = "
         "Tag::Base64UrlModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue hex_module() { NativeValue out; out.tag = "
         "Tag::HexModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue digest_module() { NativeValue out; out.tag = "
         "Tag::DigestModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue benchmark_module() { NativeValue out; out.tag = "
         "Tag::BenchmarkModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue url_module() { NativeValue out; out.tag = "
         "Tag::UrlModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue argparser_module() { NativeValue out; "
         "out.tag = Tag::ArgParserModule; out.scalar_value = 0; return "
         "out; }\n";
  out << "  static NativeValue regexp_module() { NativeValue out; "
         "out.tag = Tag::RegexpModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue fs_module() { NativeValue out; "
         "out.tag = Tag::FsModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue secure_random_module() { NativeValue out; "
         "out.tag = Tag::SecureRandomModule; out.scalar_value = 0; return "
         "out; }\n";
  out << "  static NativeValue uuid_module() { NativeValue out; out.tag = "
         "Tag::UuidModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue range_module() { NativeValue out; out.tag = "
         "Tag::RangeModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue time_module() { NativeValue out; out.tag = "
         "Tag::TimeModule; out.scalar_value = 0; return out; }\n";
  out << "  static NativeValue time_period_module() { NativeValue out; "
         "out.tag = Tag::TimePeriodModule; out.scalar_value = 0; return "
         "out; }\n";
  out << "  static NativeValue heap_string(std::string text);\n";
  out << "  static NativeValue bytes(std::string value);\n";
  out << "  static NativeValue list(std::vector<NativeValue> items);\n";
  out << "  static NativeValue tuple(std::vector<NativeValue> items);\n";
  out << "  static NativeValue set(std::vector<NativeValue> items);\n";
  out << "  static NativeValue map(std::vector<std::pair<std::string, "
         "NativeValue>> entries);\n";
  out << "  static NativeValue map_entries("
         "std::vector<std::pair<NativeValue, NativeValue>> entries, "
         "bool strict = false);\n";
  out << "  static NativeValue range(NativeRange value);\n";
  out << "  static NativeValue arg_parser(NativeArgParser value);\n";
  out << "  static NativeValue fs_path(NativeFsPath value);\n";
  out << "  static NativeValue regexp(NativeRegexp value);\n";
  out << "  static NativeValue regexp_match(NativeRegexpMatch value);\n";
  out << "  static NativeValue uuid(amber::runtime::RuntimeUuidValue value);\n";
  out << "  static NativeValue time(amber::runtime::RuntimeTimeValue value);\n";
  out << "  static NativeValue time_period("
         "amber::runtime::RuntimeTimePeriodValue value);\n";
  out << "  static NativeValue closure(NativeClosure *value);\n";
  out << "};\n\n";
  // Every refcounted payload starts with its rc so retain/release can use a
  // uniform header instead of a per-type switch on the hot path.
  out << "struct NativeRcHeader { std::uint32_t rc = 1; };\n";
  // Per-type free list: dead payload shells are recycled instead of paying a
  // malloc/free round-trip per value. Growth is bounded by the peak live
  // count of each type. Single-threaded like the rest of the native lane.
  out << "#define AMBER_NATIVE_POOL_NEW \\\n";
  out << "  static void *&native_pool_head() { "
         "static void *head = nullptr; return head; } \\\n";
  out << "  static void *operator new(std::size_t size) { \\\n";
  out << "    void *&head = native_pool_head(); \\\n";
  out << "    if (head != nullptr) { \\\n";
  out << "      void *taken = head; \\\n";
  out << "      head = *static_cast<void **>(taken); \\\n";
  out << "      return taken; \\\n";
  out << "    } \\\n";
  out << "    return ::operator new(size); \\\n";
  out << "  } \\\n";
  out << "  static void operator delete(void *pointer) noexcept { \\\n";
  out << "    void *&head = native_pool_head(); \\\n";
  out << "    *static_cast<void **>(pointer) = head; \\\n";
  out << "    head = pointer; \\\n";
  out << "  }\n";
  out << "struct NativeBytes : NativeRcHeader { std::string bytes; "
         "AMBER_NATIVE_POOL_NEW };\n";
  out << "struct NativeList : NativeRcHeader { "
         "std::vector<NativeValue> items; AMBER_NATIVE_POOL_NEW };\n";
  out << "struct NativeTuple : NativeRcHeader { "
         "std::vector<NativeValue> items; AMBER_NATIVE_POOL_NEW };\n";
  out << "struct NativeSet : NativeRcHeader { "
         "std::vector<NativeValue> items; AMBER_NATIVE_POOL_NEW };\n";
  out << "static constexpr std::size_t kNativeMapInlineNameIndexCapacity = "
         "8;\n";
  out << "struct NativeMap : NativeRcHeader {\n";
  out << "  std::vector<std::pair<NativeValue, NativeValue>> entries;\n";
  out << "  std::array<std::pair<std::int64_t, std::size_t>, "
         "kNativeMapInlineNameIndexCapacity> inline_name_index{};\n";
  out << "  std::size_t inline_name_index_size = 0;\n";
  out << "  std::unordered_map<std::int64_t, std::size_t> name_index;\n";
  out << "  bool strict = false;\n";
  out << "  AMBER_NATIVE_POOL_NEW\n";
  out << "};\n";
  out << "struct NativeRange : NativeRcHeader {\n";
  out << "  std::int64_t start = 0;\n";
  out << "  std::int64_t finish = 0;\n";
  out << "  std::int64_t step = 1;\n";
  out << "  bool inclusive_end = true;\n";
  out << "};\n";
  out << "struct NativeFsPath : NativeRcHeader {\n";
  out << "  std::string path;\n";
  out << "  NativeFsPath() = default;\n";
  out << "  explicit NativeFsPath(std::string p) : path(std::move(p)) {}\n";
  out << "};\n";
  out << "struct NativeRegexp : NativeRcHeader {\n";
  out << "  std::string source;\n";
  out << "  std::regex::flag_type flags = std::regex::ECMAScript;\n";
  out << "  std::regex compiled;\n";
  out << "};\n";
  out << "struct NativeRegexpCapture {\n";
  out << "  bool matched = false;\n";
  out << "  std::size_t start = 0;\n";
  out << "  std::size_t end = 0;\n";
  out << "};\n";
  out << "struct NativeRegexpMatch : NativeRcHeader {\n";
  out << "  std::string source;\n";
  out << "  NativeValue pattern = NativeValue::nullv();\n";
  out << "  std::vector<NativeRegexpCapture> captures;\n";
  out << "};\n";
  out << "enum class NativeArgValueType { Str, Int, Float, Bool, Symbol };\n";
  out << "struct NativeArgParser : NativeRcHeader {\n";
  out << "  enum class SpecKind { Option, Flag, Positional, Rest };\n";
  out << "  struct Spec {\n";
  out << "    SpecKind kind = SpecKind::Option;\n";
  out << "    std::vector<std::string> spellings;\n";
  out << "    std::string name;\n";
  out << "    NativeArgValueType type = NativeArgValueType::Str;\n";
  out << "    bool required = false;\n";
  out << "    bool multiple = false;\n";
  out << "    bool negatable = false;\n";
  out << "    bool has_default = false;\n";
  out << "    NativeValue default_value = NativeValue::nullv();\n";
  out << "    bool has_choices = false;\n";
  out << "    std::vector<NativeValue> choices;\n";
  out << "    std::string env;\n";
  out << "  };\n";
  out << "  std::string name;\n";
  out << "  std::string about;\n";
  out << "  std::vector<std::string> cmdline;\n";
  out << "  std::vector<std::pair<std::string, std::string>> env;\n";
  out << "  bool add_help = true;\n";
  out << "  std::vector<Spec> specs;\n";
  out << "};\n";
  // Time/TimePeriod/Uuid stay plain runtime value types in helper code;
  // heap-stored instances carry their refcount in an explicit box.
  out << "struct NativeTimeBox : NativeRcHeader {\n";
  out << "  NativeTime value;\n";
  out << "  explicit NativeTimeBox(NativeTime v) : value(std::move(v)) {}\n";
  out << "  AMBER_NATIVE_POOL_NEW\n";
  out << "};\n";
  out << "struct NativeTimePeriodBox : NativeRcHeader {\n";
  out << "  NativeTimePeriod value;\n";
  out << "  explicit NativeTimePeriodBox(NativeTimePeriod v) : "
         "value(std::move(v)) {}\n";
  out << "  AMBER_NATIVE_POOL_NEW\n";
  out << "};\n";
  out << "struct NativeUuidBox : NativeRcHeader {\n";
  out << "  NativeUuid value;\n";
  out << "  explicit NativeUuidBox(NativeUuid v) : value(std::move(v)) {}\n";
  out << "  AMBER_NATIVE_POOL_NEW\n";
  out << "};\n";
  // Dynamic string results live here; only module literals, symbols, and
  // map-key canonicalization use the permanent intern table.
  out << "struct NativeHeapString : NativeRcHeader {\n";
  out << "  std::string text;\n";
  out << "  NativeHeapString() = default;\n";
  out << "  explicit NativeHeapString(std::string t) : text(std::move(t)) {}\n";
  out << "  AMBER_NATIVE_POOL_NEW\n";
  out << "};\n";
  out << "struct NativeCell { NativeValue value; };\n";
  out << "struct NativeClosure {\n";
  out << "  std::uint32_t code_id = 0;\n";
  out << "  std::vector<NativeCell *> captures;\n";
  out << "  NativeValue self = NativeValue::nullv();\n";
  out << "};\n\n";
  out << R"AMBERCPP(// Refcounted tags are contiguous; the hot paths are a bit copy plus one
// range test. Payload deletion is the only place that needs the type switch.
inline bool native_value_tag_refcounted(NativeValue::Tag tag) {
  return tag >= NativeValue::Tag::Bytes && tag <= NativeValue::Tag::HeapString;
}
inline bool native_value_is_string(const NativeValue &value) {
  return value.tag == NativeValue::Tag::String ||
         value.tag == NativeValue::Tag::HeapString;
}
static void native_value_delete_payload(NativeValue::Tag tag, void *payload);
AMBER_NATIVE_ALWAYS_INLINE void NativeValue::copy_from(const NativeValue &other) {
  tag = other.tag;
  heap_value = other.heap_value;
  if (native_value_tag_refcounted(tag) && heap_value != nullptr) {
    ++static_cast<NativeRcHeader *>(heap_value)->rc;
  }
}
AMBER_NATIVE_ALWAYS_INLINE void NativeValue::take_from(NativeValue &other) {
  tag = other.tag;
  heap_value = other.heap_value;
  other.tag = Tag::Null;
  other.scalar_value = 0;
}
AMBER_NATIVE_ALWAYS_INLINE void NativeValue::destroy() {
  if (native_value_tag_refcounted(tag) && heap_value != nullptr) {
    auto *header = static_cast<NativeRcHeader *>(heap_value);
    if (--header->rc == 0) native_value_delete_payload(tag, heap_value);
  }
}
AMBER_NATIVE_ALWAYS_INLINE NativeValue::NativeValue(const NativeValue &other) { copy_from(other); }
AMBER_NATIVE_ALWAYS_INLINE NativeValue::NativeValue(NativeValue &&other) noexcept {
  take_from(other);
}
AMBER_NATIVE_ALWAYS_INLINE NativeValue &NativeValue::operator=(const NativeValue &other) {
  if (this == &other) return *this;
  NativeValue retained(other);
  destroy();
  take_from(retained);
  return *this;
}
AMBER_NATIVE_ALWAYS_INLINE NativeValue &NativeValue::operator=(NativeValue &&other) noexcept {
  if (this == &other) return *this;
  NativeValue stolen(std::move(other));
  destroy();
  take_from(stolen);
  return *this;
}
AMBER_NATIVE_ALWAYS_INLINE NativeValue::~NativeValue() { destroy(); }
static void native_value_delete_payload(NativeValue::Tag tag, void *payload) {
  switch (tag) {
    case NativeValue::Tag::Bytes:
      delete static_cast<NativeBytes *>(payload); return;
    case NativeValue::Tag::List:
      delete static_cast<NativeList *>(payload); return;
    case NativeValue::Tag::Tuple:
      delete static_cast<NativeTuple *>(payload); return;
    case NativeValue::Tag::Set:
      delete static_cast<NativeSet *>(payload); return;
    case NativeValue::Tag::Map:
      delete static_cast<NativeMap *>(payload); return;
    case NativeValue::Tag::Range:
      delete static_cast<NativeRange *>(payload); return;
    case NativeValue::Tag::ArgParser:
      delete static_cast<NativeArgParser *>(payload); return;
    case NativeValue::Tag::FsPath:
      delete static_cast<NativeFsPath *>(payload); return;
    case NativeValue::Tag::Regexp:
      delete static_cast<NativeRegexp *>(payload); return;
    case NativeValue::Tag::RegexpMatch:
      delete static_cast<NativeRegexpMatch *>(payload); return;
    case NativeValue::Tag::Uuid:
      delete static_cast<NativeUuidBox *>(payload); return;
    case NativeValue::Tag::Time:
      delete static_cast<NativeTimeBox *>(payload); return;
    case NativeValue::Tag::TimePeriod:
      delete static_cast<NativeTimePeriodBox *>(payload); return;
    case NativeValue::Tag::HeapString:
      delete static_cast<NativeHeapString *>(payload); return;
    default: return;
  }
}

)AMBERCPP";
  out << "struct NativeArena {\n";
  out << "  std::vector<std::unique_ptr<NativeClosure>> closures;\n";
  out << "  std::vector<std::unique_ptr<NativeCell>> cells;\n";
  out << "};\n";
  out << "static NativeArena native_arena;\n\n";
  out << "static const std::string &native_string_text("
         "const NativeValue &value);\n";
  out << "static std::optional<std::int64_t> native_map_index_key_id("
         "const NativeValue &key, bool strict) {\n";
  out << "  if (strict) return std::nullopt;\n";
  out << "  if (key.tag == NativeValue::Tag::String) return "
         "key.scalar_value;\n";
  out << "  if (key.tag == NativeValue::Tag::HeapString) return "
         "native_intern_string(native_string_text(key));\n";
  out << "  if (key.tag == NativeValue::Tag::Symbol) return "
         "native_intern_string(native_symbol_text(key.scalar_value));\n";
  out << "  return std::nullopt;\n";
  out << "}\n";
  // Read-side variant: never inserts. A nameable probe whose text is not in
  // the intern table cannot match any stored nameable key, because stores
  // intern every nameable key.
  out << "static std::optional<std::int64_t> native_map_index_key_id_for_read("
         "const NativeValue &key, bool strict) {\n";
  out << "  if (strict) return std::nullopt;\n";
  out << "  if (key.tag == NativeValue::Tag::String) return "
         "key.scalar_value;\n";
  out << "  if (key.tag == NativeValue::Tag::HeapString) return "
         "native_intern_string_lookup(native_string_text(key));\n";
  out << "  if (key.tag == NativeValue::Tag::Symbol) return "
         "native_intern_string_lookup(native_symbol_text(key.scalar_value));\n";
  out << "  return std::nullopt;\n";
  out << "}\n";
  out << "static std::optional<std::size_t> "
         "native_map_find_inline_name_index(const NativeMap &map, "
         "std::int64_t key_id) {\n";
  out << "  for (std::size_t i = 0; i < map.inline_name_index_size; ++i) {\n";
  out << "    const auto &entry = map.inline_name_index[i];\n";
  out << "    if (entry.first == key_id && entry.second < map.entries.size()) "
         "return entry.second;\n";
  out << "  }\n";
  out << "  return std::nullopt;\n";
  out << "}\n";
  out << "static void native_map_add_inline_name_index(NativeMap &map, "
         "std::int64_t key_id, std::size_t index) {\n";
  out << "  if (map.inline_name_index_size >= "
         "kNativeMapInlineNameIndexCapacity) return;\n";
  out << "  map.inline_name_index[map.inline_name_index_size++] = {key_id, "
         "index};\n";
  out << "}\n";
  out << "static void native_map_rebuild_index(NativeMap &map) {\n";
  out << "  map.name_index.clear();\n";
  out << "  map.inline_name_index_size = 0;\n";
  out << "  if (map.strict) return;\n";
  out << "  if (map.entries.size() <= kNativeMapInlineNameIndexCapacity) {\n";
  out << "    for (std::size_t i = 0; i < map.entries.size(); ++i) {\n";
  out << "      const std::optional<std::int64_t> key_id = "
         "native_map_index_key_id(map.entries[i].first, map.strict);\n";
  out << "      if (key_id.has_value()) "
         "native_map_add_inline_name_index(map, *key_id, i);\n";
  out << "    }\n";
  out << "    return;\n";
  out << "  }\n";
  out << "  map.name_index.reserve(map.entries.size());\n";
  out << "  for (std::size_t i = 0; i < map.entries.size(); ++i) {\n";
  out << "    const std::optional<std::int64_t> key_id = "
         "native_map_index_key_id(map.entries[i].first, map.strict);\n";
  out << "    if (key_id.has_value()) map.name_index.emplace(*key_id, i);\n";
  out << "  }\n";
  out << "}\n\n";
  out << "NativeValue NativeValue::heap_string(std::string text) {\n";
  out << "  NativeValue out;\n";
  out << "  out.heap_value = new NativeHeapString(std::move(text));\n";
  out << "  out.tag = Tag::HeapString; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::bytes(std::string value) {\n";
  out << "  NativeValue out;\n";
  out << "  auto *bytes = new NativeBytes();\n";
  out << "  bytes->bytes = std::move(value);\n";
  out << "  out.heap_value = bytes; out.tag = Tag::Bytes; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::list(std::vector<NativeValue> items) {\n";
  out << "  NativeValue out;\n";
  out << "  auto *list = new NativeList();\n";
  out << "  list->items = std::move(items);\n";
  out << "  out.heap_value = list; out.tag = Tag::List; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::tuple(std::vector<NativeValue> items) {\n";
  out << "  NativeValue out;\n";
  out << "  auto *tuple = new NativeTuple();\n";
  out << "  tuple->items = std::move(items);\n";
  out << "  out.heap_value = tuple; out.tag = Tag::Tuple; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::set(std::vector<NativeValue> items) {\n";
  out << "  NativeValue out;\n";
  out << "  auto *set = new NativeSet();\n";
  out << "  set->items = std::move(items);\n";
  out << "  out.heap_value = set; out.tag = Tag::Set; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::map_entries("
         "std::vector<std::pair<NativeValue, NativeValue>> entries, "
         "bool strict) {\n";
  out << "  NativeValue out;\n";
  out << "  auto *map = new NativeMap();\n";
  out << "  map->entries = std::move(entries); map->strict = strict; "
         "native_map_rebuild_index(*map);\n";
  out << "  out.heap_value = map; out.tag = Tag::Map; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::map(std::vector<std::pair<std::string, "
         "NativeValue>> entries) {\n";
  out << "  std::vector<std::pair<NativeValue, NativeValue>> converted;\n";
  out << "  converted.reserve(entries.size());\n";
  out << "  for (auto &entry : entries) {\n";
  out << "    converted.emplace_back("
         "NativeValue::string_ref(native_intern_string(entry.first)), "
         "std::move(entry.second));\n";
  out << "  }\n";
  out << "  return NativeValue::map_entries(std::move(converted), false);\n";
  out << "}\n";
  out << "NativeValue NativeValue::range(NativeRange value) {\n";
  out << "  NativeValue out;\n";
  out << "  out.heap_value = new NativeRange(value);\n";
  out << "  out.tag = Tag::Range; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::arg_parser(NativeArgParser value) {\n";
  out << "  NativeValue out;\n";
  out << "  out.heap_value = new NativeArgParser(std::move(value));\n";
  out << "  out.tag = Tag::ArgParser; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::fs_path(NativeFsPath value) {\n";
  out << "  NativeValue out;\n";
  out << "  out.heap_value = new NativeFsPath(std::move(value));\n";
  out << "  out.tag = Tag::FsPath; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::regexp(NativeRegexp value) {\n";
  out << "  NativeValue out;\n";
  out << "  out.heap_value = new NativeRegexp(std::move(value));\n";
  out << "  out.tag = Tag::Regexp; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::regexp_match(NativeRegexpMatch value) {\n";
  out << "  NativeValue out;\n";
  out << "  out.heap_value = new NativeRegexpMatch(std::move(value));\n";
  out << "  out.tag = Tag::RegexpMatch; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::uuid(amber::runtime::RuntimeUuidValue "
         "value) {\n";
  out << "  NativeValue out;\n";
  out << "  out.heap_value = new NativeUuidBox{value};\n";
  out << "  out.tag = Tag::Uuid; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::time(amber::runtime::RuntimeTimeValue "
         "value) {\n";
  out << "  NativeValue out;\n";
  out << "  out.heap_value = new NativeTimeBox{value};\n";
  out << "  out.tag = Tag::Time; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::time_period("
         "amber::runtime::RuntimeTimePeriodValue value) {\n";
  out << "  NativeValue out;\n";
  out << "  out.heap_value = new NativeTimePeriodBox{value};\n";
  out << "  out.tag = Tag::TimePeriod; return out;\n";
  out << "}\n";
  out << "NativeValue NativeValue::closure(NativeClosure *value) {\n";
  out << "  NativeValue out; out.tag = Tag::Closure;\n";
  out << "  out.heap_value = value; return out;\n";
  out << "}\n\n";
  out << "static NativeClosure *make_native_closure() {\n";
  out << "  auto value = std::make_unique<NativeClosure>();\n";
  out << "  NativeClosure *raw = value.get();\n";
  out << "  native_arena.closures.push_back(std::move(value)); return raw;\n";
  out << "}\n";
  out << "static NativeCell *make_native_cell(NativeValue value) {\n";
  out << "  auto cell = std::make_unique<NativeCell>(); cell->value = value;\n";
  out << "  NativeCell *raw = cell.get();\n";
  out << "  native_arena.cells.push_back(std::move(cell)); return raw;\n";
  out << "}\n\n";
  out << "struct NativeFrame {\n";
  out << "  NativeValue *regs = nullptr;\n";
  out << "  NativeCell **local_cells = nullptr;\n";
  out << "  std::size_t reg_count = 0;\n";
  out << "  NativeClosure *closure = nullptr;\n";
  out << "  NativeValue self = NativeValue::nullv();\n";
  out << "  NativeValue last = NativeValue::nullv();\n";
  out << "  NativeFrame(NativeValue *frame_regs, NativeCell **frame_cells, "
         "std::size_t frame_reg_count, NativeClosure *current)\n";
  out << "      : regs(frame_regs), local_cells(frame_cells), "
         "reg_count(frame_reg_count), closure(current) {\n";
  out << "    if (closure != nullptr) self = closure->self;\n";
  out << "  }\n";
  out << "};\n\n";
  out << "static NativeValue read_reg(const NativeFrame &frame, "
         "std::uint32_t slot) {\n";
  out << "  if (slot >= frame.reg_count) throw NativeBailout();\n";
  out << "  if (frame.local_cells != nullptr && "
         "frame.local_cells[slot] != nullptr) "
         "return frame.local_cells[slot]->value;\n";
  out << "  return frame.regs[slot];\n";
  out << "}\n";
  out << "static void write_reg(NativeFrame &frame, std::uint32_t slot, "
         "NativeValue value) {\n";
  out << "  if (slot >= frame.reg_count) throw NativeBailout();\n";
  out << "  frame.regs[slot] = value;\n";
  out << "  if (frame.local_cells != nullptr && "
         "frame.local_cells[slot] != nullptr) "
         "frame.local_cells[slot]->value = std::move(value);\n";
  out << "}\n";
  out << "static NativeCell *local_cell(NativeFrame &frame, "
         "std::uint32_t slot) {\n";
  out << "  if (slot >= frame.reg_count || frame.local_cells == nullptr) "
         "throw NativeBailout();\n";
  out << "  if (frame.local_cells[slot] == nullptr) {\n";
  out << "    frame.local_cells[slot] = make_native_cell(frame.regs[slot]);\n";
  out << "  }\n";
  out << "  return frame.local_cells[slot];\n";
  out << "}\n";
  out << "static NativeCell *capture_cell(const NativeFrame &frame, "
         "std::uint32_t slot) {\n";
  out << "  if (frame.closure == nullptr || "
         "slot >= frame.closure->captures.size() || "
         "frame.closure->captures[slot] == nullptr) throw NativeBailout();\n";
  out << "  return frame.closure->captures[slot];\n";
  out << "}\n";
  out << "static NativeValue read_capture(const NativeFrame &frame, "
         "std::uint32_t slot) { return capture_cell(frame, slot)->value; }\n";
  out << "static void write_capture(const NativeFrame &frame, std::uint32_t "
         "slot, "
         "NativeValue value) { capture_cell(frame, slot)->value = "
         "std::move(value); }\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "as_int(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Integer) { throw "
         "NativeBailout(); }\n";
  out << "  return value.scalar_value;\n";
  out << "}\n\n";
  out << "static const NativeList &as_list(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::List || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeList *>(value.heap_value);\n";
  out << "}\n";
  out << "static NativeList &as_mutable_list(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::List || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeList *>(value.heap_value);\n";
  out << "}\n";
  out << "static const NativeTuple &as_tuple(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Tuple || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeTuple *>(value.heap_value);\n";
  out << "}\n";
  out << "static const NativeSet &as_set(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Set || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeSet *>(value.heap_value);\n";
  out << "}\n";
  out << "static NativeSet &as_mutable_set(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Set || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeSet *>(value.heap_value);\n";
  out << "}\n";
  out << "static const NativeMap &as_map(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Map || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeMap *>(value.heap_value);\n";
  out << "}\n";
  out << "static NativeMap &as_mutable_map(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Map || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeMap *>(value.heap_value);\n";
  out << "}\n";
  out << "static const NativeBytes &as_bytes(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Bytes || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeBytes *>(value.heap_value);\n";
  out << "}\n";
  out << "static const NativeRange &as_range(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Range || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeRange *>(value.heap_value);\n";
  out << "}\n";
  out << "static const NativeArgParser &as_arg_parser("
         "const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::ArgParser || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeArgParser *>(value.heap_value);\n";
  out << "}\n";
  out << "static NativeArgParser &as_mutable_arg_parser("
         "const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::ArgParser || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeArgParser *>(value.heap_value);\n";
  out << "}\n";
  out << "static const NativeFsPath &as_fs_path("
         "const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::FsPath || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeFsPath *>(value.heap_value);\n";
  out << "}\n";
  out << "static const NativeRegexp &as_regexp("
         "const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Regexp || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeRegexp *>(value.heap_value);\n";
  out << "}\n";
  out << "static const NativeRegexpMatch &as_regexp_match("
         "const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::RegexpMatch || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return *static_cast<NativeRegexpMatch *>(value.heap_value);\n";
  out << "}\n";
  out << "static const NativeUuid &as_uuid(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Uuid || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return static_cast<NativeUuidBox *>(value.heap_value)->value;\n";
  out << "}\n";
  out << "static const NativeTime &as_time(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Time || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return static_cast<NativeTimeBox *>(value.heap_value)->value;\n";
  out << "}\n";
  out << "static const NativeTimePeriod &as_time_period("
         "const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::TimePeriod || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return static_cast<NativeTimePeriodBox *>"
         "(value.heap_value)->value;\n";
  out << "}\n";
  out << "static NativeClosure *as_closure(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Closure || "
         "value.heap_value == nullptr) throw NativeBailout();\n";
  out << "  return static_cast<NativeClosure *>(value.heap_value);\n";
  out << "}\n";
  out << "static std::optional<std::size_t> native_optional_sequence_index("
         "std::int64_t index, std::size_t size) {\n";
  out << "  const std::int64_t size_i64 = static_cast<std::int64_t>(size);\n";
  out << "  const std::int64_t normalized = index < 0 ? size_i64 + index : "
         "index;\n";
  out << "  if (normalized < 0 || static_cast<std::uint64_t>(normalized) >= "
         "size) return std::nullopt;\n";
  out << "  return static_cast<std::size_t>(normalized);\n";
  out << "}\n";
  out << "static std::size_t native_sequence_index(std::int64_t index, "
         "std::size_t size) {\n";
  out << "  const std::optional<std::size_t> normalized = "
         "native_optional_sequence_index(index, size);\n";
  out << "  if (!normalized.has_value()) throw NativeBailout();\n";
  out << "  return *normalized;\n";
  out << "}\n";
  out << "static std::vector<NativeValue> native_range_items("
         "const NativeValue &value) {\n";
  out << "  const NativeRange &range = as_range(value);\n";
  out << "  if (range.step == 0) throw NativeBailout();\n";
  out << "  const __int128 start = static_cast<__int128>(range.start);\n";
  out << "  const __int128 finish = static_cast<__int128>(range.finish);\n";
  out << "  const __int128 step = static_cast<__int128>(range.step);\n";
  out << "  const __int128 last = range.inclusive_end ? finish : "
         "(step > 0 ? finish - 1 : finish + 1);\n";
  out << "  std::vector<NativeValue> items;\n";
  out << "  if ((step > 0 && start > last) || (step < 0 && start < last)) "
         "return items;\n";
  out << "  for (__int128 current = start; step > 0 ? current <= last : "
         "current >= last; current += step) {\n";
  out << "    if (current < static_cast<__int128>("
         "std::numeric_limits<std::int64_t>::min()) || current > "
         "static_cast<__int128>(std::numeric_limits<std::int64_t>::max())) "
         "throw NativeBailout();\n";
  out << "    items.push_back(NativeValue::integer("
         "static_cast<std::int64_t>(current)));\n";
  out << "    if (items.size() == std::numeric_limits<std::size_t>::max()) "
         "throw NativeBailout();\n";
  out << "  }\n";
  out << "  return items;\n";
  out << "}\n";
  out << "static std::vector<NativeValue> native_sequence_items_copy("
         "const NativeValue &value) {\n";
  out << "  if (value.tag == NativeValue::Tag::List) return "
         "as_list(value).items;\n";
  out << "  if (value.tag == NativeValue::Tag::Tuple) return "
         "as_tuple(value).items;\n";
  out << "  if (value.tag == NativeValue::Tag::Set) return "
         "as_set(value).items;\n";
  out << "  if (value.tag == NativeValue::Tag::Range) return "
         "native_range_items(value);\n";
  out << "  throw NativeBailout();\n";
  out << "}\n";
  // Borrowing view for read-only paths with no callbacks between borrow and
  // use; Range has no backing vector and returns nullptr (callers fall back
  // to the materializing copy).
  out << "static const std::vector<NativeValue> *native_sequence_items_view("
         "const NativeValue &value) {\n";
  out << "  if (value.tag == NativeValue::Tag::List) return "
         "&as_list(value).items;\n";
  out << "  if (value.tag == NativeValue::Tag::Tuple) return "
         "&as_tuple(value).items;\n";
  out << "  if (value.tag == NativeValue::Tag::Set) return "
         "&as_set(value).items;\n";
  out << "  return nullptr;\n";
  out << "}\n";
  out << "static NativeValue native_list_at(const NativeValue &value, "
         "const NativeValue &index_value) {\n";
  out << "  const std::int64_t raw_index = as_int(index_value);\n";
  out << "  if (value.tag == NativeValue::Tag::Range && raw_index < 0) "
         "throw NativeBailout();\n";
  out << "  if (const std::vector<NativeValue> *items = "
         "native_sequence_items_view(value)) {\n";
  out << "    return (*items)[native_sequence_index(raw_index, "
         "items->size())];\n";
  out << "  }\n";
  out << "  const std::vector<NativeValue> items = "
         "native_sequence_items_copy(value);\n";
  out << "  return items[native_sequence_index(raw_index, items.size())];\n";
  out << "}\n";
  out << "static NativeValue native_list_set(const NativeValue &value, "
         "const NativeValue &index_value, const NativeValue &next_value) {\n";
  out << "  auto &items = as_mutable_list(value).items;\n";
  out << "  const std::size_t index = "
         "native_sequence_index(as_int(index_value), "
         "items.size());\n";
  out << "  items[index] = next_value;\n";
  out << "  return next_value;\n";
  out << "}\n";
  out << "static NativeValue native_count(const NativeValue &value) {\n";
  out << "  if (const std::vector<NativeValue> *items = "
         "native_sequence_items_view(value)) return "
         "NativeValue::integer(static_cast<std::int64_t>(items->size()));\n";
  out << "  if (value.tag == NativeValue::Tag::Range) return "
         "NativeValue::integer(static_cast<std::int64_t>("
         "native_sequence_items_copy(value).size()));\n";
  out << "  if (value.tag == NativeValue::Tag::Map) return "
         "NativeValue::integer(static_cast<std::int64_t>("
         "as_map(value).entries.size()));\n";
  out << "  if (value.tag == NativeValue::Tag::Bytes) return "
         "NativeValue::integer(static_cast<std::int64_t>("
         "as_bytes(value).bytes.size()));\n";
  out << "  if (value.tag == NativeValue::Tag::RegexpMatch) {\n";
  out << "    const NativeRegexpMatch &match = as_regexp_match(value);\n";
  out << "    const std::size_t captures = match.captures.empty() ? 0U : "
         "match.captures.size() - 1U;\n";
  out << "    return NativeValue::integer(static_cast<std::int64_t>("
         "captures));\n";
  out << "  }\n";
  out << "  throw NativeBailout();\n";
  out << "}\n";
  out << "static NativeValue native_list_first(const NativeValue &value, "
         "const NativeValue &count_value, bool has_count) {\n";
  out << "  const std::vector<NativeValue> *view = "
         "native_sequence_items_view(value);\n";
  out << "  std::vector<NativeValue> materialized;\n";
  out << "  if (view == nullptr) {\n";
  out << "    materialized = native_sequence_items_copy(value);\n";
  out << "    view = &materialized;\n";
  out << "  }\n";
  out << "  const std::vector<NativeValue> &items = *view;\n";
  out << "  if (!has_count) return items.empty() ? NativeValue::nullv() : "
         "items.front();\n";
  out << "  const std::int64_t raw_count = as_int(count_value);\n";
  out << "  const std::size_t take = raw_count <= 0 ? 0U : "
         "std::min<std::size_t>(static_cast<std::size_t>(raw_count), "
         "items.size());\n";
  out << "  return NativeValue::list(std::vector<NativeValue>("
         "items.begin(), items.begin() + take));\n";
  out << "}\n\n";
  out << "static void native_map_store(std::vector<std::pair<std::string, "
         "NativeValue>> &entries, std::string key, NativeValue value) {\n";
  out << "  for (auto &entry : entries) {\n";
  out << "    if (entry.first == key) { entry.second = std::move(value); "
         "return; }\n";
  out << "  }\n";
  out << "  entries.emplace_back(std::move(key), std::move(value));\n";
  out << "}\n\n";
  out << "static bool truthy(const NativeValue &value) {\n";
  out << "  return value.tag != NativeValue::Tag::Null && "
         "!(value.tag == NativeValue::Tag::Bool && value.scalar_value == 0);\n";
  out << "}\n\n";
  out << "static const char *native_type_tag_name(const NativeValue &value) "
         "{\n";
  out << "  switch (value.tag) {\n";
  out << "  case NativeValue::Tag::StrType: return \"Str\";\n";
  out << "  case NativeValue::Tag::IntType: return \"Int\";\n";
  out << "  case NativeValue::Tag::BigIntType: return \"BigInt\";\n";
  out << "  case NativeValue::Tag::FloatType: return \"Float\";\n";
  out << "  case NativeValue::Tag::BoolType: return \"Bool\";\n";
  out << "  case NativeValue::Tag::SymbolType: return \"Symbol\";\n";
  out << "  case NativeValue::Tag::NullType: return \"Null\";\n";
  out << "  case NativeValue::Tag::ObjectType: return \"Object\";\n";
  out << "  case NativeValue::Tag::MathModule: return \"Math\";\n";
  out << "  case NativeValue::Tag::JsonModule: return \"Json\";\n";
  out << "  case NativeValue::Tag::YamlModule: return \"Yaml\";\n";
  out << "  case NativeValue::Tag::BytesModule: return \"Bytes\";\n";
  out << "  case NativeValue::Tag::Base64Module: return \"Base64\";\n";
  out << "  case NativeValue::Tag::Base64UrlModule: return \"Base64Url\";\n";
  out << "  case NativeValue::Tag::HexModule: return \"Hex\";\n";
  out << "  case NativeValue::Tag::DigestModule: return \"Digest\";\n";
  out << "  case NativeValue::Tag::BenchmarkModule: return \"Benchmark\";\n";
  out << "  case NativeValue::Tag::UrlModule: return \"Url\";\n";
  out << "  case NativeValue::Tag::ArgParserModule: return \"ArgParser\";\n";
  out << "  case NativeValue::Tag::RegexpModule: return \"Regexp\";\n";
  out << "  case NativeValue::Tag::FsModule: return \"fs\";\n";
  out << "  case NativeValue::Tag::SecureRandomModule: return "
         "\"SecureRandom\";\n";
  out << "  case NativeValue::Tag::UuidModule: return \"Uuid\";\n";
  out << "  case NativeValue::Tag::RangeModule: return \"Range\";\n";
  out << "  case NativeValue::Tag::TimeModule: return \"Time\";\n";
  out << "  case NativeValue::Tag::TimePeriodModule: return \"TimePeriod\";\n";
  out << "  default: throw NativeBailout();\n";
  out << "  }\n";
  out << "}\n\n";
  out << "static NativeValue native_value_class(const NativeValue &value) "
         "{\n";
  out << "  switch (value.tag) {\n";
  out << "  case NativeValue::Tag::Null: return NativeValue::null_type();\n";
  out << "  case NativeValue::Tag::Bool: return NativeValue::bool_type();\n";
  out << "  case NativeValue::Tag::Integer: return NativeValue::int_type();\n";
  out << "  case NativeValue::Tag::Float: return NativeValue::float_type();\n";
  out << "  case NativeValue::Tag::String:\n";
  out << "  case NativeValue::Tag::HeapString: return "
         "NativeValue::str_type();\n";
  out << "  case NativeValue::Tag::Symbol: return "
         "NativeValue::symbol_type();\n";
  out << "  default: throw NativeBailout();\n";
  out << "  }\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "compare_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  if (lhs < rhs) return -1;\n";
  out << "  if (lhs > rhs) return 1;\n";
  out << "  return 0;\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "floor_div_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  if (rhs == 0) throw NativeBailout();\n";
  out << "  if (lhs == INT64_MIN && rhs == -1) throw NativeBailout();\n";
  out << "  std::int64_t quotient = lhs / rhs;\n";
  out << "  const std::int64_t remainder = lhs % rhs;\n";
  out << "  if (remainder != 0 && ((remainder < 0) != (rhs < 0))) "
         "--quotient;\n";
  out << "  return quotient;\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "floor_mod_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  if (rhs == -1) return 0;\n";
  out << "  return lhs - floor_div_int64(lhs, rhs) * rhs;\n";
  out << "}\n\n";
  // Fixed-width Int arithmetic under amber.numeric-profile.v1. Checked
  // overflows bail to the VM so it raises the language-level OverflowError;
  // wrapping and saturating profiles resolve directly in generated code.
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "native_numeric_wrap_to_width("
         "std::int64_t value) {\n";
  out << "  if (kNativeNumericPolicy.bits >= 64U) return value;\n";
  out << "  const std::uint64_t mask = "
         "(std::uint64_t{1} << kNativeNumericPolicy.bits) - 1U;\n";
  out << "  std::uint64_t wrapped = static_cast<std::uint64_t>(value) & "
         "mask;\n";
  out << "  if (kNativeNumericPolicy.min < 0) {\n";
  out << "    const std::uint64_t sign_bit = std::uint64_t{1} << "
         "(kNativeNumericPolicy.bits - 1U);\n";
  out << "    if ((wrapped & sign_bit) != 0U) wrapped |= ~mask;\n";
  out << "  }\n";
  out << "  return static_cast<std::int64_t>(wrapped);\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE bool "
         "native_numeric_resolve(std::int64_t exact, "
         "bool overflowed64, bool positive_overflow, std::int64_t *out) {\n";
  out << "  if (!overflowed64 && exact >= kNativeNumericPolicy.min && "
         "exact <= kNativeNumericPolicy.max) { *out = exact; return true; "
         "}\n";
  out << "  switch (kNativeNumericPolicy.mode) {\n";
  out << "  case NativeNumericOverflowMode::Wrapping:\n";
  out << "    *out = native_numeric_wrap_to_width(exact);\n";
  out << "    return true;\n";
  out << "  case NativeNumericOverflowMode::Saturating:\n";
  out << "    *out = overflowed64 ? "
         "(positive_overflow ? kNativeNumericPolicy.max : "
         "kNativeNumericPolicy.min) : "
         "(exact > kNativeNumericPolicy.max ? kNativeNumericPolicy.max : "
         "kNativeNumericPolicy.min);\n";
  out << "    return true;\n";
  out << "  case NativeNumericOverflowMode::Checked:\n";
  out << "  default:\n";
  out << "    return false;\n";
  out << "  }\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "profile_add_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  std::int64_t result = 0;\n";
  out << "  const bool overflowed = __builtin_add_overflow(lhs, rhs, "
         "&result);\n";
  out << "  if (!native_numeric_resolve(result, overflowed, lhs > 0, "
         "&result)) throw NativeBailout();\n";
  out << "  return result;\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "profile_sub_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  std::int64_t result = 0;\n";
  out << "  const bool overflowed = __builtin_sub_overflow(lhs, rhs, "
         "&result);\n";
  out << "  if (!native_numeric_resolve(result, overflowed, lhs >= 0, "
         "&result)) throw NativeBailout();\n";
  out << "  return result;\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "profile_mul_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  std::int64_t result = 0;\n";
  out << "  const bool overflowed = __builtin_mul_overflow(lhs, rhs, "
         "&result);\n";
  out << "  if (!native_numeric_resolve(result, overflowed, "
         "(lhs < 0) == (rhs < 0), &result)) throw NativeBailout();\n";
  out << "  return result;\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "profile_neg_int64(std::int64_t value) {\n";
  out << "  std::int64_t result = 0;\n";
  out << "  if (value == std::numeric_limits<std::int64_t>::min()) {\n";
  out << "    if (!native_numeric_resolve(value, true, true, &result)) "
         "throw NativeBailout();\n";
  out << "    return result;\n";
  out << "  }\n";
  out << "  if (!native_numeric_resolve(-value, false, value < 0, "
         "&result)) throw NativeBailout();\n";
  out << "  return result;\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "profile_div_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  if (rhs == 0) throw NativeBailout();\n";
  out << "  std::int64_t result = 0;\n";
  out << "  if (lhs == std::numeric_limits<std::int64_t>::min() && "
         "rhs == -1) {\n";
  out << "    if (!native_numeric_resolve(lhs, true, true, &result)) "
         "throw NativeBailout();\n";
  out << "    return result;\n";
  out << "  }\n";
  out << "  if (!native_numeric_resolve(lhs / rhs, false, true, &result)) "
         "throw NativeBailout();\n";
  out << "  return result;\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "profile_floor_div_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  if (rhs == 0) throw NativeBailout();\n";
  out << "  std::int64_t result = 0;\n";
  out << "  if (lhs == std::numeric_limits<std::int64_t>::min() && "
         "rhs == -1) {\n";
  out << "    if (!native_numeric_resolve(lhs, true, true, &result)) "
         "throw NativeBailout();\n";
  out << "    return result;\n";
  out << "  }\n";
  out << "  if (!native_numeric_resolve(floor_div_int64(lhs, rhs), false, "
         "true, &result)) throw NativeBailout();\n";
  out << "  return result;\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "profile_shl_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  const std::int64_t shifted = static_cast<std::int64_t>("
         "static_cast<std::uint64_t>(lhs) << rhs);\n";
  out << "  const bool overflowed = (shifted >> rhs) != lhs;\n";
  out << "  std::int64_t result = 0;\n";
  out << "  if (!native_numeric_resolve(shifted, overflowed, lhs >= 0, "
         "&result)) throw NativeBailout();\n";
  out << "  return result;\n";
  out << "}\n\n";
  out << "static AMBER_NATIVE_ALWAYS_INLINE std::int64_t "
         "profile_pow_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  if (rhs < 0) throw NativeBailout();\n";
  out << "  std::int64_t result = 1;\n";
  out << "  std::int64_t base = lhs;\n";
  out << "  std::uint64_t exponent = static_cast<std::uint64_t>(rhs);\n";
  out << "  bool overflowed = false;\n";
  out << "  while (exponent > 0) {\n";
  out << "    if ((exponent & 1U) != 0U && "
         "__builtin_mul_overflow(result, base, &result)) overflowed = "
         "true;\n";
  out << "    exponent >>= 1U;\n";
  out << "    if (exponent > 0 && __builtin_mul_overflow(base, base, "
         "&base)) overflowed = true;\n";
  out << "  }\n";
  out << "  const bool positive_overflow = "
         "!(lhs < 0 && (static_cast<std::uint64_t>(rhs) & 1U) != 0U);\n";
  out << "  if (!native_numeric_resolve(result, overflowed, "
         "positive_overflow, &result)) throw NativeBailout();\n";
  out << "  return result;\n";
  out << "}\n\n";
  out << "static std::int64_t bit_xor_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  return static_cast<std::int64_t>("
         "static_cast<std::uint64_t>(lhs) ^ "
         "static_cast<std::uint64_t>(rhs));\n";
  out << "}\n\n";
  out << "static std::int64_t bit_and_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  return static_cast<std::int64_t>("
         "static_cast<std::uint64_t>(lhs) & "
         "static_cast<std::uint64_t>(rhs));\n";
  out << "}\n\n";
  out << "static std::int64_t bit_or_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  return static_cast<std::int64_t>("
         "static_cast<std::uint64_t>(lhs) | "
         "static_cast<std::uint64_t>(rhs));\n";
  out << "}\n\n";
  out << "static std::int64_t shl_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  return static_cast<std::int64_t>("
         "static_cast<std::uint64_t>(lhs) << rhs);\n";
  out << "}\n\n";
  out << "static std::int64_t shr_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  return static_cast<std::int64_t>("
         "static_cast<std::uint64_t>(lhs) >> rhs);\n";
  out << "}\n\n";
  // Polymorphic numeric helpers mirroring try_apply_scalar_send: Int/Int
  // stays exact (checked per the default numeric profile), any Int/Float mix
  // promotes to double, anything non-numeric bails to the VM. Float division
  // and modulo by zero bail so the VM raises the language-level
  // ZeroDivisionError.
  out << "static bool numeric_tag(const NativeValue &value) {\n";
  out << "  return value.tag == NativeValue::Tag::Integer || "
         "value.tag == NativeValue::Tag::Float;\n";
  out << "}\n\n";
  out << "static double as_double_numeric(const NativeValue &value) {\n";
  out << "  return value.tag == NativeValue::Tag::Integer ? "
         "static_cast<double>(value.scalar_value) : value.float_value;\n";
  out << "}\n\n";
  out << "static NativeValue native_unary_plus(const NativeValue &value) "
         "{\n";
  out << "  if (numeric_tag(value)) return value;\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue native_unary_minus(const NativeValue &value) "
         "{\n";
  out << "  if (value.tag == NativeValue::Tag::Integer) return "
         "NativeValue::integer(profile_neg_int64(value.scalar_value));\n";
  out << "  if (value.tag == NativeValue::Tag::Float) return "
         "NativeValue::floating(-value.float_value);\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue native_abs(const NativeValue &value) {\n";
  out << "  if (value.tag == NativeValue::Tag::Integer) {\n";
  out << "    return value.scalar_value < 0 ? "
         "NativeValue::integer(profile_neg_int64(value.scalar_value)) : "
         "value;\n";
  out << "  }\n";
  out << "  if (value.tag == NativeValue::Tag::Float) return "
         "NativeValue::floating(std::fabs(value.float_value));\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static std::int64_t compare_double_native(double lhs, double rhs) "
         "{\n";
  out << "  if (lhs < rhs) return -1;\n";
  out << "  if (lhs > rhs) return 1;\n";
  out << "  return 0;\n";
  out << "}\n\n";
  out << "static double floor_mod_double_native(double lhs, double rhs) {\n";
  out << "  return lhs - std::floor(lhs / rhs) * rhs;\n";
  out << "}\n\n";
  out << "static const std::string &native_string_text(const NativeValue "
         "&value) {\n";
  out << "  if (value.tag == NativeValue::Tag::HeapString) {\n";
  out << "    return static_cast<const NativeHeapString *>("
         "value.heap_value)->text;\n";
  out << "  }\n";
  out << "  return native_strings()[static_cast<std::size_t>("
         "value.scalar_value)];\n";
  out << "}\n\n";
  out << "static bool native_string_text_equal(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::String && "
         "rhs.tag == NativeValue::Tag::String) {\n";
  out << "    return lhs.scalar_value == rhs.scalar_value;\n";
  out << "  }\n";
  out << "  const std::string &lhs_text = native_string_text(lhs);\n";
  out << "  const std::string &rhs_text = native_string_text(rhs);\n";
  out << "  return lhs_text.size() == rhs_text.size() && "
         "lhs_text == rhs_text;\n";
  out << "}\n\n";
  out << "static NativeValue native_string_concat(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (!native_value_is_string(lhs) || "
         "!native_value_is_string(rhs)) throw NativeBailout();\n";
  out << "  return NativeValue::heap_string("
         "native_string_text(lhs) + native_string_text(rhs));\n";
  out << "}\n\n";
  out << "static std::int64_t native_string_codepoint_count("
         "const std::string &text) {\n";
  out << "  std::int64_t count = 0;\n";
  out << "  for (const char c : text) {\n";
  out << "    if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ++count;\n";
  out << "  }\n";
  out << "  return count;\n";
  out << "}\n\n";
  out << "static NativeValue native_string_length(const NativeValue &value) "
         "{\n";
  out << "  if (!native_value_is_string(value)) throw "
         "NativeBailout();\n";
  out << "  return NativeValue::integer(native_string_codepoint_count("
         "native_string_text(value)));\n";
  out << "}\n\n";
  out << "static NativeValue native_length(const NativeValue &value) {\n";
  out << "  if (native_value_is_string(value)) return "
         "native_string_length(value);\n";
  out << "  return native_count(value);\n";
  out << "}\n\n";
  out << "static NativeValue native_empty(const NativeValue &value) {\n";
  out << "  if (native_value_is_string(value)) return "
         "NativeValue::boolean(native_string_text(value).empty());\n";
  out << "  if (value.tag == NativeValue::Tag::List) return "
         "NativeValue::boolean(as_list(value).items.empty());\n";
  out << "  if (value.tag == NativeValue::Tag::Tuple) return "
         "NativeValue::boolean(as_tuple(value).items.empty());\n";
  out << "  if (value.tag == NativeValue::Tag::Set) return "
         "NativeValue::boolean(as_set(value).items.empty());\n";
  out << "  if (value.tag == NativeValue::Tag::Range) return "
         "NativeValue::boolean(native_range_items(value).empty());\n";
  out << "  if (value.tag == NativeValue::Tag::Map) return "
         "NativeValue::boolean(as_map(value).entries.empty());\n";
  out << "  if (value.tag == NativeValue::Tag::Bytes) return "
         "NativeValue::boolean(as_bytes(value).bytes.empty());\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue native_bytesize(const NativeValue &value) {\n";
  out << "  if (native_value_is_string(value)) return "
         "NativeValue::integer(static_cast<std::int64_t>("
         "native_string_text(value).size()));\n";
  out << "  if (value.tag == NativeValue::Tag::Bytes) return "
         "NativeValue::integer(static_cast<std::int64_t>("
         "as_bytes(value).bytes.size()));\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue native_string_contains(const NativeValue &value, "
         "const NativeValue &needle) {\n";
  out << "  if (!native_value_is_string(value) || "
         "!native_value_is_string(needle)) throw NativeBailout();\n";
  out << "  return NativeValue::boolean(native_string_text(value).find("
         "native_string_text(needle)) != std::string::npos);\n";
  out << "}\n\n";
  out << "static std::size_t native_utf8_next_cp(const std::string &text, "
         "std::size_t i) {\n";
  out << "  std::size_t j = i + 1U;\n";
  out << "  while (j < text.size() && "
         "(static_cast<unsigned char>(text[j]) & 0xC0U) == 0x80U) ++j;\n";
  out << "  return j;\n";
  out << "}\n\n";
  out << "static NativeValue native_string_reverse(const NativeValue &value) "
         "{\n";
  out << "  if (!native_value_is_string(value)) throw NativeBailout();\n";
  out << "  const std::string &text = native_string_text(value);\n";
  out << "  std::vector<std::string> cps;\n";
  out << "  for (std::size_t i = 0; i < text.size();) {\n";
  out << "    const std::size_t j = native_utf8_next_cp(text, i);\n";
  out << "    cps.push_back(text.substr(i, j - i));\n";
  out << "    i = j;\n";
  out << "  }\n";
  out << "  std::string out_text;\n";
  out << "  out_text.reserve(text.size());\n";
  out << "  for (auto it = cps.rbegin(); it != cps.rend(); ++it) "
         "out_text += *it;\n";
  out << "  return NativeValue::heap_string(out_text);\n";
  out << "}\n\n";
  out << "static NativeValue native_string_chars(const NativeValue &value) {\n";
  out << "  if (!native_value_is_string(value)) throw NativeBailout();\n";
  out << "  const std::string &text = native_string_text(value);\n";
  out << "  std::vector<NativeValue> cps;\n";
  out << "  for (std::size_t i = 0; i < text.size();) {\n";
  out << "    const std::size_t j = native_utf8_next_cp(text, i);\n";
  out << "    cps.push_back(NativeValue::heap_string("
         "text.substr(i, j - i)));\n";
  out << "    i = j;\n";
  out << "  }\n";
  out << "  return NativeValue::list(std::move(cps));\n";
  out << "}\n\n";
  out << "static NativeValue native_string_case(const NativeValue &value, "
         "bool up) {\n";
  out << "  if (!native_value_is_string(value)) throw "
         "NativeBailout();\n";
  out << "  std::string text = native_string_text(value);\n";
  out << "  for (char &c : text) {\n";
  out << "    const unsigned char uc = static_cast<unsigned char>(c);\n";
  out << "    if (up && uc >= 'a' && uc <= 'z') c = "
         "static_cast<char>(uc - 32U);\n";
  out << "    else if (!up && uc >= 'A' && uc <= 'Z') c = "
         "static_cast<char>(uc + 32U);\n";
  out << "  }\n";
  out << "  return NativeValue::heap_string(text);\n";
  out << "}\n\n";
  out << "static bool native_ascii_space(unsigned char c) {\n";
  out << "  return c == ' ' || c == '\\t' || c == '\\n' || c == '\\r' || "
         "c == '\\f' || c == '\\v';\n";
  out << "}\n\n";
  out << "static NativeValue native_string_trim(const NativeValue &value) "
         "{\n";
  out << "  if (!native_value_is_string(value)) throw "
         "NativeBailout();\n";
  out << "  const std::string &text = native_string_text(value);\n";
  out << "  std::size_t first = 0;\n";
  out << "  std::size_t last = text.size();\n";
  out << "  while (first < last && native_ascii_space("
         "static_cast<unsigned char>(text[first]))) ++first;\n";
  out << "  while (last > first && native_ascii_space("
         "static_cast<unsigned char>(text[last - 1U]))) --last;\n";
  out << "  return NativeValue::heap_string("
         "text.substr(first, last - first));\n";
  out << "}\n\n";
  out << "static NativeValue native_string_starts_with(const NativeValue "
         "&value, const NativeValue &prefix) {\n";
  out << "  if (!native_value_is_string(value) || "
         "!native_value_is_string(prefix)) throw NativeBailout();\n";
  out << "  const std::string &text = native_string_text(value);\n";
  out << "  const std::string &needle = native_string_text(prefix);\n";
  out << "  return NativeValue::boolean(text.size() >= needle.size() && "
         "text.compare(0, needle.size(), needle) == 0);\n";
  out << "}\n\n";
  out << "static NativeValue native_string_ends_with(const NativeValue "
         "&value, const NativeValue &suffix) {\n";
  out << "  if (!native_value_is_string(value) || "
         "!native_value_is_string(suffix)) throw NativeBailout();\n";
  out << "  const std::string &text = native_string_text(value);\n";
  out << "  const std::string &needle = native_string_text(suffix);\n";
  out << "  return NativeValue::boolean(text.size() >= needle.size() && "
         "text.compare(text.size() - needle.size(), needle.size(), needle) "
         "== 0);\n";
  out << "}\n\n";
  out << "static NativeValue native_string_split(const NativeValue &value, "
         "const NativeValue &separator) {\n";
  out << "  if (!native_value_is_string(value) || "
         "!native_value_is_string(separator)) throw NativeBailout();\n";
  out << "  const std::string &text = native_string_text(value);\n";
  out << "  const std::string &sep = native_string_text(separator);\n";
  out << "  std::vector<NativeValue> parts;\n";
  out << "  if (sep.empty()) {\n";
  out << "    for (std::size_t i = 0; i < text.size();) {\n";
  out << "      const std::size_t j = native_utf8_next_cp(text, i);\n";
  out << "      parts.push_back(NativeValue::heap_string("
         "text.substr(i, j - i)));\n";
  out << "      i = j;\n";
  out << "    }\n";
  out << "  } else {\n";
  out << "    std::size_t start = 0;\n";
  out << "    while (true) {\n";
  out << "      const std::size_t pos = text.find(sep, start);\n";
  out << "      if (pos == std::string::npos) {\n";
  out << "        parts.push_back(NativeValue::string_ref("
         "native_intern_string(text.substr(start))));\n";
  out << "        break;\n";
  out << "      }\n";
  out << "      parts.push_back(NativeValue::heap_string("
         "text.substr(start, pos - start)));\n";
  out << "      start = pos + sep.size();\n";
  out << "    }\n";
  out << "  }\n";
  out << "  return NativeValue::list(std::move(parts));\n";
  out << "}\n\n";
  out << R"AMBERCPP(static NativeValue amber_native_call_closure(
    const NativeValue &value, std::initializer_list<NativeValue> args);

static std::string native_regexp_pattern_to_string(
    const NativeRegexp &pattern) {
  return "/" + pattern.source + "/";
}

static std::optional<std::string> native_regexp_group_text(
    const NativeRegexpMatch &match, std::size_t index) {
  if (index >= match.captures.size()) return std::nullopt;
  const NativeRegexpCapture &capture = match.captures[index];
  if (!capture.matched || capture.start > capture.end ||
      capture.end > match.source.size()) {
    return std::nullopt;
  }
  return match.source.substr(capture.start, capture.end - capture.start);
}

static std::string native_regexp_match_to_string(
    const NativeRegexpMatch &match) {
  return native_regexp_group_text(match, 0U).value_or(std::string{});
}

static std::string native_regexp_text_arg(const NativeValue &value) {
  if (native_value_is_string(value)) return native_string_text(value);
  if (value.tag == NativeValue::Tag::Symbol) {
    return native_symbol_text(value.scalar_value);
  }
  throw NativeBailout();
}

static std::regex::flag_type native_regexp_flags_from_value(
    const NativeValue &value) {
  const std::string text = native_regexp_text_arg(value);
  std::regex::flag_type flags = std::regex::ECMAScript;
  for (char c : text) {
    if (c == 'i') {
      flags |= std::regex::icase;
      continue;
    }
    throw NativeBailout();
  }
  return flags;
}

static const NativeValue *native_regexp_kw(
    std::initializer_list<std::pair<std::string, NativeValue>> kwargs,
    const std::string &name) {
  for (const auto &entry : kwargs) {
    if (entry.first == name) return &entry.second;
  }
  return nullptr;
}

static void native_regexp_reject_unknown_kw(
    std::initializer_list<std::pair<std::string, NativeValue>> kwargs,
    std::initializer_list<const char *> allowed) {
  for (const auto &entry : kwargs) {
    bool matched = false;
    for (const char *name : allowed) {
      if (entry.first == name) {
        matched = true;
        break;
      }
    }
    if (!matched) throw NativeBailout();
  }
}

static NativeValue native_regexp_compile_value(
    const NativeValue &source_value,
    std::initializer_list<std::pair<std::string, NativeValue>> kwargs) {
  native_regexp_reject_unknown_kw(kwargs, {"flags"});
  const std::string source = native_regexp_text_arg(source_value);
  std::regex::flag_type flags = std::regex::ECMAScript;
  if (const NativeValue *flag_value = native_regexp_kw(kwargs, "flags")) {
    flags = native_regexp_flags_from_value(*flag_value);
  }
  NativeRegexp pattern;
  pattern.source = source;
  pattern.flags = flags;
  try {
    pattern.compiled = std::regex(pattern.source, pattern.flags);
  } catch (const std::regex_error &) {
    throw NativeBailout();
  }
  return NativeValue::regexp(std::move(pattern));
}

static NativeRegexpMatch native_regexp_make_match(
    const NativeValue &pattern_value, const std::string &source,
    const std::smatch &match, std::size_t base_offset) {
  NativeRegexpMatch value;
  value.source = source;
  value.pattern = pattern_value;
  value.captures.reserve(match.size());
  for (std::size_t i = 0; i < match.size(); ++i) {
    NativeRegexpCapture capture;
    capture.matched = match[i].matched;
    if (capture.matched) {
      capture.start =
          base_offset + static_cast<std::size_t>(match.position(i));
      capture.end = capture.start + static_cast<std::size_t>(match.length(i));
    }
    value.captures.push_back(capture);
  }
  return value;
}

static NativeValue native_regexp_search_value(const NativeValue &pattern_value,
                                              const NativeValue &source_value,
                                              bool full_match,
                                              bool bool_only,
                                              bool negate) {
  const NativeRegexp &pattern = as_regexp(pattern_value);
  const std::string source = native_regexp_text_arg(source_value);
  std::smatch match;
  bool matched = false;
  try {
    matched = full_match ? std::regex_match(source, match, pattern.compiled)
                         : std::regex_search(source, match, pattern.compiled);
  } catch (const std::regex_error &) {
    throw NativeBailout();
  }
  if (bool_only) return NativeValue::boolean(negate ? !matched : matched);
  if (!matched) return NativeValue::nullv();
  return NativeValue::regexp_match(
      native_regexp_make_match(pattern_value, source, match, 0U));
}

static NativeValue native_regexp_match_operator(const NativeValue &lhs,
                                                const NativeValue &rhs,
                                                bool negate) {
  if (lhs.tag == NativeValue::Tag::Regexp) {
    return native_regexp_search_value(lhs, rhs, false, negate, negate);
  }
  if (rhs.tag == NativeValue::Tag::Regexp) {
    return native_regexp_search_value(rhs, lhs, false, negate, negate);
  }
  throw NativeBailout();
}

static std::optional<std::size_t> native_regexp_parse_group_index(
    const std::string &text) {
  if (text.empty()) return std::nullopt;
  std::size_t value = 0;
  for (char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    const std::size_t digit = static_cast<std::size_t>(c - '0');
    if (value >
        (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
      return std::nullopt;
    }
    value = value * 10U + digit;
  }
  return value;
}

static std::string native_regexp_expand_replacement(
    const NativeRegexpMatch &match, const std::string &replacement) {
  std::string out;
  out.reserve(replacement.size());
  for (std::size_t i = 0; i < replacement.size();) {
    if (replacement[i] != '$') {
      out.push_back(replacement[i++]);
      continue;
    }
    if (i + 1U >= replacement.size()) {
      out.push_back('$');
      ++i;
      continue;
    }
    const char next = replacement[i + 1U];
    if (next == '$') {
      out.push_back('$');
      i += 2U;
      continue;
    }
    if (next == '&') {
      if (const auto text = native_regexp_group_text(match, 0U)) out += *text;
      i += 2U;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(next))) {
      std::size_t cursor = i + 1U;
      while (cursor < replacement.size() &&
             std::isdigit(static_cast<unsigned char>(replacement[cursor]))) {
        ++cursor;
      }
      const auto index = native_regexp_parse_group_index(
          replacement.substr(i + 1U, cursor - (i + 1U)));
      if (index.has_value()) {
        if (const auto text = native_regexp_group_text(match, *index)) {
          out += *text;
        }
      }
      i = cursor;
      continue;
    }
    if (next == '{') {
      const std::size_t close = replacement.find('}', i + 2U);
      if (close == std::string::npos) throw NativeBailout();
      const std::string name = replacement.substr(i + 2U, close - (i + 2U));
      const auto index = native_regexp_parse_group_index(name);
      if (!index.has_value()) throw NativeBailout();
      if (const auto text = native_regexp_group_text(match, *index)) {
        out += *text;
      }
      i = close + 1U;
      continue;
    }
    out.push_back('$');
    ++i;
  }
  return out;
}

static std::size_t native_regexp_replace_limit(
    std::initializer_list<std::pair<std::string, NativeValue>> kwargs) {
  native_regexp_reject_unknown_kw(kwargs, {"count"});
  const NativeValue *count_value = native_regexp_kw(kwargs, "count");
  if (count_value == nullptr) return std::numeric_limits<std::size_t>::max();
  if (count_value->tag == NativeValue::Tag::Integer) {
    if (count_value->scalar_value < 0) throw NativeBailout();
    return static_cast<std::size_t>(count_value->scalar_value);
  }
  if (native_regexp_text_arg(*count_value) == "all") {
    return std::numeric_limits<std::size_t>::max();
  }
  throw NativeBailout();
}

static NativeValue native_regexp_replace_string(
    const NativeValue &value, const NativeValue &pattern_value,
    const NativeValue &replacement_value, bool has_replacement,
    const NativeValue &block_value, bool has_block,
    std::initializer_list<std::pair<std::string, NativeValue>> kwargs) {
  if (!native_value_is_string(value)) throw NativeBailout();
  const NativeRegexp &pattern = as_regexp(pattern_value);
  const std::string source = native_string_text(value);
  const std::size_t limit = native_regexp_replace_limit(kwargs);
  if (limit == 0U) return value;
  std::optional<std::string> replacement;
  if (has_block) {
    if (has_replacement) throw NativeBailout();
  } else {
    if (!has_replacement) throw NativeBailout();
    replacement = native_regexp_text_arg(replacement_value);
  }

  std::string out;
  std::size_t last_append = 0;
  std::size_t search_offset = 0;
  std::size_t replaced = 0;
  while (replaced < limit && search_offset <= source.size()) {
    std::smatch match;
    const auto begin =
        source.cbegin() + static_cast<std::ptrdiff_t>(search_offset);
    try {
      if (!std::regex_search(begin, source.cend(), match, pattern.compiled)) {
        break;
      }
    } catch (const std::regex_error &) {
      throw NativeBailout();
    }
    const std::size_t match_start =
        search_offset + static_cast<std::size_t>(match.position(0));
    const std::size_t match_end =
        match_start + static_cast<std::size_t>(match.length(0));
    out.append(source, last_append, match_start - last_append);
    NativeValue match_value = NativeValue::regexp_match(
        native_regexp_make_match(pattern_value, source, match, search_offset));
    if (replacement.has_value()) {
      out += native_regexp_expand_replacement(as_regexp_match(match_value),
                                              *replacement);
    } else {
      out += native_regexp_text_arg(
          amber_native_call_closure(block_value, {match_value}));
    }
    ++replaced;
    last_append = match_end;
    if (match_start == match_end) {
      if (match_end >= source.size()) break;
      search_offset = match_end + 1U;
    } else {
      search_offset = match_end;
    }
  }
  out.append(source, last_append, std::string::npos);
  return NativeValue::heap_string(std::move(out));
}

static NativeValue native_string_replace(
    const NativeValue &value, const NativeValue &from_value,
    const NativeValue &to_value, bool has_to = true,
    const NativeValue &block_value = NativeValue::nullv(),
    bool has_block = false,
    std::initializer_list<std::pair<std::string, NativeValue>> kwargs = {}) {
  if (from_value.tag == NativeValue::Tag::Regexp) {
    return native_regexp_replace_string(value, from_value, to_value, has_to,
                                        block_value, has_block, kwargs);
  }
  if (!native_value_is_string(value) || !native_value_is_string(from_value) ||
      !native_value_is_string(to_value) || !has_to || has_block ||
      kwargs.size() != 0U) {
    throw NativeBailout();
  }
  const std::string &text = native_string_text(value);
  const std::string &from = native_string_text(from_value);
  const std::string &to = native_string_text(to_value);
  if (from.empty()) return value;
  std::string replaced;
  std::size_t start = 0;
  while (true) {
    const std::size_t pos = text.find(from, start);
    if (pos == std::string::npos) {
      replaced += text.substr(start);
      break;
    }
    replaced += text.substr(start, pos - start);
    replaced += to;
    start = pos + from.size();
  }
  return NativeValue::heap_string(replaced);
}

static NativeValue native_regexp_group_value(const NativeRegexpMatch &match,
                                             const NativeValue &index_value) {
  if (index_value.tag != NativeValue::Tag::Integer ||
      index_value.scalar_value < 0) {
    throw NativeBailout();
  }
  const auto text = native_regexp_group_text(
      match, static_cast<std::size_t>(index_value.scalar_value));
  return text.has_value() ? NativeValue::heap_string(*text)
                          : NativeValue::nullv();
}

static NativeValue native_regexp_send(
    const NativeValue &receiver, std::string_view selector,
    std::initializer_list<NativeValue> args,
    std::initializer_list<std::pair<std::string, NativeValue>> kwargs) {
  const NativeValue *arg0 = args.size() >= 1U ? args.begin() : nullptr;
  if (selector == "compile" || selector == "new" || selector == "r" ||
      selector == "__call__") {
    if (receiver.tag != NativeValue::Tag::RegexpModule ||
        args.size() != 1U) {
      throw NativeBailout();
    }
    return native_regexp_compile_value(*arg0, kwargs);
  }
  if (selector == "escape") {
    if (receiver.tag != NativeValue::Tag::RegexpModule ||
        args.size() != 1U) {
      throw NativeBailout();
    }
    native_regexp_reject_unknown_kw(kwargs, {});
    const std::string source = native_regexp_text_arg(*arg0);
    std::string out;
    out.reserve(source.size() * 2U);
    for (char c : source) {
      switch (c) {
      case '\\':
      case '^':
      case '$':
      case '.':
      case '|':
      case '?':
      case '*':
      case '+':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
        out.push_back('\\');
        break;
      default:
        break;
      }
      out.push_back(c);
    }
    return NativeValue::heap_string(std::move(out));
  }
  native_regexp_reject_unknown_kw(kwargs, {});
  if (receiver.tag == NativeValue::Tag::Regexp) {
    if ((selector == "source" || selector == "to_str" ||
         selector == "inspect") &&
        args.size() == 0U) {
      const NativeRegexp &pattern = as_regexp(receiver);
      return selector == "source"
                 ? NativeValue::heap_string(pattern.source)
                 : NativeValue::heap_string(
                       native_regexp_pattern_to_string(pattern));
    }
    if ((selector == "match" || selector == "find" ||
         selector == "match?" || selector == "matches?" ||
         selector == "full_match" || selector == "full_match?" ||
         selector == "=~" || selector == "!~") &&
        args.size() == 1U) {
      const bool full =
          selector == "full_match" || selector == "full_match?";
      const bool bool_only = selector == "match?" ||
                             selector == "matches?" ||
                             selector == "full_match?" ||
                             selector == "!~";
      return native_regexp_search_value(receiver, *arg0, full, bool_only,
                                        selector == "!~");
    }
    throw NativeBailout();
  }
  if (receiver.tag == NativeValue::Tag::RegexpMatch) {
    const NativeRegexpMatch &match = as_regexp_match(receiver);
    if ((selector == "text" || selector == "to_str" ||
         selector == "inspect") &&
        args.size() == 0U) {
      return NativeValue::heap_string(native_regexp_match_to_string(match));
    }
    if (selector == "pattern" && args.size() == 0U) {
      return match.pattern;
    }
    if (selector == "source" && args.size() == 0U) {
      return NativeValue::heap_string(match.source);
    }
    if ((selector == "count" || selector == "size" ||
         selector == "length") &&
        args.size() == 0U) {
      const std::size_t captures =
          match.captures.empty() ? 0U : match.captures.size() - 1U;
      return NativeValue::integer(static_cast<std::int64_t>(captures));
    }
    if ((selector == "[]" || selector == "group") && args.size() == 1U) {
      return native_regexp_group_value(match, *arg0);
    }
    if ((selector == "start" || selector == "finish" ||
         selector == "end") &&
        args.size() == 0U) {
      if (match.captures.empty() || !match.captures[0].matched) {
        return NativeValue::nullv();
      }
      return NativeValue::integer(static_cast<std::int64_t>(
          selector == "start" ? match.captures[0].start
                              : match.captures[0].end));
    }
    if (selector == "captures" && args.size() == 0U) {
      std::vector<NativeValue> captures;
      for (std::size_t i = 1; i < match.captures.size(); ++i) {
        const auto text = native_regexp_group_text(match, i);
        captures.push_back(text.has_value() ? NativeValue::heap_string(*text)
                                            : NativeValue::nullv());
      }
      return NativeValue::list(std::move(captures));
    }
  }
  throw NativeBailout();
}

)AMBERCPP";
  out << R"AMBERCPP(static bool native_is_sequence(const NativeValue &value) {
  return value.tag == NativeValue::Tag::List ||
         value.tag == NativeValue::Tag::Tuple ||
         value.tag == NativeValue::Tag::Set ||
         value.tag == NativeValue::Tag::Range;
}

static bool native_value_equal(const NativeValue &lhs, const NativeValue &rhs);

static bool native_sequence_equal(const std::vector<NativeValue> &lhs,
                                  const std::vector<NativeValue> &rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (!native_value_equal(lhs[i], rhs[i])) return false;
  }
  return true;
}

static bool native_unordered_sequence_equal(
    const std::vector<NativeValue> &lhs,
    const std::vector<NativeValue> &rhs) {
  if (lhs.size() != rhs.size()) return false;
  std::vector<bool> matched(rhs.size(), false);
  for (const NativeValue &left : lhs) {
    bool found = false;
    for (std::size_t i = 0; i < rhs.size(); ++i) {
      if (!matched[i] && native_value_equal(left, rhs[i])) {
        matched[i] = true;
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

static bool native_value_equal(const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return lhs.scalar_value == rhs.scalar_value;
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    return as_double_numeric(lhs) == as_double_numeric(rhs);
  }
  if (native_value_is_string(lhs) && native_value_is_string(rhs)) {
    return native_string_text_equal(lhs, rhs);
  }
  if (lhs.tag != rhs.tag) return false;
  switch (lhs.tag) {
  case NativeValue::Tag::Null:
    return true;
  case NativeValue::Tag::Bool:
  case NativeValue::Tag::String:
  case NativeValue::Tag::Symbol:
    return lhs.scalar_value == rhs.scalar_value;
  case NativeValue::Tag::Bytes:
    return as_bytes(lhs).bytes == as_bytes(rhs).bytes;
  case NativeValue::Tag::List:
    return native_sequence_equal(as_list(lhs).items, as_list(rhs).items);
  case NativeValue::Tag::Tuple:
    return native_sequence_equal(as_tuple(lhs).items, as_tuple(rhs).items);
  case NativeValue::Tag::Set:
    return native_unordered_sequence_equal(as_set(lhs).items,
                                           as_set(rhs).items);
  case NativeValue::Tag::Range: {
    const NativeRange &left = as_range(lhs);
    const NativeRange &right = as_range(rhs);
    return left.start == right.start && left.finish == right.finish &&
           left.step == right.step &&
           left.inclusive_end == right.inclusive_end;
  }
  case NativeValue::Tag::Regexp: {
    const NativeRegexp &left = as_regexp(lhs);
    const NativeRegexp &right = as_regexp(rhs);
    return left.source == right.source && left.flags == right.flags;
  }
  case NativeValue::Tag::RegexpMatch:
    return lhs.heap_value == rhs.heap_value;
  default:
    throw NativeBailout();
  }
}

static bool native_map_key_is_nameable(const NativeValue &value) {
  return native_value_is_string(value) ||
         value.tag == NativeValue::Tag::Symbol;
}

static const std::string &native_name_key_text(const NativeValue &value) {
  if (native_value_is_string(value)) return native_string_text(value);
  if (value.tag == NativeValue::Tag::Symbol) {
    return native_symbol_text(value.scalar_value);
  }
  throw NativeBailout();
}

static NativeValue native_normalize_map_key(const NativeValue &key) {
  if (key.tag == NativeValue::Tag::Float && std::isnan(key.float_value)) {
    throw NativeBailout();
  }
  if (key.tag == NativeValue::Tag::Map ||
      key.tag == NativeValue::Tag::Set ||
      key.tag == NativeValue::Tag::Closure) {
    throw NativeBailout();
  }
  if (key.tag == NativeValue::Tag::List) {
    std::vector<NativeValue> normalized;
    normalized.reserve(as_list(key).items.size());
    for (const NativeValue &item : as_list(key).items) {
      normalized.push_back(native_normalize_map_key(item));
    }
    return NativeValue::tuple(std::move(normalized));
  }
  if (key.tag == NativeValue::Tag::Tuple) {
    std::vector<NativeValue> normalized;
    normalized.reserve(as_tuple(key).items.size());
    for (const NativeValue &item : as_tuple(key).items) {
      normalized.push_back(native_normalize_map_key(item));
    }
    return NativeValue::tuple(std::move(normalized));
  }
  return key;
}

static bool native_map_keys_equivalent(const NativeValue &stored,
                                       const NativeValue &lookup,
                                       bool strict) {
  if (!strict && native_map_key_is_nameable(stored) &&
      native_map_key_is_nameable(lookup)) {
    return native_name_key_text(stored) == native_name_key_text(lookup);
  }
  return native_value_equal(stored, lookup);
}

static const std::pair<NativeValue, NativeValue> *
native_map_find_entry(const NativeValue &map_value, const NativeValue &key) {
  const NativeMap &map = as_map(map_value);
  const NativeValue normalized = native_normalize_map_key(key);
  const bool nameable = !map.strict && native_map_key_is_nameable(normalized);
  const std::optional<std::int64_t> key_id =
      native_map_index_key_id_for_read(normalized, map.strict);
  if (nameable && !key_id.has_value()) return nullptr;
  if (key_id.has_value()) {
    if (!map.name_index.empty()) {
      const auto found = map.name_index.find(*key_id);
      if (found != map.name_index.end() && found->second < map.entries.size()) {
        return &map.entries[found->second];
      }
      if (native_map_key_is_nameable(normalized)) return nullptr;
    }
    const std::optional<std::size_t> inline_index =
        native_map_find_inline_name_index(map, *key_id);
    if (inline_index.has_value()) return &map.entries[*inline_index];
    if (map.entries.size() <= kNativeMapInlineNameIndexCapacity &&
        native_map_key_is_nameable(normalized)) {
      return nullptr;
    }
  }
  for (const auto &entry : map.entries) {
    if (native_map_keys_equivalent(entry.first, normalized, map.strict)) {
      return &entry;
    }
  }
  return nullptr;
}

static NativeValue native_map_lookup(const NativeValue &map_value,
                                     const NativeValue &key,
                                     bool optional) {
  const auto *entry = native_map_find_entry(map_value, key);
  if (entry == nullptr) {
    if (optional) return NativeValue::nullv();
    throw NativeBailout();
  }
  return entry->second;
}

static NativeValue native_map_contains_key(const NativeValue &map_value,
                                           const NativeValue &key) {
  return NativeValue::boolean(native_map_find_entry(map_value, key) != nullptr);
}

static void native_map_store(
    std::vector<std::pair<NativeValue, NativeValue>> &entries,
    NativeValue key, NativeValue value, bool strict) {
  NativeValue normalized = native_normalize_map_key(key);
  for (auto &entry : entries) {
    if (native_map_keys_equivalent(entry.first, normalized, strict)) {
      entry.second = std::move(value);
      return;
    }
  }
  entries.emplace_back(std::move(normalized), std::move(value));
}

static void native_map_store(NativeMap &map, NativeValue key,
                             NativeValue value) {
  NativeValue normalized = native_normalize_map_key(key);
  const std::optional<std::int64_t> key_id =
      native_map_index_key_id(normalized, map.strict);
  if (key_id.has_value() && !map.name_index.empty()) {
    const auto found = map.name_index.find(*key_id);
    if (found != map.name_index.end() && found->second < map.entries.size()) {
      map.entries[found->second].second = std::move(value);
      return;
    }
    if (native_map_key_is_nameable(normalized)) {
      const std::size_t index = map.entries.size();
      map.entries.emplace_back(std::move(normalized), std::move(value));
      map.name_index[*key_id] = index;
      return;
    }
  }
  if (key_id.has_value()) {
    const std::optional<std::size_t> inline_index =
        native_map_find_inline_name_index(map, *key_id);
    if (inline_index.has_value()) {
      map.entries[*inline_index].second = std::move(value);
      return;
    }
  }
  for (std::size_t i = 0; i < map.entries.size(); ++i) {
    if (native_map_keys_equivalent(map.entries[i].first, normalized,
                                   map.strict)) {
      map.entries[i].second = std::move(value);
      if (key_id.has_value() && !map.name_index.empty()) {
        map.name_index[*key_id] = i;
      }
      return;
    }
  }
  const std::size_t index = map.entries.size();
  map.entries.emplace_back(std::move(normalized), std::move(value));
  if (key_id.has_value()) {
    if (!map.name_index.empty()) {
      map.name_index[*key_id] = index;
    } else if (map.entries.size() > kNativeMapInlineNameIndexCapacity) {
      native_map_rebuild_index(map);
    } else {
      native_map_add_inline_name_index(map, *key_id, index);
    }
  }
}

static void native_map_append_entries(
    std::vector<std::pair<NativeValue, NativeValue>> &entries,
    const NativeValue &source, bool strict) {
  const NativeMap &map = as_map(source);
  for (const auto &entry : map.entries) {
    native_map_store(entries, entry.first, entry.second, strict);
  }
}

static NativeValue native_map_keys(const NativeValue &map_value) {
  const auto &entries = as_map(map_value).entries;
  std::vector<NativeValue> keys;
  keys.reserve(entries.size());
  for (const auto &entry : entries) keys.push_back(entry.first);
  return NativeValue::list(std::move(keys));
}

static NativeValue native_map_values(const NativeValue &map_value) {
  const auto &entries = as_map(map_value).entries;
  std::vector<NativeValue> values;
  values.reserve(entries.size());
  for (const auto &entry : entries) values.push_back(entry.second);
  return NativeValue::list(std::move(values));
}

static NativeValue native_map_has_value(const NativeValue &map_value,
                                        const NativeValue &needle) {
  const auto &entries = as_map(map_value).entries;
  for (const auto &entry : entries) {
    if (native_value_equal(entry.second, needle)) {
      return NativeValue::boolean(true);
    }
  }
  return NativeValue::boolean(false);
}

static NativeValue native_map_entries_array(const NativeValue &map_value) {
  const auto &entries = as_map(map_value).entries;
  std::vector<NativeValue> result;
  result.reserve(entries.size());
  for (const auto &entry : entries) {
    result.push_back(NativeValue::tuple({entry.first, entry.second}));
  }
  return NativeValue::list(std::move(result));
}

static NativeValue native_map_slice(const NativeValue &map_value,
                                    const NativeValue &first_key,
                                    const NativeValue &second_key,
                                    bool has_second_key) {
  const NativeMap &map = as_map(map_value);
  std::vector<NativeValue> wanted;
  wanted.push_back(native_normalize_map_key(first_key));
  if (has_second_key) wanted.push_back(native_normalize_map_key(second_key));
  std::vector<std::pair<NativeValue, NativeValue>> result;
  for (const auto &entry : map.entries) {
    for (const NativeValue &key : wanted) {
      if (native_map_keys_equivalent(entry.first, key, map.strict)) {
        result.push_back(entry);
        break;
      }
    }
  }
  return NativeValue::map_entries(std::move(result), map.strict);
}

static NativeValue native_map_with(const NativeValue &map_value,
                                   const NativeValue &key,
                                   const NativeValue &value) {
  const NativeMap &map = as_map(map_value);
  std::vector<std::pair<NativeValue, NativeValue>> result = map.entries;
  native_map_store(result, key, value, map.strict);
  return NativeValue::map_entries(std::move(result), map.strict);
}

static NativeValue native_map_except(const NativeValue &map_value,
                                     std::initializer_list<NativeValue> keys) {
  const NativeMap &map = as_map(map_value);
  std::vector<NativeValue> wanted;
  wanted.reserve(keys.size());
  for (const NativeValue &key : keys) {
    wanted.push_back(native_normalize_map_key(key));
  }
  std::vector<std::pair<NativeValue, NativeValue>> result;
  result.reserve(map.entries.size());
  for (const auto &entry : map.entries) {
    bool drop = false;
    for (const NativeValue &key : wanted) {
      if (native_map_keys_equivalent(entry.first, key, map.strict)) {
        drop = true;
        break;
      }
    }
    if (!drop) result.push_back(entry);
  }
  return NativeValue::map_entries(std::move(result), map.strict);
}

static NativeValue native_map_compact(const NativeValue &map_value) {
  const NativeMap &map = as_map(map_value);
  std::vector<std::pair<NativeValue, NativeValue>> result;
  result.reserve(map.entries.size());
  for (const auto &entry : map.entries) {
    if (entry.second.tag != NativeValue::Tag::Null) result.push_back(entry);
  }
  return NativeValue::map_entries(std::move(result), map.strict);
}

static NativeValue native_map_merge(const NativeValue &lhs,
                                    const NativeValue &rhs) {
  std::vector<std::pair<NativeValue, NativeValue>> result = as_map(lhs).entries;
  native_map_append_entries(result, rhs, false);
  return NativeValue::map_entries(std::move(result), false);
}

static bool native_sequence_contains_value(
    const std::vector<NativeValue> &items, const NativeValue &needle) {
  for (const NativeValue &item : items) {
    if (native_value_equal(item, needle)) return true;
  }
  return false;
}

static void native_append_unique_value(std::vector<NativeValue> &items,
                                       const NativeValue &value) {
  if (!native_sequence_contains_value(items, value)) {
    items.push_back(value);
  }
}

static std::vector<NativeValue>
native_unique_sequence_items(const std::vector<NativeValue> &items) {
  std::vector<NativeValue> unique;
  unique.reserve(items.size());
  for (const NativeValue &item : items) {
    native_append_unique_value(unique, item);
  }
  return unique;
}

static NativeValue native_normalize_set_element(const NativeValue &value) {
  return native_normalize_map_key(value);
}

static NativeValue native_set_from_items(std::vector<NativeValue> items) {
  std::vector<NativeValue> unique;
  unique.reserve(items.size());
  for (const NativeValue &item : items) {
    native_append_unique_value(unique, native_normalize_set_element(item));
  }
  return NativeValue::set(std::move(unique));
}

static void native_set_spread_append(std::vector<NativeValue> &items,
                                     const NativeValue &value) {
  const std::vector<NativeValue> expanded = native_sequence_items_copy(value);
  items.insert(items.end(), expanded.begin(), expanded.end());
}

static NativeValue native_materialize_set_like_result(
    const NativeValue &receiver, std::vector<NativeValue> items) {
  if (receiver.tag == NativeValue::Tag::Set) {
    return native_set_from_items(std::move(items));
  }
  return NativeValue::list(std::move(items));
}

static std::vector<NativeValue> native_sequence_union_items(
    const std::vector<NativeValue> &left,
    const std::vector<NativeValue> &right) {
  std::vector<NativeValue> result;
  result.reserve(left.size() + right.size());
  for (const NativeValue &item : left) native_append_unique_value(result, item);
  for (const NativeValue &item : right) native_append_unique_value(result, item);
  return result;
}

static std::vector<NativeValue> native_sequence_intersection_items(
    const std::vector<NativeValue> &left,
    const std::vector<NativeValue> &right) {
  std::vector<NativeValue> result;
  for (const NativeValue &item : left) {
    if (native_sequence_contains_value(right, item)) {
      native_append_unique_value(result, item);
    }
  }
  return result;
}

static std::vector<NativeValue> native_sequence_difference_items(
    const std::vector<NativeValue> &left,
    const std::vector<NativeValue> &right) {
  std::vector<NativeValue> result;
  for (const NativeValue &item : left) {
    if (!native_sequence_contains_value(right, item)) {
      native_append_unique_value(result, item);
    }
  }
  return result;
}

static std::vector<NativeValue> native_sequence_symmetric_difference_items(
    const std::vector<NativeValue> &left,
    const std::vector<NativeValue> &right) {
  std::vector<NativeValue> result =
      native_sequence_difference_items(left, right);
  const std::vector<NativeValue> right_only =
      native_sequence_difference_items(right, left);
  for (const NativeValue &item : right_only) {
    native_append_unique_value(result, item);
  }
  return result;
}

static bool native_sequence_is_subset(
    const std::vector<NativeValue> &left,
    const std::vector<NativeValue> &right) {
  for (const NativeValue &item : native_unique_sequence_items(left)) {
    if (!native_sequence_contains_value(right, item)) return false;
  }
  return true;
}

static NativeValue native_set_operation(const NativeValue &receiver,
                                        const NativeValue &other,
                                        const std::string &selector) {
  const std::vector<NativeValue> left = native_sequence_items_copy(receiver);
  const std::vector<NativeValue> right = native_sequence_items_copy(other);
  if (selector == "union") {
    return native_materialize_set_like_result(
        receiver, native_sequence_union_items(left, right));
  }
  if (selector == "intersection") {
    return native_materialize_set_like_result(
        receiver, native_sequence_intersection_items(left, right));
  }
  if (selector == "difference" || selector == "left_difference") {
    return native_materialize_set_like_result(
        receiver, native_sequence_difference_items(left, right));
  }
  if (selector == "symmetric_difference") {
    return native_materialize_set_like_result(
        receiver, native_sequence_symmetric_difference_items(left, right));
  }
  if (selector == "subset?" || selector == "proper_subset?") {
    const bool subset = native_sequence_is_subset(left, right);
    const bool proper =
        native_unique_sequence_items(left).size() <
        native_unique_sequence_items(right).size();
    return NativeValue::boolean(selector == "subset?" ? subset
                                                      : subset && proper);
  }
  if (selector == "superset?" || selector == "proper_superset?") {
    const bool superset = native_sequence_is_subset(right, left);
    const bool proper =
        native_unique_sequence_items(left).size() >
        native_unique_sequence_items(right).size();
    return NativeValue::boolean(selector == "superset?" ? superset
                                                       : superset && proper);
  }
  if (selector == "disjoint?") {
    return NativeValue::boolean(
        native_sequence_intersection_items(left, right).empty());
  }
  throw NativeBailout();
}

static NativeValue native_sequence_added(const NativeValue &receiver,
                                         const NativeValue &value) {
  std::vector<NativeValue> result = native_sequence_items_copy(receiver);
  native_append_unique_value(result, value);
  return native_materialize_set_like_result(receiver, std::move(result));
}

static NativeValue native_set_add_mut(const NativeValue &receiver,
                                      const NativeValue &value) {
  NativeSet &set = as_mutable_set(receiver);
  native_append_unique_value(set.items, native_normalize_set_element(value));
  return receiver;
}

static NativeValue native_set_subtract_mut(const NativeValue &receiver,
                                           const NativeValue &other) {
  NativeSet &set = as_mutable_set(receiver);
  const std::vector<NativeValue> right = native_sequence_items_copy(other);
  std::vector<NativeValue> kept;
  kept.reserve(set.items.size());
  for (const NativeValue &item : set.items) {
    if (!native_sequence_contains_value(right, item)) kept.push_back(item);
  }
  set.items = std::move(kept);
  return receiver;
}

static int native_compare_for_sort(const NativeValue &lhs,
                                   const NativeValue &rhs);

static int native_compare_sequences_for_sort(
    const std::vector<NativeValue> &lhs,
    const std::vector<NativeValue> &rhs) {
  const std::size_t shared = std::min(lhs.size(), rhs.size());
  for (std::size_t i = 0; i < shared; ++i) {
    const int cmp = native_compare_for_sort(lhs[i], rhs[i]);
    if (cmp != 0) return cmp;
  }
  if (lhs.size() < rhs.size()) return -1;
  if (lhs.size() > rhs.size()) return 1;
  return 0;
}

static int native_compare_for_sort(const NativeValue &lhs,
                                   const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return compare_int64(lhs.scalar_value, rhs.scalar_value);
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    return compare_double_native(as_double_numeric(lhs), as_double_numeric(rhs));
  }
  if (lhs.tag == NativeValue::Tag::Bool &&
      rhs.tag == NativeValue::Tag::Bool) {
    const bool left = lhs.scalar_value != 0;
    const bool right = rhs.scalar_value != 0;
    return left == right ? 0 : (left ? 1 : -1);
  }
  if (native_value_is_string(lhs) &&
      native_value_is_string(rhs)) {
    const std::string &left = native_string_text(lhs);
    const std::string &right = native_string_text(rhs);
    return left < right ? -1 : (left > right ? 1 : 0);
  }
  if (lhs.tag == NativeValue::Tag::Symbol &&
      rhs.tag == NativeValue::Tag::Symbol) {
    const std::string &left = native_symbol_text(lhs.scalar_value);
    const std::string &right = native_symbol_text(rhs.scalar_value);
    return left < right ? -1 : (left > right ? 1 : 0);
  }
  if (lhs.tag == NativeValue::Tag::List &&
      rhs.tag == NativeValue::Tag::List) {
    return native_compare_sequences_for_sort(as_list(lhs).items,
                                             as_list(rhs).items);
  }
  if (lhs.tag == NativeValue::Tag::Tuple &&
      rhs.tag == NativeValue::Tag::Tuple) {
    return native_compare_sequences_for_sort(as_tuple(lhs).items,
                                             as_tuple(rhs).items);
  }
  throw NativeBailout();
}

static NativeValue native_optional_index(const NativeValue &value,
                                         const NativeValue &index_value) {
  if (value.tag == NativeValue::Tag::Map) {
    return native_map_lookup(value, index_value, true);
  }
  const std::int64_t raw_index = as_int(index_value);
  if (value.tag == NativeValue::Tag::Range && raw_index < 0) {
    return NativeValue::nullv();
  }
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  const std::optional<std::size_t> index =
      native_optional_sequence_index(raw_index, items.size());
  return index.has_value() ? items[*index] : NativeValue::nullv();
}

static NativeValue native_has_index(const NativeValue &value,
                                    const NativeValue &index_value) {
  if (value.tag == NativeValue::Tag::Map) {
    return native_map_contains_key(value, index_value);
  }
  const std::int64_t raw_index = as_int(index_value);
  if (value.tag == NativeValue::Tag::Range && raw_index < 0) {
    return NativeValue::boolean(false);
  }
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  return NativeValue::boolean(
      native_optional_sequence_index(raw_index, items.size()).has_value());
}

static NativeValue native_deconstruct(const NativeValue &value) {
  if (!native_is_sequence(value)) throw NativeBailout();
  return value;
}

static NativeValue native_to_array(const NativeValue &value) {
  if (value.tag == NativeValue::Tag::Map) {
    return native_map_entries_array(value);
  }
  return NativeValue::list(native_sequence_items_copy(value));
}

static NativeValue native_sequence_appended(
    const NativeValue &value, std::initializer_list<NativeValue> additions) {
  std::vector<NativeValue> result = native_sequence_items_copy(value);
  result.insert(result.end(), additions.begin(), additions.end());
  return NativeValue::list(std::move(result));
}

static NativeValue native_sequence_inserted(
    const NativeValue &value, const NativeValue &index_value,
    std::initializer_list<NativeValue> additions) {
  std::vector<NativeValue> result = native_sequence_items_copy(value);
  const std::int64_t raw = as_int(index_value);
  const std::int64_t size_i64 = static_cast<std::int64_t>(result.size());
  const std::int64_t normalized = raw < 0 ? size_i64 + raw + 1 : raw;
  if (normalized < 0 || normalized > size_i64) throw NativeBailout();
  result.insert(result.begin() + normalized, additions.begin(),
                additions.end());
  return NativeValue::list(std::move(result));
}

static NativeValue native_sequence_deleted(const NativeValue &value,
                                           const NativeValue &needle) {
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  std::vector<NativeValue> result;
  result.reserve(items.size());
  for (const NativeValue &item : items) {
    if (!native_value_equal(item, needle)) result.push_back(item);
  }
  if (value.tag == NativeValue::Tag::Set) {
    return native_set_from_items(std::move(result));
  }
  return NativeValue::list(std::move(result));
}

static NativeValue native_sequence_reversed(const NativeValue &value) {
  std::vector<NativeValue> result = native_sequence_items_copy(value);
  std::reverse(result.begin(), result.end());
  return NativeValue::list(std::move(result));
}

static NativeValue native_sequence_init(const NativeValue &value) {
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  if (items.empty()) return NativeValue::list({});
  return NativeValue::list(
      std::vector<NativeValue>(items.begin(), items.end() - 1));
}

static NativeValue native_sequence_tail(const NativeValue &value) {
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  if (items.empty()) return NativeValue::list({});
  return NativeValue::list(
      std::vector<NativeValue>(items.begin() + 1, items.end()));
}

static NativeValue native_sequence_take(const NativeValue &value,
                                        const NativeValue &count_value) {
  const std::int64_t raw_count = as_int(count_value);
  if (raw_count < 0) throw NativeBailout();
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  const std::size_t count = std::min<std::size_t>(
      static_cast<std::size_t>(raw_count), items.size());
  return NativeValue::list(
      std::vector<NativeValue>(items.begin(), items.begin() + count));
}

static NativeValue native_sequence_drop(const NativeValue &value,
                                        const NativeValue &count_value) {
  const std::int64_t raw_count = as_int(count_value);
  if (raw_count < 0) throw NativeBailout();
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  const std::size_t count = std::min<std::size_t>(
      static_cast<std::size_t>(raw_count), items.size());
  return NativeValue::list(
      std::vector<NativeValue>(items.begin() + count, items.end()));
}

static NativeValue native_sequence_concat(const NativeValue &lhs,
                                          const NativeValue &rhs) {
  std::vector<NativeValue> result = native_sequence_items_copy(lhs);
  const std::vector<NativeValue> right = native_sequence_items_copy(rhs);
  result.insert(result.end(), right.begin(), right.end());
  return NativeValue::list(std::move(result));
}

static NativeValue native_sequence_repeat(const NativeValue &value,
                                          const NativeValue &count_value) {
  const std::int64_t raw_count = as_int(count_value);
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  std::vector<NativeValue> result;
  if (raw_count <= 0) return NativeValue::list(std::move(result));
  const std::uint64_t count = static_cast<std::uint64_t>(raw_count);
  if (!items.empty() &&
      count > std::numeric_limits<std::size_t>::max() / items.size()) {
    throw NativeBailout();
  }
  result.reserve(items.size() * static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    result.insert(result.end(), items.begin(), items.end());
  }
  return NativeValue::list(std::move(result));
}

static NativeValue native_sequence_sorted(const NativeValue &value) {
  std::vector<NativeValue> result = native_sequence_items_copy(value);
  for (std::size_t i = 1; i < result.size(); ++i) {
    std::size_t j = i;
    while (j > 0 && native_compare_for_sort(result[j], result[j - 1U]) < 0) {
      std::swap(result[j], result[j - 1U]);
      --j;
    }
  }
  return NativeValue::list(std::move(result));
}

static NativeValue native_sequence_extreme(const NativeValue &value,
                                           const std::string &selector) {
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  if (items.empty()) return NativeValue::nullv();
  std::size_t min_i = 0;
  std::size_t max_i = 0;
  for (std::size_t i = 1; i < items.size(); ++i) {
    if (native_compare_for_sort(items[i], items[min_i]) < 0) min_i = i;
    if (native_compare_for_sort(items[i], items[max_i]) > 0) max_i = i;
  }
  if (selector == "min") return items[min_i];
  if (selector == "max") return items[max_i];
  if (selector == "minmax") {
    return NativeValue::list({items[min_i], items[max_i]});
  }
  throw NativeBailout();
}

static NativeValue native_concat(const NativeValue &lhs,
                                 const NativeValue &rhs) {
  if (native_value_is_string(lhs) &&
      native_value_is_string(rhs)) {
    return native_string_concat(lhs, rhs);
  }
  if (native_is_sequence(lhs)) {
    return native_sequence_concat(lhs, rhs);
  }
  throw NativeBailout();
}

)AMBERCPP";
  out << R"AMBERCPP(static NativeValue amber_native_call_closure(const NativeValue &value, std::initializer_list<NativeValue> args);

static bool native_truthy(const NativeValue &value) {
  return value.tag != NativeValue::Tag::Null &&
         !(value.tag == NativeValue::Tag::Bool && value.scalar_value == 0);
}

static const std::string &native_key_text(const NativeValue &value) {
  return native_name_key_text(value);
}

static NativeValue native_map_set(const NativeValue &value,
                                  const NativeValue &key,
                                  const NativeValue &next_value) {
  if (value.tag != NativeValue::Tag::Map) throw NativeBailout();
  native_map_store(as_mutable_map(value), key, next_value);
  return next_value;
}

static NativeValue native_index_set(const NativeValue &value,
                                    const NativeValue &key,
                                    const NativeValue &next_value) {
  if (value.tag == NativeValue::Tag::List) {
    return native_list_set(value, key, next_value);
  }
  if (value.tag == NativeValue::Tag::Map) {
    return native_map_set(value, key, next_value);
  }
  throw NativeBailout();
}

static NativeValue native_contains(const NativeValue &value,
                                   const NativeValue &needle) {
  if (native_value_is_string(value)) {
    return native_string_contains(value, needle);
  }
  if (native_is_sequence(value)) {
    const std::vector<NativeValue> items = native_sequence_items_copy(value);
    for (const NativeValue &item : items) {
      if (native_value_equal(item, needle)) return NativeValue::boolean(true);
    }
    return NativeValue::boolean(false);
  }
  if (value.tag == NativeValue::Tag::Map) {
    return native_map_contains_key(value, needle);
  }
  throw NativeBailout();
}

static NativeValue native_each(const NativeValue &value,
                               const NativeValue &block_value) {
  if (value.tag == NativeValue::Tag::Map) {
    const auto entries = as_map(value).entries;
    for (const auto &entry : entries) {
      (void)amber_native_call_closure(
          block_value, {entry.first, entry.second});
    }
    return value;
  }
  if (native_is_sequence(value)) {
    const auto items = native_sequence_items_copy(value);
    for (const NativeValue &item : items) {
      (void)amber_native_call_closure(block_value, {item});
    }
    return value;
  }
  throw NativeBailout();
}

static NativeValue native_sequence_higher_order(
    const NativeValue &value, const NativeValue &block_value,
    const std::string &selector) {
  if (value.tag == NativeValue::Tag::Map) {
    const NativeMap &map = as_map(value);
    if (selector == "map") {
      std::vector<NativeValue> mapped;
      mapped.reserve(map.entries.size());
      for (const auto &entry : map.entries) {
        mapped.push_back(
            amber_native_call_closure(block_value, {entry.first, entry.second}));
      }
      return NativeValue::list(std::move(mapped));
    }
    if (selector == "select" || selector == "reject") {
      std::vector<std::pair<NativeValue, NativeValue>> filtered;
      filtered.reserve(map.entries.size());
      for (const auto &entry : map.entries) {
        const bool keep = native_truthy(
            amber_native_call_closure(block_value, {entry.first, entry.second}));
        if ((selector == "select" && keep) ||
            (selector == "reject" && !keep)) {
          filtered.push_back(entry);
        }
      }
      return NativeValue::map_entries(std::move(filtered), false);
    }
    if (selector == "transform_values") {
      std::vector<std::pair<NativeValue, NativeValue>> transformed;
      transformed.reserve(map.entries.size());
      for (const auto &entry : map.entries) {
        transformed.emplace_back(
            entry.first,
            amber_native_call_closure(block_value, {entry.second, entry.first}));
      }
      return NativeValue::map_entries(std::move(transformed), false);
    }
    if (selector == "transform_keys") {
      std::vector<std::pair<NativeValue, NativeValue>> transformed;
      transformed.reserve(map.entries.size());
      for (const auto &entry : map.entries) {
        NativeValue key =
            amber_native_call_closure(block_value, {entry.first, entry.second});
        native_map_store(transformed, key, entry.second, false);
      }
      return NativeValue::map_entries(std::move(transformed), false);
    }
    throw NativeBailout();
  }
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  if (selector == "map") {
    std::vector<NativeValue> mapped;
    mapped.reserve(items.size());
    for (const NativeValue &item : items) {
      mapped.push_back(amber_native_call_closure(block_value, {item}));
    }
    return NativeValue::list(std::move(mapped));
  }
  if (selector == "select" || selector == "reject") {
    std::vector<NativeValue> filtered;
    filtered.reserve(items.size());
    for (const NativeValue &item : items) {
      const bool keep =
          native_truthy(amber_native_call_closure(block_value, {item}));
      if ((selector == "select" && keep) ||
          (selector == "reject" && !keep)) {
        filtered.push_back(item);
      }
    }
    return NativeValue::list(std::move(filtered));
  }
  if (selector == "find") {
    for (const NativeValue &item : items) {
      if (native_truthy(amber_native_call_closure(block_value, {item}))) {
        return item;
      }
    }
    return NativeValue::nullv();
  }
  throw NativeBailout();
}

static NativeValue native_sequence_predicate(
    const NativeValue &value, const NativeValue &block_value, bool has_block,
    const std::string &selector) {
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  bool saw_any = false;
  bool all_match = true;
  bool any_match = false;
  for (const NativeValue &item : items) {
    saw_any = true;
    const NativeValue predicate =
        has_block ? amber_native_call_closure(block_value, {item}) : item;
    const bool truthy = native_truthy(predicate);
    any_match = any_match || truthy;
    all_match = all_match && truthy;
    if ((selector == "any?" && any_match) ||
        (selector == "all?" && !all_match) ||
        (selector == "none?" && any_match)) {
      break;
    }
  }
  if (selector == "any?") return NativeValue::boolean(any_match);
  if (selector == "all?") return NativeValue::boolean(!saw_any || all_match);
  if (selector == "none?") return NativeValue::boolean(!any_match);
  throw NativeBailout();
}

static NativeValue native_sequence_reduce(
    const NativeValue &value, const NativeValue &initial_value,
    bool has_initial, const NativeValue &block_value) {
  const std::vector<NativeValue> items = native_sequence_items_copy(value);
  if (items.empty() && !has_initial) throw NativeBailout();
  NativeValue accumulator =
      has_initial ? initial_value : (items.empty() ? NativeValue::nullv()
                                                  : items.front());
  std::size_t index = has_initial ? 0U : 1U;
  for (; index < items.size(); ++index) {
    accumulator =
        amber_native_call_closure(block_value, {accumulator, items[index]});
  }
  return accumulator;
}

static NativeValue native_index(const NativeValue &value, const NativeValue &key) {
  if (native_is_sequence(value)) {
    return native_list_at(value, key);
  }
  if (value.tag == NativeValue::Tag::Bytes) {
    const auto &bytes = as_bytes(value).bytes;
    std::int64_t index = as_int(key);
    if (index < 0) index += static_cast<std::int64_t>(bytes.size());
    if (index < 0 || static_cast<std::size_t>(index) >= bytes.size()) throw NativeBailout();
    return NativeValue::integer(static_cast<unsigned char>(bytes[static_cast<std::size_t>(index)]));
  }
  if (value.tag == NativeValue::Tag::Map) {
    return native_map_lookup(value, key, false);
  }
  if (value.tag == NativeValue::Tag::RegexpMatch) {
    return native_regexp_group_value(as_regexp_match(value), key);
  }
  throw NativeBailout();
}

static NativeValue native_bytes_new(const NativeValue &value) {
  if (native_value_is_string(value)) {
    return NativeValue::bytes(native_string_text(value));
  }
  if (value.tag == NativeValue::Tag::Bytes) {
    return NativeValue::bytes(as_bytes(value).bytes);
  }
  throw NativeBailout();
}

static NativeValue native_bytes_slice(const NativeValue &value,
                                      const NativeValue &start_value,
                                      const NativeValue &length_value,
                                      bool has_length) {
  const auto &bytes = as_bytes(value).bytes;
  std::int64_t normalized = as_int(start_value);
  if (normalized < 0) normalized += static_cast<std::int64_t>(bytes.size());
  normalized = std::clamp<std::int64_t>(
      normalized, 0, static_cast<std::int64_t>(bytes.size()));
  const std::size_t offset = static_cast<std::size_t>(normalized);
  std::size_t count = bytes.size() - offset;
  if (has_length && length_value.tag != NativeValue::Tag::Null) {
    const std::int64_t raw_length = as_int(length_value);
    if (raw_length < 0) throw NativeBailout();
    count = std::min<std::size_t>(static_cast<std::size_t>(raw_length),
                                  bytes.size() - offset);
  }
  return NativeValue::bytes(bytes.substr(offset, count));
}

static NativeValue native_slice(const NativeValue &value,
                                const NativeValue &first,
                                const NativeValue &second,
                                bool has_second) {
  if (value.tag == NativeValue::Tag::Bytes) {
    return native_bytes_slice(value, first, second, has_second);
  }
  if (value.tag == NativeValue::Tag::Map) {
    return native_map_slice(value, first, second, has_second);
  }
  throw NativeBailout();
}

static std::string native_hex_encode_bytes(const std::string &bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2U);
  for (unsigned char byte : bytes) {
    out.push_back(digits[(byte >> 4U) & 0x0FU]);
    out.push_back(digits[byte & 0x0FU]);
  }
  return out;
}

static NativeValue native_bytes_hex(const NativeValue &value) {
  return NativeValue::heap_string(
      native_hex_encode_bytes(as_bytes(value).bytes));
}

static int native_hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

static bool native_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
         c == '\f' || c == '\v';
}

static std::string native_hex_decode_text(const std::string &text, bool lenient) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string normalized;
  normalized.reserve(text.size());
  bool allow_prefix = true;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (lenient && (native_ascii_space(c) || c == ':' || c == '-')) continue;
    if (lenient && allow_prefix && c == '0' && i + 1U < text.size() &&
        (text[i + 1U] == 'x' || text[i + 1U] == 'X')) {
      allow_prefix = false;
      ++i;
      continue;
    }
    const int value = native_hex_value(c);
    if (value < 0) throw NativeBailout();
    allow_prefix = false;
    normalized.push_back(digits[value]);
  }
  if (normalized.size() % 2U != 0U) throw NativeBailout();
  std::string out;
  out.reserve(normalized.size() / 2U);
  for (std::size_t i = 0; i < normalized.size(); i += 2U) {
    out.push_back(static_cast<char>((native_hex_value(normalized[i]) << 4U) |
                                    native_hex_value(normalized[i + 1U])));
  }
  return out;
}

static int native_base64_value(char c, bool url) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return 26 + c - 'a';
  if (c >= '0' && c <= '9') return 52 + c - '0';
  if (url) {
    if (c == '-') return 62;
    if (c == '_') return 63;
  } else {
    if (c == '+') return 62;
    if (c == '/') return 63;
  }
  return -1;
}

static std::string native_base64_encode_bytes(const std::string &bytes,
                                              bool url, bool padding) {
  static constexpr char std_alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static constexpr char url_alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  const char *alphabet = url ? url_alphabet : std_alphabet;
  std::string out;
  out.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t i = 0; i < bytes.size(); i += 3U) {
    const std::uint32_t b0 = static_cast<unsigned char>(bytes[i]);
    const bool have_b1 = i + 1U < bytes.size();
    const bool have_b2 = i + 2U < bytes.size();
    const std::uint32_t b1 = have_b1 ? static_cast<unsigned char>(bytes[i + 1U]) : 0U;
    const std::uint32_t b2 = have_b2 ? static_cast<unsigned char>(bytes[i + 2U]) : 0U;
    out.push_back(alphabet[(b0 >> 2U) & 0x3FU]);
    out.push_back(alphabet[((b0 & 0x03U) << 4U) | ((b1 >> 4U) & 0x0FU)]);
    if (have_b1) out.push_back(alphabet[((b1 & 0x0FU) << 2U) | ((b2 >> 6U) & 0x03U)]);
    else if (padding) out.push_back('=');
    if (have_b2) out.push_back(alphabet[b2 & 0x3FU]);
    else if (padding) out.push_back('=');
  }
  return out;
}

static bool native_capability_allowed(const std::string &capability) {
  for (const auto &grant : embedded_capability_grants()) {
    if (grant.name != capability) continue;
    if ((grant.flags & amber::capability::kCapabilityFlagWildcardTarget) != 0U ||
        grant.target == "*" || grant.target.empty()) {
      return true;
    }
  }
  return false;
}

static std::string native_secure_random_bytes_raw(std::size_t count) {
  if (!native_capability_allowed("random.secure")) throw NativeBailout();
  std::string out(count, '\0');
  if (count == 0U) return out;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  arc4random_buf(&out[0], out.size());
  return out;
#elif defined(__linux__)
  char *cursor = &out[0];
  std::size_t remaining = out.size();
  while (remaining > 0U) {
    const ssize_t got = getrandom(cursor, remaining, 0);
    if (got < 0) {
      if (errno == EINTR) continue;
      throw NativeBailout();
    }
    if (got == 0) throw NativeBailout();
    cursor += got;
    remaining -= static_cast<std::size_t>(got);
  }
  return out;
#else
  throw NativeBailout();
#endif
}

static std::size_t native_secure_random_count(const NativeValue &value) {
  if (value.tag != NativeValue::Tag::Integer || value.scalar_value < 0) {
    throw NativeBailout();
  }
  if (static_cast<std::uint64_t>(value.scalar_value) >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw NativeBailout();
  }
  return static_cast<std::size_t>(value.scalar_value);
}

static NativeValue native_range_new(const NativeValue &module,
                                    const NativeValue &start_value,
                                    const NativeValue &finish_value,
                                    const NativeValue &inclusive_value,
                                    bool has_inclusive,
                                    const NativeValue &step_value,
                                    bool has_step) {
  if (module.tag != NativeValue::Tag::RangeModule ||
      start_value.tag != NativeValue::Tag::Integer ||
      finish_value.tag != NativeValue::Tag::Integer) {
    throw NativeBailout();
  }
  NativeRange range;
  range.start = start_value.scalar_value;
  range.finish = finish_value.scalar_value;
  range.inclusive_end = true;
  if (has_inclusive) {
    if (inclusive_value.tag != NativeValue::Tag::Bool) throw NativeBailout();
    range.inclusive_end = inclusive_value.scalar_value != 0;
  }
  range.step = 1;
  if (has_step && step_value.tag != NativeValue::Tag::Null) {
    if (step_value.tag != NativeValue::Tag::Integer) throw NativeBailout();
    range.step = step_value.scalar_value;
  }
  if (range.step == 0) throw NativeBailout();
  return NativeValue::range(range);
}

static std::uint64_t native_load_u64_le(const std::string &bytes) {
  if (bytes.size() < 8U) throw NativeBailout();
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8U; ++i) {
    value |= static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes[i]))
             << (i * 8U);
  }
  return value;
}

static NativeValue native_secure_random_int(const NativeValue &module,
                                            const NativeValue &range_value) {
  if (module.tag != NativeValue::Tag::SecureRandomModule) throw NativeBailout();
  const NativeRange &range = as_range(range_value);
  const __int128 start = static_cast<__int128>(range.start);
  const __int128 finish = static_cast<__int128>(range.finish);
  const __int128 step = static_cast<__int128>(range.step);
  __int128 count = 0;
  if (step > 0) {
    const __int128 last = range.inclusive_end ? finish : finish - 1;
    if (start > last) throw NativeBailout();
    count = ((last - start) / step) + 1;
  } else {
    const __int128 last = range.inclusive_end ? finish : finish + 1;
    if (start < last) throw NativeBailout();
    count = ((start - last) / (-step)) + 1;
  }
  if (count <= 0 ||
      count > static_cast<__int128>(std::numeric_limits<std::uint64_t>::max())) {
    throw NativeBailout();
  }
  const std::uint64_t count_u64 = static_cast<std::uint64_t>(count);
  if (count_u64 == 1U) {
    (void)native_secure_random_bytes_raw(0U);
    return NativeValue::integer(range.start);
  }
  const std::uint64_t threshold =
      (std::numeric_limits<std::uint64_t>::max() - count_u64 + 1U) %
      count_u64;
  while (true) {
    const std::uint64_t sample =
        native_load_u64_le(native_secure_random_bytes_raw(8U));
    if (sample < threshold) continue;
    const std::uint64_t offset = sample % count_u64;
    const __int128 value =
        start + step * static_cast<__int128>(offset);
    if (value < static_cast<__int128>(std::numeric_limits<std::int64_t>::min()) ||
        value > static_cast<__int128>(std::numeric_limits<std::int64_t>::max())) {
      throw NativeBailout();
    }
    return NativeValue::integer(static_cast<std::int64_t>(value));
  }
}

static NativeValue native_secure_random_bytes_value(const NativeValue &module,
                                                    const NativeValue &count_value) {
  if (module.tag != NativeValue::Tag::SecureRandomModule) throw NativeBailout();
  return NativeValue::bytes(native_secure_random_bytes_raw(
      native_secure_random_count(count_value)));
}

static NativeValue native_secure_random_hex(const NativeValue &module,
                                            const NativeValue &count_value) {
  if (module.tag != NativeValue::Tag::SecureRandomModule) throw NativeBailout();
  return NativeValue::heap_string(native_hex_encode_bytes(
      native_secure_random_bytes_raw(native_secure_random_count(count_value))));
}

static NativeValue native_secure_random_base64(const NativeValue &module,
                                               const NativeValue &count_value,
                                               const NativeValue &padding_value,
                                               bool has_padding,
                                               bool url) {
  if (module.tag != NativeValue::Tag::SecureRandomModule) throw NativeBailout();
  bool padding = !url;
  if (has_padding) {
    if (padding_value.tag != NativeValue::Tag::Bool) throw NativeBailout();
    padding = padding_value.scalar_value != 0;
  }
  return NativeValue::heap_string(native_base64_encode_bytes(
      native_secure_random_bytes_raw(native_secure_random_count(count_value)),
      url, padding));
}

static NativeValue native_uuid_v4(const NativeValue &module) {
  if (module.tag != NativeValue::Tag::UuidModule &&
      module.tag != NativeValue::Tag::SecureRandomModule) {
    throw NativeBailout();
  }
  const std::string random = native_secure_random_bytes_raw(16U);
  NativeUuid uuid;
  for (std::size_t i = 0; i < uuid.bytes.size(); ++i) {
    uuid.bytes[i] =
        static_cast<std::uint8_t>(static_cast<unsigned char>(random[i]));
  }
  uuid.bytes[6] =
      static_cast<std::uint8_t>((uuid.bytes[6] & 0x0FU) | 0x40U);
  uuid.bytes[8] =
      static_cast<std::uint8_t>((uuid.bytes[8] & 0x3FU) | 0x80U);
  return NativeValue::uuid(uuid);
}

static NativeValue native_uuid_v7(const NativeValue &module) {
  if (module.tag != NativeValue::Tag::UuidModule) throw NativeBailout();
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  if (millis < 0 ||
      static_cast<std::uint64_t>(millis) > ((1ULL << 48U) - 1U)) {
    throw NativeBailout();
  }
  const std::string random = native_secure_random_bytes_raw(10U);
  NativeUuid uuid;
  const std::uint64_t timestamp = static_cast<std::uint64_t>(millis);
  for (std::size_t i = 0; i < 6U; ++i) {
    uuid.bytes[i] =
        static_cast<std::uint8_t>(timestamp >> ((5U - i) * 8U));
  }
  for (std::size_t i = 0; i < random.size(); ++i) {
    uuid.bytes[6U + i] =
        static_cast<std::uint8_t>(static_cast<unsigned char>(random[i]));
  }
  uuid.bytes[6] =
      static_cast<std::uint8_t>((uuid.bytes[6] & 0x0FU) | 0x70U);
  uuid.bytes[8] =
      static_cast<std::uint8_t>((uuid.bytes[8] & 0x3FU) | 0x80U);
  return NativeValue::uuid(uuid);
}

static NativeValue native_uuid_parse(const NativeValue &module,
                                     const NativeValue &text_value) {
  if (module.tag != NativeValue::Tag::UuidModule ||
      !native_value_is_string(text_value)) {
    throw NativeBailout();
  }
  const std::string &text = native_string_text(text_value);
  if (text.size() != 36U || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    throw NativeBailout();
  }
  NativeUuid uuid;
  std::size_t byte_index = 0;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '-') {
      ++i;
      continue;
    }
    if (i + 1U >= text.size() || byte_index >= uuid.bytes.size()) {
      throw NativeBailout();
    }
    const int high = native_hex_value(text[i]);
    const int low = native_hex_value(text[i + 1U]);
    if (high < 0 || low < 0) throw NativeBailout();
    uuid.bytes[byte_index++] =
        static_cast<std::uint8_t>((high << 4U) | low);
    i += 2U;
  }
  if (byte_index != uuid.bytes.size()) throw NativeBailout();
  return NativeValue::uuid(uuid);
}

static NativeValue native_uuid_nullary(const NativeValue &receiver,
                                       std::string_view selector) {
  const NativeUuid &uuid = as_uuid(receiver);
  if (selector == "to_str" || selector == "inspect") {
    return NativeValue::heap_string(
        amber::runtime::runtime_uuid_to_string(uuid));
  }
  if (selector == "version") {
    return NativeValue::integer((uuid.bytes[6] >> 4U) & 0x0FU);
  }
  throw NativeBailout();
}

static NativeValue native_type_matches(const NativeValue &module,
                                       const NativeValue &value) {
  bool matched = false;
  if (module.tag == NativeValue::Tag::StrType) {
    matched = native_value_is_string(value);
  } else if (module.tag == NativeValue::Tag::IntType) {
    matched = value.tag == NativeValue::Tag::Integer;
  } else if (module.tag == NativeValue::Tag::BigIntType) {
    matched = false;
  } else if (module.tag == NativeValue::Tag::FloatType) {
    matched = value.tag == NativeValue::Tag::Float;
  } else if (module.tag == NativeValue::Tag::BoolType) {
    matched = value.tag == NativeValue::Tag::Bool;
  } else if (module.tag == NativeValue::Tag::SymbolType) {
    matched = value.tag == NativeValue::Tag::Symbol;
  } else if (module.tag == NativeValue::Tag::NullType) {
    matched = value.tag == NativeValue::Tag::Null;
  } else if (module.tag == NativeValue::Tag::ObjectType) {
    matched = true;
  } else if (module.tag == NativeValue::Tag::UuidModule) {
    matched = value.tag == NativeValue::Tag::Uuid ||
              value.tag == NativeValue::Tag::UuidModule;
  } else if (module.tag == NativeValue::Tag::RegexpModule) {
    matched = value.tag == NativeValue::Tag::Regexp ||
              value.tag == NativeValue::Tag::RegexpMatch ||
              value.tag == NativeValue::Tag::RegexpModule;
  } else if (module.tag == NativeValue::Tag::TimeModule) {
    matched = value.tag == NativeValue::Tag::Time ||
              value.tag == NativeValue::Tag::TimeModule;
  } else if (module.tag == NativeValue::Tag::TimePeriodModule) {
    matched = value.tag == NativeValue::Tag::TimePeriod ||
              value.tag == NativeValue::Tag::TimePeriodModule;
  } else if (module.tag == NativeValue::Tag::BytesModule) {
    matched = value.tag == NativeValue::Tag::Bytes ||
              value.tag == NativeValue::Tag::BytesModule;
  } else if (module.tag == NativeValue::Tag::RangeModule) {
    matched = value.tag == NativeValue::Tag::Range ||
              value.tag == NativeValue::Tag::RangeModule;
  } else if (module.tag == NativeValue::Tag::UrlModule) {
    matched = value.tag == NativeValue::Tag::UrlModule;
  } else if (module.tag == NativeValue::Tag::BenchmarkModule) {
    matched = value.tag == NativeValue::Tag::BenchmarkModule;
  } else if (module.tag == NativeValue::Tag::ArgParserModule) {
    matched = value.tag == NativeValue::Tag::ArgParser ||
              value.tag == NativeValue::Tag::ArgParserModule;
  } else {
    throw NativeBailout();
  }
  return NativeValue::boolean(matched);
}

static NativeValue native_secure_random_uuid(const NativeValue &module) {
  return native_uuid_v4(module);
}

static std::string native_base64_decode_text(const std::string &text,
                                             bool url, bool lenient) {
  std::string normalized;
  normalized.reserve(text.size());
  for (char c : text) {
    if (native_ascii_space(c)) {
      if (lenient) continue;
      throw NativeBailout();
    }
    if (c == '=' || native_base64_value(c, url) >= 0) {
      normalized.push_back(c);
    } else {
      throw NativeBailout();
    }
  }
  const std::size_t first_pad = normalized.find('=');
  if (first_pad != std::string::npos) {
    for (std::size_t i = first_pad; i < normalized.size(); ++i) {
      if (normalized[i] != '=') throw NativeBailout();
    }
    const std::size_t pad_count = normalized.size() - first_pad;
    if (pad_count > 2U || normalized.size() % 4U != 0U) throw NativeBailout();
  } else {
    const std::size_t residue = normalized.size() % 4U;
    if (residue == 1U) throw NativeBailout();
    if (residue != 0U) normalized.append(4U - residue, '=');
  }
  std::string out;
  out.reserve((normalized.size() / 4U) * 3U);
  for (std::size_t i = 0; i < normalized.size(); i += 4U) {
    const char c0 = normalized[i];
    const char c1 = normalized[i + 1U];
    const char c2 = normalized[i + 2U];
    const char c3 = normalized[i + 3U];
    const bool last = i + 4U == normalized.size();
    if (c0 == '=' || c1 == '=' || (c2 == '=' && c3 != '=') ||
        ((c2 == '=' || c3 == '=') && !last)) {
      throw NativeBailout();
    }
    const int v0 = native_base64_value(c0, url);
    const int v1 = native_base64_value(c1, url);
    const int v2 = c2 == '=' ? 0 : native_base64_value(c2, url);
    const int v3 = c3 == '=' ? 0 : native_base64_value(c3, url);
    if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) throw NativeBailout();
    if (c2 == '=') {
      if ((v1 & 0x0F) != 0) throw NativeBailout();
      out.push_back(static_cast<char>((v0 << 2U) | (v1 >> 4U)));
    } else if (c3 == '=') {
      if ((v2 & 0x03) != 0) throw NativeBailout();
      out.push_back(static_cast<char>((v0 << 2U) | (v1 >> 4U)));
      out.push_back(static_cast<char>(((v1 & 0x0FU) << 4U) | (v2 >> 2U)));
    } else {
      out.push_back(static_cast<char>((v0 << 2U) | (v1 >> 4U)));
      out.push_back(static_cast<char>(((v1 & 0x0FU) << 4U) | (v2 >> 2U)));
      out.push_back(static_cast<char>(((v2 & 0x03U) << 6U) | v3));
    }
  }
  return out;
}

static bool native_decode_lenient(const NativeValue &mode_value, bool has_mode) {
  if (!has_mode) return false;
  std::string mode;
  if (native_value_is_string(mode_value)) mode = native_string_text(mode_value);
  else if (mode_value.tag == NativeValue::Tag::Symbol) mode = native_symbol_text(mode_value.scalar_value);
  else throw NativeBailout();
  if (mode == "strict") return false;
  if (mode == "lenient") return true;
  throw NativeBailout();
}

static NativeValue native_codec_encode(const NativeValue &module,
                                       const NativeValue &bytes_value,
                                       const NativeValue &padding_value,
                                       bool has_padding) {
  const std::string &bytes = as_bytes(bytes_value).bytes;
  if (module.tag == NativeValue::Tag::HexModule) {
    if (has_padding) throw NativeBailout();
    return NativeValue::heap_string(native_hex_encode_bytes(bytes));
  }
  bool url = false;
  bool padding = true;
  if (module.tag == NativeValue::Tag::Base64Module) {
    url = false;
    padding = true;
  } else if (module.tag == NativeValue::Tag::Base64UrlModule) {
    url = true;
    padding = false;
  } else {
    throw NativeBailout();
  }
  if (has_padding) {
    if (padding_value.tag != NativeValue::Tag::Bool) throw NativeBailout();
    padding = padding_value.scalar_value != 0;
  }
  return NativeValue::heap_string(
      native_base64_encode_bytes(bytes, url, padding));
}

static NativeValue native_codec_decode(const NativeValue &module,
                                       const NativeValue &text_value,
                                       const NativeValue &mode_value,
                                       bool has_mode) {
  if (!native_value_is_string(text_value)) throw NativeBailout();
  const std::string &text = native_string_text(text_value);
  const bool lenient = native_decode_lenient(mode_value, has_mode);
  if (module.tag == NativeValue::Tag::HexModule) {
    return NativeValue::bytes(native_hex_decode_text(text, lenient));
  }
  if (module.tag == NativeValue::Tag::Base64Module) {
    return NativeValue::bytes(native_base64_decode_text(text, false, lenient));
  }
  if (module.tag == NativeValue::Tag::Base64UrlModule) {
    return NativeValue::bytes(native_base64_decode_text(text, true, lenient));
  }
  throw NativeBailout();
}

static NativeValue native_digest_one(const NativeValue &module,
                                     const NativeValue &bytes_value,
                                     const std::string &selector) {
  if (module.tag != NativeValue::Tag::DigestModule) throw NativeBailout();
  const std::string &bytes = as_bytes(bytes_value).bytes;
  if (selector == "crc32") {
    return NativeValue::bytes(amber::runtime::digest_crc32(bytes));
  }
  if (selector == "md5") {
    return NativeValue::bytes(amber::runtime::digest_md5(bytes));
  }
  if (selector == "sha1") {
    return NativeValue::bytes(amber::runtime::digest_sha1(bytes));
  }
  if (selector == "sha256") {
    return NativeValue::bytes(amber::runtime::digest_sha256(bytes));
  }
  if (selector == "streebog256" || selector == "gost256" ||
      selector == "гост256" || selector == "стрибог256") {
    return NativeValue::bytes(amber::runtime::digest_streebog256(bytes));
  }
  if (selector == "streebog512" || selector == "gost512" ||
      selector == "гост512" || selector == "стрибог512") {
    return NativeValue::bytes(amber::runtime::digest_streebog512(bytes));
  }
  throw NativeBailout();
}

static NativeValue native_digest_hmac_sha256(const NativeValue &module,
                                             const NativeValue &key_value,
                                             const NativeValue &bytes_value) {
  if (module.tag != NativeValue::Tag::DigestModule) throw NativeBailout();
  return NativeValue::bytes(amber::runtime::digest_hmac_sha256(
      as_bytes(key_value).bytes, as_bytes(bytes_value).bytes));
}

static void append_json_escaped(std::string &out, const std::string &text) {
  out.push_back('"');
  for (unsigned char ch : text) {
    switch (ch) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (ch < 0x20) throw NativeBailout();
      out.push_back(static_cast<char>(ch));
      break;
    }
  }
  out.push_back('"');
}

static void append_json_indent(std::string &out, int count) {
  for (int i = 0; i < count; ++i) out.push_back(' ');
}

static void append_json_value(std::string &out, const NativeValue &value,
                              bool pretty, int depth) {
  switch (value.tag) {
  case NativeValue::Tag::Null:
    out += "null";
    return;
  case NativeValue::Tag::Bool:
    out += value.scalar_value != 0 ? "true" : "false";
    return;
  case NativeValue::Tag::Integer:
    out += std::to_string(value.scalar_value);
    return;
  case NativeValue::Tag::Float: {
    char buffer[128];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value.float_value);
    if (converted.ec != std::errc{}) throw NativeBailout();
    out.append(buffer, converted.ptr);
    return;
  }
  case NativeValue::Tag::String:
  case NativeValue::Tag::HeapString:
    append_json_escaped(out, native_string_text(value));
    return;
  case NativeValue::Tag::Symbol:
    append_json_escaped(out, native_symbol_text(value.scalar_value));
    return;
  case NativeValue::Tag::Time:
    append_json_escaped(
        out, amber::runtime::runtime_time_to_iso8601(as_time(value)));
    return;
  case NativeValue::Tag::Uuid:
    append_json_escaped(
        out, amber::runtime::runtime_uuid_to_string(as_uuid(value)));
    return;
  case NativeValue::Tag::List: {
    const auto &items = as_list(value).items;
    out.push_back('[');
    if (pretty && !items.empty()) out.push_back('\n');
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (i != 0U) {
        out.push_back(',');
        if (pretty) out.push_back('\n');
      }
      if (pretty) append_json_indent(out, (depth + 1) * 2);
      append_json_value(out, items[i], pretty, depth + 1);
    }
    if (pretty && !items.empty()) {
      out.push_back('\n');
      append_json_indent(out, depth * 2);
    }
    out.push_back(']');
    return;
  }
  case NativeValue::Tag::Tuple: {
    const auto &items = as_tuple(value).items;
    out.push_back('[');
    if (pretty && !items.empty()) out.push_back('\n');
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (i != 0U) {
        out.push_back(',');
        if (pretty) out.push_back('\n');
      }
      if (pretty) append_json_indent(out, (depth + 1) * 2);
      append_json_value(out, items[i], pretty, depth + 1);
    }
    if (pretty && !items.empty()) {
      out.push_back('\n');
      append_json_indent(out, depth * 2);
    }
    out.push_back(']');
    return;
  }
  case NativeValue::Tag::Set: {
    const auto &items = as_set(value).items;
    out.push_back('[');
    if (pretty && !items.empty()) out.push_back('\n');
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (i != 0U) {
        out.push_back(',');
        if (pretty) out.push_back('\n');
      }
      if (pretty) append_json_indent(out, (depth + 1) * 2);
      append_json_value(out, items[i], pretty, depth + 1);
    }
    if (pretty && !items.empty()) {
      out.push_back('\n');
      append_json_indent(out, depth * 2);
    }
    out.push_back(']');
    return;
  }
  case NativeValue::Tag::Map: {
    const auto &entries = as_map(value).entries;
    out.push_back('{');
    if (pretty && !entries.empty()) out.push_back('\n');
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (i != 0U) {
        out.push_back(',');
        if (pretty) out.push_back('\n');
      }
      if (pretty) append_json_indent(out, (depth + 1) * 2);
      append_json_escaped(out, native_key_text(entries[i].first));
      out.push_back(':');
      if (pretty) out.push_back(' ');
      append_json_value(out, entries[i].second, pretty, depth + 1);
    }
    if (pretty && !entries.empty()) {
      out.push_back('\n');
      append_json_indent(out, depth * 2);
    }
    out.push_back('}');
    return;
  }
  default:
    throw NativeBailout();
  }
}

static NativeValue native_json_generate_value(const NativeValue &value, bool pretty) {
  std::string out;
  append_json_value(out, value, pretty, 0);
  return NativeValue::heap_string(out);
}

class NativeJsonParser {
public:
  explicit NativeJsonParser(const std::string &text) : text_(text) {}

  NativeValue parse_document() {
    skip_ws();
    NativeValue value = parse_value();
    skip_ws();
    if (pos_ != text_.size()) throw NativeBailout();
    return value;
  }

private:
  const std::string &text_;
  std::size_t pos_ = 0;

  void skip_ws() {
    while (pos_ < text_.size() &&
           std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }

  bool consume(char expected) {
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  void expect(char expected) {
    if (!consume(expected)) throw NativeBailout();
  }

  NativeValue parse_value() {
    skip_ws();
    if (pos_ >= text_.size()) throw NativeBailout();
    const char ch = text_[pos_];
    if (ch == '"') return NativeValue::heap_string(parse_string());
    if (ch == '{') return parse_object();
    if (ch == '[') return parse_array();
    if (ch == 't' && text_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      return NativeValue::boolean(true);
    }
    if (ch == 'f' && text_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      return NativeValue::boolean(false);
    }
    if (ch == 'n' && text_.compare(pos_, 4, "null") == 0) {
      pos_ += 4;
      return NativeValue::nullv();
    }
    return parse_number();
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (pos_ < text_.size()) {
      const char ch = text_[pos_++];
      if (ch == '"') return out;
      if (ch == '\\') {
        if (pos_ >= text_.size()) throw NativeBailout();
        const char esc = text_[pos_++];
        switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: throw NativeBailout();
        }
      } else {
        out.push_back(ch);
      }
    }
    throw NativeBailout();
  }

  NativeValue parse_array() {
    expect('[');
    std::vector<NativeValue> items;
    skip_ws();
    if (consume(']')) return NativeValue::list(std::move(items));
    while (true) {
      items.push_back(parse_value());
      skip_ws();
      if (consume(']')) break;
      expect(',');
    }
    return NativeValue::list(std::move(items));
  }

  NativeValue parse_object() {
    expect('{');
    std::vector<std::pair<NativeValue, NativeValue>> entries;
    skip_ws();
    if (consume('}')) return NativeValue::map_entries(std::move(entries));
    while (true) {
      skip_ws();
      NativeValue key =
          NativeValue::heap_string(parse_string());
      expect(':');
      native_map_store(entries, std::move(key), parse_value(), false);
      skip_ws();
      if (consume('}')) break;
      expect(',');
    }
    return NativeValue::map_entries(std::move(entries));
  }

  NativeValue parse_number() {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size() ||
        !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
      throw NativeBailout();
    }
    while (pos_ < text_.size() &&
           std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
    bool floating = false;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      floating = true;
      ++pos_;
      while (pos_ < text_.size() &&
             std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      floating = true;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      while (pos_ < text_.size() &&
             std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
    }
    if (!floating) {
      const bool negative = text_[start] == '-';
      const std::size_t digits_begin = negative ? start + 1U : start;
      constexpr std::uint64_t kInt64Max =
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
      const std::uint64_t limit = negative ? kInt64Max + 1U : kInt64Max;
      std::uint64_t magnitude = 0;
      for (std::size_t i = digits_begin; i < pos_; ++i) {
        const std::uint64_t digit =
            static_cast<std::uint64_t>(text_[i] - '0');
        if (magnitude > (limit - digit) / 10U) throw NativeBailout();
        magnitude = magnitude * 10U + digit;
      }
      if (negative) {
        if (magnitude == limit) {
          return NativeValue::integer(std::numeric_limits<std::int64_t>::min());
        }
        return NativeValue::integer(-static_cast<std::int64_t>(magnitude));
      }
      return NativeValue::integer(static_cast<std::int64_t>(magnitude));
    }
    try {
      return NativeValue::floating(std::stod(text_.substr(start, pos_ - start)));
    } catch (const std::exception &) {
      throw NativeBailout();
    }
  }
};

static NativeValue native_json_parse_value(const NativeValue &value) {
  if (!native_value_is_string(value)) throw NativeBailout();
  NativeJsonParser parser(native_string_text(value));
  return parser.parse_document();
}
)AMBERCPP";
  out << R"AMBERCPP(
// --- Yaml -----------------------------------------------------------------
// A native mirror of runtime/stdlib_yaml.cpp. Every parse/generate decision
// matches the VM lane so both backends emit byte-identical results; anything
// this subset cannot represent throws NativeBailout so the VM lane produces
// the real YamlParseError / YamlGenerateError.

static void native_yaml_encode_utf8(std::uint32_t cp, std::string &out) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

static std::string native_yaml_rstrip(const std::string &text) {
  std::size_t end = text.size();
  while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
  return text.substr(0, end);
}

static std::string native_yaml_strip(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
  return text.substr(begin, end - begin);
}

static bool native_yaml_matches_integer(const std::string &s) {
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  if (i >= s.size()) return false;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

static bool native_yaml_matches_float(const std::string &s) {
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  std::size_t digits_before = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++digits_before; }
  bool has_dot = false;
  std::size_t digits_after = 0;
  if (i < s.size() && s[i] == '.') {
    has_dot = true;
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++digits_after; }
  }
  bool has_exp = false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    has_exp = true;
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    std::size_t exp_digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++exp_digits; }
    if (exp_digits == 0) return false;
  }
  if (i != s.size()) return false;
  if (!has_dot && !has_exp) return false;
  if (digits_before == 0 && digits_after == 0) return false;
  return true;
}

static NativeValue native_yaml_integer_or_float(const std::string &s) {
  const bool negative = s[0] == '-';
  const std::size_t digits_begin = (s[0] == '-' || s[0] == '+') ? 1U : 0U;
  constexpr std::uint64_t kInt64Max =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  const std::uint64_t limit = negative ? kInt64Max + 1U : kInt64Max;
  std::uint64_t magnitude = 0;
  bool overflow = false;
  for (std::size_t i = digits_begin; i < s.size(); ++i) {
    const std::uint64_t digit = static_cast<std::uint64_t>(s[i] - '0');
    if (magnitude > (limit - digit) / 10U) { overflow = true; break; }
    magnitude = magnitude * 10U + digit;
  }
  if (!overflow) {
    if (negative) {
      if (magnitude == limit) {
        return NativeValue::integer(std::numeric_limits<std::int64_t>::min());
      }
      return NativeValue::integer(-static_cast<std::int64_t>(magnitude));
    }
    return NativeValue::integer(static_cast<std::int64_t>(magnitude));
  }
  return NativeValue::floating(std::strtod(s.c_str(), nullptr));
}

static NativeValue native_yaml_classify_plain(const std::string &raw) {
  const std::string s = native_yaml_strip(raw);
  if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL") {
    return NativeValue::nullv();
  }
  if (s == "true" || s == "True" || s == "TRUE") return NativeValue::boolean(true);
  if (s == "false" || s == "False" || s == "FALSE") {
    return NativeValue::boolean(false);
  }
  if (native_yaml_matches_integer(s)) return native_yaml_integer_or_float(s);
  if (native_yaml_matches_float(s)) {
    return NativeValue::floating(std::strtod(s.c_str(), nullptr));
  }
  return NativeValue::heap_string(s);
}

struct NativeYamlLine {
  std::size_t indent = 0;
  std::string content;
};

struct NativeYamlParser {
  explicit NativeYamlParser(const std::string &src_in) : src(src_in) {}

  const std::string &src;
  std::vector<NativeYamlLine> lines;
  std::size_t cursor = 0;

  static std::size_t comment_index(const std::string &s) {
    std::size_t i = 0;
    while (i < s.size()) {
      const char c = s[i];
      if (c == '"') {
        ++i;
        while (i < s.size() && s[i] != '"') {
          if (s[i] == '\\' && i + 1 < s.size()) ++i;
          ++i;
        }
        if (i < s.size()) ++i;
        continue;
      }
      if (c == '\'') {
        ++i;
        while (i < s.size()) {
          if (s[i] == '\'') {
            if (i + 1 < s.size() && s[i + 1] == '\'') { i += 2; continue; }
            ++i;
            break;
          }
          ++i;
        }
        continue;
      }
      if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) return i;
      ++i;
    }
    return std::string::npos;
  }

  static std::string without_comment(const std::string &s) {
    const std::size_t idx = comment_index(s);
    if (idx == std::string::npos) return s;
    return s.substr(0, idx);
  }

  void tokenize() {
    std::size_t line_start = 0;
    bool seen_content = false;
    while (line_start <= src.size()) {
      std::size_t line_end = src.find('\n', line_start);
      const bool last = line_end == std::string::npos;
      if (last) line_end = src.size();
      std::string raw = src.substr(line_start, line_end - line_start);
      if (!raw.empty() && raw.back() == '\r') raw.pop_back();
      std::size_t indent = 0;
      while (indent < raw.size() && raw[indent] == ' ') ++indent;
      if (indent < raw.size() && raw[indent] == '\t') throw NativeBailout();
      std::string content = native_yaml_rstrip(raw.substr(indent));
      const bool blank = content.empty();
      const bool comment_only = !content.empty() && content[0] == '#';
      if (content == "---") {
        if (seen_content) throw NativeBailout();
      } else if (content == "...") {
        break;
      } else if (!blank && !comment_only) {
        NativeYamlLine line;
        line.indent = indent;
        line.content = content;
        lines.push_back(std::move(line));
        seen_content = true;
      }
      if (last) break;
      line_start = line_end + 1;
    }
  }

  static bool is_dash(const std::string &c) {
    return c == "-" || (c.size() >= 2 && c[0] == '-' && c[1] == ' ');
  }

  static std::size_t key_colon(const std::string &s) {
    int depth = 0;
    std::size_t i = 0;
    while (i < s.size()) {
      const char c = s[i];
      if (c == '"') {
        ++i;
        while (i < s.size() && s[i] != '"') {
          if (s[i] == '\\' && i + 1 < s.size()) ++i;
          ++i;
        }
        if (i < s.size()) ++i;
        continue;
      }
      if (c == '\'') {
        ++i;
        while (i < s.size()) {
          if (s[i] == '\'') {
            if (i + 1 < s.size() && s[i + 1] == '\'') { i += 2; continue; }
            ++i;
            break;
          }
          ++i;
        }
        continue;
      }
      if (c == '[' || c == '{') {
        ++depth;
      } else if (c == ']' || c == '}') {
        if (depth > 0) --depth;
      } else if (c == '#' && depth == 0 &&
                 (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
        return std::string::npos;
      } else if (c == ':' && depth == 0 &&
                 (i + 1 == s.size() || s[i + 1] == ' ' || s[i + 1] == '\t')) {
        return i;
      }
      ++i;
    }
    return std::string::npos;
  }

  NativeValue parse_node(std::size_t indent, int depth) {
    if (depth > 256) throw NativeBailout();
    if (cursor >= lines.size()) return NativeValue::nullv();
    const std::string &c = lines[cursor].content;
    if (!c.empty() && (c[0] == '[' || c[0] == '{')) {
      NativeValue value = parse_flow_line(c);
      ++cursor;
      return value;
    }
    if (is_dash(c)) return parse_sequence(indent, depth);
    if (key_colon(c) != std::string::npos) return parse_mapping(indent, depth);
    NativeValue value = parse_scalar(c);
    ++cursor;
    return value;
  }

  NativeValue parse_mapping(std::size_t indent, int depth) {
    std::vector<std::pair<NativeValue, NativeValue>> entries;
    while (cursor < lines.size() && lines[cursor].indent == indent) {
      const std::string content = lines[cursor].content;
      if (is_dash(content)) break;
      const std::size_t colon = key_colon(content);
      if (colon == std::string::npos) throw NativeBailout();
      const std::string key = parse_key(content.substr(0, colon));
      const std::string rest =
          native_yaml_strip(without_comment(content.substr(colon + 1)));
      ++cursor;
      NativeValue value;
      if (rest.empty()) {
        if (cursor < lines.size() && lines[cursor].indent > indent) {
          value = parse_node(lines[cursor].indent, depth + 1);
        } else if (cursor < lines.size() && lines[cursor].indent == indent &&
                   is_dash(lines[cursor].content)) {
          value = parse_sequence(indent, depth + 1);
        } else {
          value = NativeValue::nullv();
        }
      } else {
        value = parse_scalar(rest);
      }
      native_map_store(entries, NativeValue::heap_string(key), value, false);
    }
    return NativeValue::map_entries(std::move(entries), false);
  }

  NativeValue parse_sequence(std::size_t indent, int depth) {
    std::vector<NativeValue> items;
    while (cursor < lines.size() && lines[cursor].indent == indent &&
           is_dash(lines[cursor].content)) {
      const std::string content = lines[cursor].content;
      std::size_t pos = 1;
      while (pos < content.size() && content[pos] == ' ') ++pos;
      const std::string after = content.substr(pos);
      const std::size_t after_col = indent + pos;
      const std::string after_stripped =
          native_yaml_strip(without_comment(after));
      if (after_stripped.empty()) {
        ++cursor;
        if (cursor < lines.size() && lines[cursor].indent > indent) {
          items.push_back(parse_node(lines[cursor].indent, depth + 1));
        } else {
          items.push_back(NativeValue::nullv());
        }
      } else {
        lines[cursor].indent = after_col;
        lines[cursor].content = after;
        items.push_back(parse_node(after_col, depth + 1));
      }
    }
    return NativeValue::list(std::move(items));
  }

  std::string parse_key(const std::string &raw) {
    const std::string s = native_yaml_strip(raw);
    if (!s.empty() && s[0] == '"') {
      std::size_t consumed = 0;
      return parse_double_quoted(s, &consumed);
    }
    if (!s.empty() && s[0] == '\'') {
      std::size_t consumed = 0;
      return parse_single_quoted(s, &consumed);
    }
    return s;
  }

  NativeValue parse_scalar(const std::string &raw) {
    const std::string s = native_yaml_strip(raw);
    if (s.empty()) return NativeValue::nullv();
    if (s[0] == '[' || s[0] == '{') return parse_flow_line(s);
    if (s[0] == '"') {
      std::size_t consumed = 0;
      return NativeValue::heap_string(parse_double_quoted(s, &consumed));
    }
    if (s[0] == '\'') {
      std::size_t consumed = 0;
      return NativeValue::heap_string(parse_single_quoted(s, &consumed));
    }
    return native_yaml_classify_plain(without_comment(s));
  }

  std::string parse_double_quoted(const std::string &s, std::size_t *consumed) {
    std::string out;
    std::size_t i = 1;
    while (i < s.size()) {
      const char c = s[i];
      if (c == '"') { *consumed = i + 1; return out; }
      if (c == '\\') {
        ++i;
        if (i >= s.size()) throw NativeBailout();
        const char e = s[i];
        switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case '0': out.push_back('\0'); break;
        case 'a': out.push_back('\a'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'v': out.push_back('\v'); break;
        case 'e': out.push_back('\x1b'); break;
        case ' ': out.push_back(' '); break;
        case 'u': { std::uint32_t cp = 0; parse_hex(s, &i, 4, &cp); native_yaml_encode_utf8(cp, out); break; }
        case 'x': { std::uint32_t cp = 0; parse_hex(s, &i, 2, &cp); native_yaml_encode_utf8(cp, out); break; }
        default: throw NativeBailout();
        }
        ++i;
        continue;
      }
      out.push_back(c);
      ++i;
    }
    throw NativeBailout();
  }

  void parse_hex(const std::string &s, std::size_t *i, int count,
                 std::uint32_t *out) {
    std::uint32_t value = 0;
    for (int n = 0; n < count; ++n) {
      ++(*i);
      if (*i >= s.size()) throw NativeBailout();
      const char c = s[*i];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
      else throw NativeBailout();
    }
    *out = value;
  }

  std::string parse_single_quoted(const std::string &s, std::size_t *consumed) {
    std::string out;
    std::size_t i = 1;
    while (i < s.size()) {
      const char c = s[i];
      if (c == '\'') {
        if (i + 1 < s.size() && s[i + 1] == '\'') { out.push_back('\''); i += 2; continue; }
        *consumed = i + 1;
        return out;
      }
      out.push_back(c);
      ++i;
    }
    throw NativeBailout();
  }

  NativeValue parse_flow_line(const std::string &s) {
    std::size_t pos = 0;
    NativeValue value = parse_flow_node(s, &pos, 0);
    skip_spaces(s, &pos);
    if (pos < s.size() && s[pos] != '#') throw NativeBailout();
    return value;
  }

  static void skip_spaces(const std::string &s, std::size_t *pos) {
    while (*pos < s.size() && (s[*pos] == ' ' || s[*pos] == '\t')) ++(*pos);
  }

  NativeValue parse_flow_node(const std::string &s, std::size_t *pos, int depth) {
    if (depth > 256) throw NativeBailout();
    skip_spaces(s, pos);
    if (*pos >= s.size()) throw NativeBailout();
    const char c = s[*pos];
    if (c == '[') return parse_flow_seq(s, pos, depth);
    if (c == '{') return parse_flow_map(s, pos, depth);
    return parse_flow_scalar(s, pos, false);
  }

  NativeValue parse_flow_seq(const std::string &s, std::size_t *pos, int depth) {
    ++(*pos);
    std::vector<NativeValue> items;
    skip_spaces(s, pos);
    if (*pos < s.size() && s[*pos] == ']') { ++(*pos); return NativeValue::list(std::move(items)); }
    while (true) {
      items.push_back(parse_flow_node(s, pos, depth + 1));
      skip_spaces(s, pos);
      if (*pos >= s.size()) throw NativeBailout();
      if (s[*pos] == ',') {
        ++(*pos);
        skip_spaces(s, pos);
        if (*pos < s.size() && s[*pos] == ']') { ++(*pos); return NativeValue::list(std::move(items)); }
        continue;
      }
      if (s[*pos] == ']') { ++(*pos); return NativeValue::list(std::move(items)); }
      throw NativeBailout();
    }
  }

  NativeValue parse_flow_map(const std::string &s, std::size_t *pos, int depth) {
    ++(*pos);
    std::vector<std::pair<NativeValue, NativeValue>> entries;
    skip_spaces(s, pos);
    if (*pos < s.size() && s[*pos] == '}') { ++(*pos); return NativeValue::map_entries(std::move(entries), false); }
    while (true) {
      skip_spaces(s, pos);
      NativeValue key_value = parse_flow_scalar(s, pos, true);
      if (!native_value_is_string(key_value)) throw NativeBailout();
      skip_spaces(s, pos);
      if (*pos >= s.size() || s[*pos] != ':') throw NativeBailout();
      ++(*pos);
      NativeValue value = parse_flow_node(s, pos, depth + 1);
      native_map_store(entries, key_value, value, false);
      skip_spaces(s, pos);
      if (*pos >= s.size()) throw NativeBailout();
      if (s[*pos] == ',') {
        ++(*pos);
        skip_spaces(s, pos);
        if (*pos < s.size() && s[*pos] == '}') { ++(*pos); return NativeValue::map_entries(std::move(entries), false); }
        continue;
      }
      if (s[*pos] == '}') { ++(*pos); return NativeValue::map_entries(std::move(entries), false); }
      throw NativeBailout();
    }
  }

  NativeValue parse_flow_scalar(const std::string &s, std::size_t *pos, bool as_key) {
    skip_spaces(s, pos);
    if (*pos >= s.size()) throw NativeBailout();
    if (s[*pos] == '"') {
      std::size_t consumed = 0;
      std::string text = parse_double_quoted(s.substr(*pos), &consumed);
      *pos += consumed;
      return NativeValue::heap_string(text);
    }
    if (s[*pos] == '\'') {
      std::size_t consumed = 0;
      std::string text = parse_single_quoted(s.substr(*pos), &consumed);
      *pos += consumed;
      return NativeValue::heap_string(text);
    }
    const std::size_t begin = *pos;
    while (*pos < s.size()) {
      const char c = s[*pos];
      if (c == ',' || c == ']' || c == '}') break;
      if (as_key && c == ':') break;
      ++(*pos);
    }
    const std::string token = native_yaml_strip(s.substr(begin, *pos - begin));
    if (as_key) return NativeValue::heap_string(token);
    return native_yaml_classify_plain(token);
  }

  NativeValue parse_document() {
    tokenize();
    if (lines.empty()) return NativeValue::nullv();
    NativeValue value = parse_node(lines.front().indent, 0);
    if (cursor != lines.size()) throw NativeBailout();
    return value;
  }
};

static NativeValue native_yaml_parse_value(const NativeValue &value) {
  if (!native_value_is_string(value)) throw NativeBailout();
  NativeYamlParser parser(native_string_text(value));
  return parser.parse_document();
}

static std::string native_yaml_format_double(double d) {
  if (std::isnan(d) || std::isinf(d)) throw NativeBailout();
  char buf[40];
  std::string result;
  for (int precision = 1; precision <= 17; ++precision) {
    std::snprintf(buf, sizeof(buf), "%.*g", precision, d);
    if (std::strtod(buf, nullptr) == d) { result.assign(buf); break; }
    if (precision == 17) result.assign(buf);
  }
  if (result.find_first_of(".eEnN") == std::string::npos) result += ".0";
  return result;
}

static bool native_yaml_needs_quote(const std::string &s) {
  if (s.empty()) return true;
  if (s == "~" || s == "null" || s == "Null" || s == "NULL" || s == "true" ||
      s == "True" || s == "TRUE" || s == "false" || s == "False" ||
      s == "FALSE") {
    return true;
  }
  if (native_yaml_matches_integer(s) || native_yaml_matches_float(s)) return true;
  if (s.front() == ' ' || s.back() == ' ' || s.front() == '\t' || s.back() == '\t') {
    return true;
  }
  switch (s.front()) {
  case '!': case '&': case '*': case '[': case ']': case '{': case '}':
  case ',': case '#': case '|': case '>': case '@': case '`': case '"':
  case '\'': case '%': case '?': case ':':
    return true;
  default:
    break;
  }
  if (s.front() == '-' && (s.size() == 1 || s[1] == ' ')) return true;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20) return true;
    if (c == ':' && (i + 1 == s.size() || s[i + 1] == ' ')) return true;
    if (c == '#' && i > 0 && s[i - 1] == ' ') return true;
  }
  return false;
}

static void native_yaml_write_quoted(const std::string &text, std::string &out) {
  out.push_back('"');
  for (const unsigned char c : text) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\t': out += "\\t"; break;
    case '\r': out += "\\r"; break;
    default:
      if (c < 0x20) {
        char esc[8];
        std::snprintf(esc, sizeof(esc), "\\u%04x", c);
        out += esc;
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
  }
  out.push_back('"');
}

static bool native_yaml_nonempty_map(const NativeValue &v) {
  return v.tag == NativeValue::Tag::Map && !as_map(v).entries.empty();
}

static bool native_yaml_nonempty_list(const NativeValue &v) {
  return v.tag == NativeValue::Tag::List && !as_list(v).items.empty();
}

static std::string native_yaml_value_text(const NativeValue &v) {
  if (v.tag == NativeValue::Tag::String || v.tag == NativeValue::Tag::HeapString) {
    return native_string_text(v);
  }
  if (v.tag == NativeValue::Tag::Symbol) return native_symbol_text(v.scalar_value);
  throw NativeBailout();
}

static std::string native_yaml_scalar_text(const NativeValue &v) {
  switch (v.tag) {
  case NativeValue::Tag::Null:
    return "null";
  case NativeValue::Tag::Bool:
    return v.scalar_value != 0 ? "true" : "false";
  case NativeValue::Tag::Integer:
    return std::to_string(v.scalar_value);
  case NativeValue::Tag::Float:
    return native_yaml_format_double(v.float_value);
  case NativeValue::Tag::String:
  case NativeValue::Tag::HeapString:
  case NativeValue::Tag::Symbol: {
    const std::string s = native_yaml_value_text(v);
    if (native_yaml_needs_quote(s)) {
      std::string quoted;
      native_yaml_write_quoted(s, quoted);
      return quoted;
    }
    return s;
  }
  default:
    throw NativeBailout();
  }
}

static void native_yaml_emit_map(const NativeValue &v, int depth, std::string &out);
static void native_yaml_emit_seq(const NativeValue &v, int depth, std::string &out);

static void native_yaml_emit_inline(const NativeValue &v, std::string &out) {
  if (v.tag == NativeValue::Tag::Map) { out += "{}"; return; }
  if (v.tag == NativeValue::Tag::List) { out += "[]"; return; }
  out += native_yaml_scalar_text(v);
}

static void native_yaml_emit_key(const NativeValue &key, std::string &out) {
  if (key.tag != NativeValue::Tag::String &&
      key.tag != NativeValue::Tag::HeapString &&
      key.tag != NativeValue::Tag::Symbol) {
    throw NativeBailout();
  }
  const std::string s = native_yaml_value_text(key);
  if (native_yaml_needs_quote(s)) native_yaml_write_quoted(s, out);
  else out += s;
}

static void native_yaml_emit_map(const NativeValue &v, int depth, std::string &out) {
  const auto &entries = as_map(v).entries;
  for (const auto &entry : entries) {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    native_yaml_emit_key(entry.first, out);
    out.push_back(':');
    if (native_yaml_nonempty_map(entry.second)) {
      out.push_back('\n');
      native_yaml_emit_map(entry.second, depth + 1, out);
    } else if (native_yaml_nonempty_list(entry.second)) {
      out.push_back('\n');
      native_yaml_emit_seq(entry.second, depth + 1, out);
    } else {
      out.push_back(' ');
      native_yaml_emit_inline(entry.second, out);
      out.push_back('\n');
    }
  }
}

static void native_yaml_emit_seq(const NativeValue &v, int depth, std::string &out) {
  const auto &items = as_list(v).items;
  for (const auto &item : items) {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    out.push_back('-');
    if (native_yaml_nonempty_map(item)) {
      out.push_back('\n');
      native_yaml_emit_map(item, depth + 1, out);
    } else if (native_yaml_nonempty_list(item)) {
      out.push_back('\n');
      native_yaml_emit_seq(item, depth + 1, out);
    } else {
      out.push_back(' ');
      native_yaml_emit_inline(item, out);
      out.push_back('\n');
    }
  }
}

static NativeValue native_yaml_generate_value(const NativeValue &value) {
  std::string out;
  if (native_yaml_nonempty_map(value)) {
    native_yaml_emit_map(value, 0, out);
  } else if (native_yaml_nonempty_list(value)) {
    native_yaml_emit_seq(value, 0, out);
  } else if (value.tag == NativeValue::Tag::Map) {
    out += "{}\n";
  } else if (value.tag == NativeValue::Tag::List) {
    out += "[]\n";
  } else {
    out += native_yaml_scalar_text(value);
    out.push_back('\n');
  }
  return NativeValue::heap_string(out);
}
)AMBERCPP";
  out << R"AMBERCPP(
struct NativeCivilDate {
  std::int64_t year = 1970;
  int month = 1;
  int day = 1;
};

struct NativeUtcFields {
  NativeCivilDate date;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int nanosecond = 0;
};

static constexpr std::int64_t kNativeNanosPerSecond = 1000000000LL;
static constexpr std::int64_t kNativeSecondsPerDay = 86400LL;

static std::int64_t native_checked_i128(__int128 value) {
  if (value < static_cast<__int128>(std::numeric_limits<std::int64_t>::min()) ||
      value > static_cast<__int128>(std::numeric_limits<std::int64_t>::max())) {
    throw NativeBailout();
  }
  return static_cast<std::int64_t>(value);
}

static std::int64_t native_floor_div_i64(std::int64_t a, std::int64_t b) {
  std::int64_t q = a / b;
  const std::int64_t r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) --q;
  return q;
}

static __int128 native_floor_div_i128(__int128 a, __int128 b) {
  __int128 q = a / b;
  const __int128 r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) --q;
  return q;
}

static int native_floor_mod_i64(std::int64_t a, std::int64_t b) {
  return static_cast<int>(a - native_floor_div_i64(a, b) * b);
}

static std::int64_t native_days_from_civil(std::int64_t y, int m, int d) {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned mp = static_cast<unsigned>(m + (m > 2 ? -3 : 9));
  const unsigned doy = (153U * mp + 2U) / 5U + static_cast<unsigned>(d) - 1U;
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

static NativeCivilDate native_civil_from_days(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe =
      (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  const unsigned mp = (5U * doy + 2U) / 153U;
  const unsigned d = doy - (153U * mp + 2U) / 5U + 1U;
  const int m = static_cast<int>(mp) + (mp < 10U ? 3 : -9);
  y += m <= 2 ? 1 : 0;
  return NativeCivilDate{y, m, static_cast<int>(d)};
}

static bool native_leap_year(std::int64_t year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int native_days_in_month(std::int64_t year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  if (month == 2 && native_leap_year(year)) return 29;
  return kDays[month - 1];
}

static bool native_valid_date(std::int64_t year, int month, int day) {
  return month >= 1 && month <= 12 && day >= 1 &&
         day <= native_days_in_month(year, month);
}

static NativeTime native_make_time(__int128 seconds, __int128 nanosecond) {
  const __int128 delta =
      native_floor_div_i128(nanosecond, kNativeNanosPerSecond);
  const __int128 nanos =
      nanosecond - delta * static_cast<__int128>(kNativeNanosPerSecond);
  return NativeTime{native_checked_i128(seconds + delta),
                    static_cast<std::uint32_t>(nanos)};
}

static NativeUtcFields native_utc_fields(const NativeTime &time) {
  static thread_local bool cached = false;
  static thread_local std::int64_t cached_epoch_seconds = 0;
  static thread_local std::uint32_t cached_nanosecond = 0;
  static thread_local NativeUtcFields cached_fields;
  if (cached && cached_epoch_seconds == time.epoch_seconds &&
      cached_nanosecond == time.nanosecond) {
    return cached_fields;
  }
  const std::int64_t days =
      native_floor_div_i64(time.epoch_seconds, kNativeSecondsPerDay);
  const int second_of_day =
      native_floor_mod_i64(time.epoch_seconds, kNativeSecondsPerDay);
  NativeUtcFields out;
  out.date = native_civil_from_days(days);
  out.hour = second_of_day / 3600;
  out.minute = (second_of_day / 60) % 60;
  out.second = second_of_day % 60;
  out.nanosecond = static_cast<int>(time.nanosecond);
  cached = true;
  cached_epoch_seconds = time.epoch_seconds;
  cached_nanosecond = time.nanosecond;
  cached_fields = out;
  return out;
}

static NativeTime native_time_from_utc_fields(std::int64_t year, int month,
                                              int day, int hour, int minute,
                                              int second, int nanosecond) {
  if (!native_valid_date(year, month, day) || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 59 ||
      nanosecond < 0 || nanosecond >= kNativeNanosPerSecond) {
    throw NativeBailout();
  }
  const __int128 days = native_days_from_civil(year, month, day);
  return native_make_time(days * kNativeSecondsPerDay + hour * 3600 +
                              minute * 60 + second,
                          nanosecond);
}

)AMBERCPP";
  out << R"AMBERCPP(static NativeTime native_apply_period_to_time(const NativeTime &time,
                                              const NativeTimePeriod &period) {
  NativeUtcFields fields = native_utc_fields(time);
  if (period.months != 0) {
    const __int128 total_month =
        static_cast<__int128>(fields.date.year) * 12 +
        (fields.date.month - 1) + period.months;
    const __int128 year = native_floor_div_i128(total_month, 12);
    const int month = static_cast<int>(total_month - year * 12) + 1;
    fields.date.year = native_checked_i128(year);
    fields.date.month = month;
    fields.date.day =
        std::min(fields.date.day, native_days_in_month(fields.date.year, month));
  }
  const __int128 base_days =
      static_cast<__int128>(native_days_from_civil(
          fields.date.year, fields.date.month, fields.date.day)) +
      period.days;
  return native_make_time(base_days * kNativeSecondsPerDay +
                              fields.hour * 3600 + fields.minute * 60 +
                              fields.second,
                          static_cast<__int128>(fields.nanosecond) +
                              period.nanoseconds);
}

static NativeTimePeriod native_period_add(const NativeTimePeriod &lhs,
                                          const NativeTimePeriod &rhs,
                                          int sign = 1) {
  return NativeTimePeriod{
      native_checked_i128(static_cast<__int128>(lhs.months) +
                          sign * static_cast<__int128>(rhs.months)),
      native_checked_i128(static_cast<__int128>(lhs.days) +
                          sign * static_cast<__int128>(rhs.days)),
      native_checked_i128(static_cast<__int128>(lhs.nanoseconds) +
                          sign * static_cast<__int128>(rhs.nanoseconds))};
}

static NativeTimePeriod native_period_between(const NativeTime &lhs,
                                              const NativeTime &rhs) {
  const __int128 left = static_cast<__int128>(lhs.epoch_seconds) *
                            kNativeNanosPerSecond +
                        lhs.nanosecond;
  const __int128 right = static_cast<__int128>(rhs.epoch_seconds) *
                             kNativeNanosPerSecond +
                         rhs.nanosecond;
  return NativeTimePeriod{0, 0, native_checked_i128(left - right)};
}

static __int128 native_fixed_period_nanoseconds(
    const NativeTimePeriod &period) {
  if (period.months != 0) throw NativeBailout();
  return static_cast<__int128>(period.days) * kNativeSecondsPerDay *
             kNativeNanosPerSecond +
         period.nanoseconds;
}

static int native_compare_time(const NativeTime &lhs, const NativeTime &rhs) {
  if (lhs.epoch_seconds != rhs.epoch_seconds) {
    return lhs.epoch_seconds < rhs.epoch_seconds ? -1 : 1;
  }
  if (lhs.nanosecond != rhs.nanosecond) {
    return lhs.nanosecond < rhs.nanosecond ? -1 : 1;
  }
  return 0;
}

static int native_compare_i128(__int128 lhs, __int128 rhs) {
  if (lhs < rhs) return -1;
  if (lhs > rhs) return 1;
  return 0;
}

static NativeValue native_time_from_unix(const NativeValue &module,
                                         const NativeValue &seconds_value,
                                         const NativeValue &nanosecond_value,
                                         bool has_nanosecond) {
  if (module.tag != NativeValue::Tag::TimeModule) throw NativeBailout();
  const std::int64_t seconds = as_int(seconds_value);
  std::int64_t nanosecond = 0;
  if (has_nanosecond) {
    nanosecond = as_int(nanosecond_value);
  }
  if (nanosecond < 0 || nanosecond >= kNativeNanosPerSecond) {
    throw NativeBailout();
  }
  return NativeValue::time(
      NativeTime{seconds, static_cast<std::uint32_t>(nanosecond)});
}

static NativeValue native_time_from_unix_ms(const NativeValue &module,
                                            const NativeValue &milliseconds) {
  if (module.tag != NativeValue::Tag::TimeModule) throw NativeBailout();
  const std::int64_t ms = as_int(milliseconds);
  const std::int64_t seconds = native_floor_div_i64(ms, 1000);
  const int millis = native_floor_mod_i64(ms, 1000);
  return NativeValue::time(
      NativeTime{seconds, static_cast<std::uint32_t>(millis * 1000000)});
}

static NativeValue native_time_from_unix_ns(const NativeValue &module,
                                            const NativeValue &nanoseconds) {
  if (module.tag != NativeValue::Tag::TimeModule) throw NativeBailout();
  return NativeValue::time(native_make_time(0, as_int(nanoseconds)));
}

static std::int64_t native_optional_int(const NativeValue &value, bool has,
                                        std::int64_t fallback) {
  return has ? as_int(value) : fallback;
}

static NativeValue native_time_utc(
    const NativeValue &module, const NativeValue &year_value,
    const NativeValue &month_value, const NativeValue &day_value,
    const NativeValue &hour_value, bool has_hour,
    const NativeValue &minute_value, bool has_minute,
    const NativeValue &second_value, bool has_second,
    const NativeValue &nanosecond_value, bool has_nanosecond) {
  if (module.tag != NativeValue::Tag::TimeModule) throw NativeBailout();
  const std::int64_t year = as_int(year_value);
  const std::int64_t month = as_int(month_value);
  const std::int64_t day = as_int(day_value);
  return NativeValue::time(native_time_from_utc_fields(
      year, static_cast<int>(month), static_cast<int>(day),
      static_cast<int>(native_optional_int(hour_value, has_hour, 0)),
      static_cast<int>(native_optional_int(minute_value, has_minute, 0)),
      static_cast<int>(native_optional_int(second_value, has_second, 0)),
      static_cast<int>(native_optional_int(nanosecond_value, has_nanosecond,
                                           0))));
}

static bool native_parse_digits(const std::string &text, std::size_t offset,
                                std::size_t count, int *out) {
  if (offset + count > text.size()) return false;
  int value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[offset + i];
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
  }
  *out = value;
  return true;
}

static NativeValue native_time_parse(const NativeValue &module,
                                     const NativeValue &text_value) {
  if (module.tag != NativeValue::Tag::TimeModule ||
      !native_value_is_string(text_value)) {
    throw NativeBailout();
  }
  const std::string &text = native_string_text(text_value);
  std::size_t pos = 0;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!native_parse_digits(text, pos, 4, &year)) throw NativeBailout();
  pos += 4;
  if (pos >= text.size() || text[pos++] != '-' ||
      !native_parse_digits(text, pos, 2, &month)) throw NativeBailout();
  pos += 2;
  if (pos >= text.size() || text[pos++] != '-' ||
      !native_parse_digits(text, pos, 2, &day)) throw NativeBailout();
  pos += 2;
  if (pos >= text.size() || (text[pos] != 'T' && text[pos] != 't')) {
    throw NativeBailout();
  }
  ++pos;
  if (!native_parse_digits(text, pos, 2, &hour)) throw NativeBailout();
  pos += 2;
  if (pos >= text.size() || text[pos++] != ':' ||
      !native_parse_digits(text, pos, 2, &minute)) throw NativeBailout();
  pos += 2;
  if (pos >= text.size() || text[pos++] != ':' ||
      !native_parse_digits(text, pos, 2, &second)) throw NativeBailout();
  pos += 2;
  int nanosecond = 0;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    int digits = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      if (digits < 9) nanosecond = nanosecond * 10 + (text[pos] - '0');
      ++digits;
      ++pos;
    }
    if (digits == 0 || digits > 9) throw NativeBailout();
    for (; digits < 9; ++digits) nanosecond *= 10;
  }
  int offset_seconds = 0;
  if (pos < text.size() && (text[pos] == 'Z' || text[pos] == 'z')) {
    ++pos;
  } else if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
    const int sign = text[pos] == '-' ? -1 : 1;
    ++pos;
    int offset_hour = 0;
    int offset_minute = 0;
    if (!native_parse_digits(text, pos, 2, &offset_hour)) throw NativeBailout();
    pos += 2;
    if (pos >= text.size() || text[pos++] != ':' ||
        !native_parse_digits(text, pos, 2, &offset_minute)) {
      throw NativeBailout();
    }
    pos += 2;
    offset_seconds = sign * (offset_hour * 3600 + offset_minute * 60);
  } else {
    throw NativeBailout();
  }
  if (pos != text.size()) throw NativeBailout();
  NativeTime local =
      native_time_from_utc_fields(year, month, day, hour, minute, second,
                                  nanosecond);
  return NativeValue::time(native_make_time(
      static_cast<__int128>(local.epoch_seconds) - offset_seconds,
      local.nanosecond));
}

static NativeValue native_url_string(const std::string &text) {
  return NativeValue::heap_string(text);
}

static NativeValue native_url_optional_string(bool present,
                                             const std::string &text) {
  return present ? native_url_string(text) : NativeValue::nullv();
}

static NativeValue native_url_query_value(
    const amber::runtime::RuntimeUrlQueryValue &value);

static NativeValue native_url_query_map(
    const std::vector<std::pair<std::string, amber::runtime::RuntimeUrlQueryValue>> &entries) {
  std::vector<std::pair<std::string, NativeValue>> object;
  object.reserve(entries.size());
  for (const auto &entry : entries) {
    object.push_back({entry.first, native_url_query_value(entry.second)});
  }
  return NativeValue::map(std::move(object));
}

static NativeValue native_url_query_value(
    const amber::runtime::RuntimeUrlQueryValue &value) {
  if (value.kind == amber::runtime::RuntimeUrlQueryValue::Kind::String) {
    return native_url_string(value.text);
  }
  if (value.kind == amber::runtime::RuntimeUrlQueryValue::Kind::List) {
    std::vector<NativeValue> items;
    items.reserve(value.list.size());
    for (const amber::runtime::RuntimeUrlQueryValue &item : value.list) {
      items.push_back(native_url_query_value(item));
    }
    return NativeValue::list(std::move(items));
  }
  return native_url_query_map(value.map);
}

static NativeValue native_url_parts_map(const amber::runtime::RuntimeUrlParts &parts) {
  std::vector<std::pair<std::string, NativeValue>> entries;
  entries.reserve(10U);
  entries.push_back({"scheme", native_url_optional_string(!parts.scheme.empty(), parts.scheme)});
  entries.push_back({"authority", native_url_optional_string(parts.has_authority, parts.authority)});
  entries.push_back({"userinfo", native_url_optional_string(!parts.userinfo.empty(), parts.userinfo)});
  entries.push_back({"host", native_url_optional_string(parts.has_authority, parts.host)});
  entries.push_back({"port", parts.has_port ? NativeValue::integer(parts.port) : NativeValue::nullv()});
  entries.push_back({"path", native_url_string(parts.path)});
  entries.push_back({"query", native_url_optional_string(parts.has_query, parts.query)});
  entries.push_back({"fragment", native_url_optional_string(parts.has_fragment, parts.fragment)});
  if (parts.has_query) {
    std::vector<std::pair<std::string, amber::runtime::RuntimeUrlQueryValue>> query;
    std::string error;
    if (!amber::runtime::runtime_url_parse_query(parts.query, &query, &error)) {
      throw NativeBailout();
    }
    entries.push_back({"query_map", native_url_query_map(query)});
  } else {
    entries.push_back({"query_map", NativeValue::nullv()});
  }
  return NativeValue::map(std::move(entries));
}

static const NativeValue *native_map_field(const NativeMap &map,
                                           const std::string &name) {
  for (auto it = map.entries.rbegin(); it != map.entries.rend(); ++it) {
    if (native_map_key_is_nameable(it->first) &&
        native_key_text(it->first) == name) {
      return &it->second;
    }
  }
  return nullptr;
}

static bool native_url_optional_string_field(
    const NativeMap &map, const std::string &name, std::string *out,
    bool *present) {
  const NativeValue *value = native_map_field(map, name);
  if (value == nullptr || value->tag == NativeValue::Tag::Null) {
    *present = false;
    out->clear();
    return true;
  }
  if (!native_value_is_string(*value)) return false;
  *present = true;
  *out = native_string_text(*value);
  return true;
}

static amber::runtime::RuntimeUrlQueryValue
native_url_query_value_from_native(const NativeValue &value);

static std::vector<std::pair<std::string, amber::runtime::RuntimeUrlQueryValue>>
native_url_query_pairs_from_map(const NativeValue &value) {
  std::vector<std::pair<std::string, amber::runtime::RuntimeUrlQueryValue>> out;
  const NativeMap &map = as_map(value);
  out.reserve(map.entries.size());
  for (const auto &entry : map.entries) {
    out.push_back({native_key_text(entry.first),
                   native_url_query_value_from_native(entry.second)});
  }
  return out;
}

static amber::runtime::RuntimeUrlQueryValue
native_url_query_value_from_native(const NativeValue &value) {
  if (native_value_is_string(value)) {
    return amber::runtime::RuntimeUrlQueryValue::string(native_string_text(value));
  }
  if (value.tag == NativeValue::Tag::List) {
    std::vector<amber::runtime::RuntimeUrlQueryValue> items;
    const auto &native_items = as_list(value).items;
    items.reserve(native_items.size());
    for (const NativeValue &item : native_items) {
      items.push_back(native_url_query_value_from_native(item));
    }
    return amber::runtime::RuntimeUrlQueryValue::list_value(std::move(items));
  }
  if (value.tag == NativeValue::Tag::Map) {
    return amber::runtime::RuntimeUrlQueryValue::map_value(
        native_url_query_pairs_from_map(value));
  }
  throw NativeBailout();
}

static amber::runtime::RuntimeUrlParts
native_url_parts_from_map(const NativeValue &value) {
  const NativeMap &map = as_map(value);
  amber::runtime::RuntimeUrlParts parts;
  bool present = false;
  if (!native_url_optional_string_field(map, "scheme", &parts.scheme, &present) ||
      !native_url_optional_string_field(map, "userinfo", &parts.userinfo, &present) ||
      !native_url_optional_string_field(map, "host", &parts.host, &present) ||
      !native_url_optional_string_field(map, "path", &parts.path, &present) ||
      !native_url_optional_string_field(map, "query", &parts.query, &parts.has_query) ||
      !native_url_optional_string_field(map, "fragment", &parts.fragment, &parts.has_fragment)) {
    throw NativeBailout();
  }
  const NativeValue *host = native_map_field(map, "host");
  parts.has_authority = host != nullptr && host->tag != NativeValue::Tag::Null;
  const NativeValue *port = native_map_field(map, "port");
  if (port != nullptr && port->tag != NativeValue::Tag::Null) {
    if (port->tag != NativeValue::Tag::Integer) throw NativeBailout();
    parts.has_port = true;
    parts.port = port->scalar_value;
  }
  const NativeValue *query_map = native_map_field(map, "query_map");
  if ((!parts.has_query || parts.query.empty()) && query_map != nullptr &&
      query_map->tag != NativeValue::Tag::Null) {
    parts.query = amber::runtime::runtime_url_build_query(
        native_url_query_pairs_from_map(*query_map));
    parts.has_query = true;
  }
  std::string error;
  if (!amber::runtime::runtime_url_validate_parts(parts, &error)) {
    throw NativeBailout();
  }
  return parts;
}

static NativeValue native_url_parse(const NativeValue &module,
                                    const NativeValue &text_value) {
  if (module.tag != NativeValue::Tag::UrlModule ||
      !native_value_is_string(text_value)) {
    throw NativeBailout();
  }
  amber::runtime::RuntimeUrlParts parts;
  std::string error;
  if (!amber::runtime::runtime_url_parse(native_string_text(text_value), &parts, &error)) {
    throw NativeBailout();
  }
  return native_url_parts_map(parts);
}

static NativeValue native_url_build(const NativeValue &module,
                                    const NativeValue &parts_value) {
  if (module.tag != NativeValue::Tag::UrlModule) throw NativeBailout();
  return native_url_string(amber::runtime::runtime_url_build(
      native_url_parts_from_map(parts_value)));
}

static NativeValue native_url_percent_encode(const NativeValue &module,
                                             const NativeValue &text_value) {
  if (module.tag != NativeValue::Tag::UrlModule ||
      !native_value_is_string(text_value)) {
    throw NativeBailout();
  }
  return native_url_string(amber::runtime::runtime_url_percent_encode(
      native_string_text(text_value), amber::runtime::RuntimeUrlEncodeMode::Component));
}

static NativeValue native_url_percent_decode(const NativeValue &module,
                                             const NativeValue &text_value) {
  if (module.tag != NativeValue::Tag::UrlModule ||
      !native_value_is_string(text_value)) {
    throw NativeBailout();
  }
  std::string decoded;
  std::string error;
  if (!amber::runtime::runtime_url_percent_decode(
          native_string_text(text_value), false, &decoded, &error)) {
    throw NativeBailout();
  }
  return native_url_string(decoded);
}

static NativeValue native_url_parse_query(const NativeValue &module,
                                          const NativeValue &text_value) {
  if (module.tag != NativeValue::Tag::UrlModule ||
      !native_value_is_string(text_value)) {
    throw NativeBailout();
  }
  std::vector<std::pair<std::string, amber::runtime::RuntimeUrlQueryValue>> entries;
  std::string error;
  if (!amber::runtime::runtime_url_parse_query(
          native_string_text(text_value), &entries, &error)) {
    throw NativeBailout();
  }
  return native_url_query_map(entries);
}

static NativeValue native_url_build_query(const NativeValue &module,
                                          const NativeValue &map_value) {
  if (module.tag != NativeValue::Tag::UrlModule) throw NativeBailout();
  return native_url_string(amber::runtime::runtime_url_build_query(
      native_url_query_pairs_from_map(map_value)));
}

static NativeValue native_parse_value(const NativeValue &module,
                                      const NativeValue &text_value) {
  if (module.tag == NativeValue::Tag::UuidModule) {
    return native_uuid_parse(module, text_value);
  }
  if (module.tag == NativeValue::Tag::TimeModule) {
    return native_time_parse(module, text_value);
  }
  if (module.tag == NativeValue::Tag::UrlModule) {
    return native_url_parse(module, text_value);
  }
  if (module.tag == NativeValue::Tag::YamlModule) {
    return native_yaml_parse_value(text_value);
  }
  return native_json_parse_value(text_value);
}

// Dispatch `Json.generate` / `Yaml.generate` on the receiver module tag; a
// YAML receiver only supports the plain (non-pretty) form, so a pretty request
// bails to the VM (which raises NoMethodError for Yaml.pretty_generate).
static NativeValue native_generate_value(const NativeValue &module,
                                         const NativeValue &value, bool pretty) {
  if (module.tag == NativeValue::Tag::YamlModule) {
    if (pretty) throw NativeBailout();
    return native_yaml_generate_value(value);
  }
  return native_json_generate_value(value, pretty);
}

using NativeArgKwArgs =
    std::initializer_list<std::pair<std::string, NativeValue>>;

static const NativeValue *native_arg_kw(NativeArgKwArgs kwargs,
                                        const std::string &name) {
  for (const auto &entry : kwargs) {
    if (entry.first == name) return &entry.second;
  }
  return nullptr;
}

static void native_arg_reject_unknown(
    NativeArgKwArgs kwargs, std::initializer_list<const char *> allowed) {
  for (const auto &entry : kwargs) {
    bool ok = false;
    for (const char *name : allowed) {
      if (entry.first == name) {
        ok = true;
        break;
      }
    }
    if (!ok) throw NativeBailout();
  }
}

static bool native_arg_starts_with(const std::string &text,
                                   const std::string &prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

static std::string native_arg_lower_ascii(std::string text) {
  for (char &ch : text) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return text;
}

static std::string native_arg_text(const NativeValue &value) {
  if (native_value_is_string(value)) return native_string_text(value);
  if (value.tag == NativeValue::Tag::Symbol) {
    return native_symbol_text(value.scalar_value);
  }
  throw NativeBailout();
}

static std::vector<std::string> native_arg_string_list(
    const NativeValue &value) {
  std::vector<std::string> out;
  const NativeList &list = as_list(value);
  out.reserve(list.items.size());
  for (const NativeValue &item : list.items) {
    out.push_back(native_arg_text(item));
  }
  return out;
}

static std::vector<std::pair<std::string, std::string>>
native_arg_string_map(const NativeValue &value) {
  std::vector<std::pair<std::string, std::string>> out;
  const NativeMap &map = as_map(value);
  out.reserve(map.entries.size());
  for (const auto &entry : map.entries) {
    out.push_back({native_key_text(entry.first),
                   native_arg_text(entry.second)});
  }
  return out;
}

static std::optional<std::string> native_arg_optional_text(
    NativeArgKwArgs kwargs, const std::string &name) {
  const NativeValue *value = native_arg_kw(kwargs, name);
  if (value == nullptr || value->tag == NativeValue::Tag::Null) {
    return std::nullopt;
  }
  return native_arg_text(*value);
}

static bool native_arg_bool_kw(NativeArgKwArgs kwargs,
                               const std::string &name, bool fallback) {
  const NativeValue *value = native_arg_kw(kwargs, name);
  if (value == nullptr) return fallback;
  if (value->tag != NativeValue::Tag::Bool) throw NativeBailout();
  return value->scalar_value != 0;
}

static NativeArgValueType native_arg_type_kw(NativeArgKwArgs kwargs,
                                             NativeArgValueType fallback) {
  const NativeValue *value = native_arg_kw(kwargs, "type");
  if (value == nullptr) return fallback;
  switch (value->tag) {
  case NativeValue::Tag::StrType:
    return NativeArgValueType::Str;
  case NativeValue::Tag::IntType:
    return NativeArgValueType::Int;
  case NativeValue::Tag::FloatType:
    return NativeArgValueType::Float;
  case NativeValue::Tag::BoolType:
    return NativeArgValueType::Bool;
  case NativeValue::Tag::SymbolType:
    return NativeArgValueType::Symbol;
  default:
    throw NativeBailout();
  }
}

static std::string native_arg_derive_name(const std::string &spelling) {
  std::size_t offset = 0;
  while (offset < spelling.size() && spelling[offset] == '-') ++offset;
  std::string name = spelling.substr(offset);
  for (char &ch : name) {
    if (ch == '-') ch = '_';
  }
  return name;
}

static bool native_arg_spelling_valid(const std::string &spelling) {
  if (spelling.size() < 2U || spelling[0] != '-') return false;
  if (spelling.find_first_of(" \t\r\n") != std::string::npos) return false;
  if (native_arg_starts_with(spelling, "--")) {
    return spelling.size() > 2U && spelling[2] != '-';
  }
  return spelling[1] != '-';
}

static bool native_arg_spelling_used(const NativeArgParser &parser,
                                     const std::string &spelling) {
  for (const NativeArgParser::Spec &spec : parser.specs) {
    for (const std::string &existing : spec.spellings) {
      if (existing == spelling) return true;
    }
    if (spec.kind == NativeArgParser::SpecKind::Flag && spec.negatable &&
        native_arg_starts_with(spelling, "--no-")) {
      const std::string positive = "--" + spelling.substr(5);
      for (const std::string &existing : spec.spellings) {
        if (existing == positive) return true;
      }
    }
  }
  return false;
}

static void native_arg_validate_spelling(const NativeArgParser &parser,
                                         const std::string &spelling) {
  if (!native_arg_spelling_valid(spelling)) throw NativeBailout();
  if (native_arg_spelling_used(parser, spelling)) throw NativeBailout();
  if ((spelling == "-h" || spelling == "--help") && parser.add_help) {
    throw NativeBailout();
  }
}

static NativeValue native_argparser_new(const NativeValue &module,
                                        NativeArgKwArgs kwargs) {
  if (module.tag != NativeValue::Tag::ArgParserModule) throw NativeBailout();
  native_arg_reject_unknown(kwargs,
                            {"cmdline", "name", "about", "env", "add_help"});
  NativeArgParser parser;
  if (const NativeValue *cmdline = native_arg_kw(kwargs, "cmdline")) {
    parser.cmdline = native_arg_string_list(*cmdline);
  }
  if (auto name = native_arg_optional_text(kwargs, "name")) {
    parser.name = *name;
  }
  if (auto about = native_arg_optional_text(kwargs, "about")) {
    parser.about = *about;
  }
  if (const NativeValue *env = native_arg_kw(kwargs, "env")) {
    parser.env = native_arg_string_map(*env);
  }
  parser.add_help = native_arg_bool_kw(kwargs, "add_help", parser.add_help);
  return NativeValue::arg_parser(std::move(parser));
}

static NativeValue native_argparser_named(const NativeValue &receiver,
                                          const NativeValue &text_value,
                                          const std::string &selector) {
  NativeArgParser &parser = as_mutable_arg_parser(receiver);
  if (selector == "name") {
    parser.name = native_arg_text(text_value);
  } else if (selector == "about") {
    parser.about = native_arg_text(text_value);
  } else {
    throw NativeBailout();
  }
  return receiver;
}

static void native_arg_apply_choices(NativeArgParser::Spec *spec,
                                     NativeArgKwArgs kwargs) {
  const NativeValue *choices = native_arg_kw(kwargs, "choices");
  if (choices == nullptr || choices->tag == NativeValue::Tag::Null) return;
  const NativeList &list = as_list(*choices);
  spec->has_choices = true;
  spec->choices = list.items;
}

static NativeValue native_argparser_option(
    const NativeValue &receiver, const std::string &selector,
    std::initializer_list<NativeValue> args, NativeArgKwArgs kwargs) {
  NativeArgParser &parser = as_mutable_arg_parser(receiver);
  const bool flag = selector == "flag";
  native_arg_reject_unknown(kwargs,
                            {"name", "type", "default", "required",
                             "choices", "multiple", "env", "negatable"});
  NativeArgParser::Spec spec;
  spec.kind = flag ? NativeArgParser::SpecKind::Flag
                   : NativeArgParser::SpecKind::Option;
  spec.type = flag ? NativeArgValueType::Bool : NativeArgValueType::Str;
  for (const NativeValue &arg : args) {
    if (arg.tag == NativeValue::Tag::Null) continue;
    const std::string spelling = native_arg_text(arg);
    native_arg_validate_spelling(parser, spelling);
    spec.spellings.push_back(spelling);
  }
  if (spec.spellings.empty()) throw NativeBailout();
  if (auto name = native_arg_optional_text(kwargs, "name")) {
    spec.name = *name;
  } else {
    spec.name = native_arg_derive_name(spec.spellings.back());
  }
  spec.type = native_arg_type_kw(kwargs, spec.type);
  spec.required = native_arg_bool_kw(kwargs, "required", false);
  spec.multiple = native_arg_bool_kw(kwargs, "multiple", false);
  spec.negatable = flag && native_arg_bool_kw(kwargs, "negatable", false);
  if (const NativeValue *default_value = native_arg_kw(kwargs, "default")) {
    spec.has_default = true;
    spec.default_value = *default_value;
  }
  native_arg_apply_choices(&spec, kwargs);
  if (auto env = native_arg_optional_text(kwargs, "env")) spec.env = *env;
  parser.specs.push_back(std::move(spec));
  return receiver;
}

static NativeValue native_argparser_positional(
    const NativeValue &receiver, const std::string &selector,
    const NativeValue &name_value, NativeArgKwArgs kwargs) {
  NativeArgParser &parser = as_mutable_arg_parser(receiver);
  const bool rest = selector == "rest";
  native_arg_reject_unknown(kwargs, {"type", "default", "required",
                                     "choices", "multiple", "env"});
  NativeArgParser::Spec spec;
  spec.kind = rest ? NativeArgParser::SpecKind::Rest
                   : NativeArgParser::SpecKind::Positional;
  spec.name = native_arg_text(name_value);
  spec.type = native_arg_type_kw(kwargs, NativeArgValueType::Str);
  spec.required = native_arg_bool_kw(kwargs, "required", !rest);
  spec.multiple = rest || native_arg_bool_kw(kwargs, "multiple", rest);
  if (const NativeValue *default_value = native_arg_kw(kwargs, "default")) {
    spec.has_default = true;
    spec.default_value = *default_value;
  }
  native_arg_apply_choices(&spec, kwargs);
  if (auto env = native_arg_optional_text(kwargs, "env")) spec.env = *env;
  parser.specs.push_back(std::move(spec));
  return receiver;
}

static bool native_arg_parse_bool(const std::string &text, bool *out) {
  const std::string lowered = native_arg_lower_ascii(text);
  if (lowered == "true" || lowered == "1" || lowered == "yes" ||
      lowered == "on") {
    *out = true;
    return true;
  }
  if (lowered == "false" || lowered == "0" || lowered == "no" ||
      lowered == "off") {
    *out = false;
    return true;
  }
  return false;
}

static NativeValue native_arg_convert(const std::string &text,
                                      NativeArgValueType type) {
  switch (type) {
  case NativeArgValueType::Str:
    return NativeValue::heap_string(text);
  case NativeArgValueType::Int: {
    char *end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') {
      throw NativeBailout();
    }
    return NativeValue::integer(static_cast<std::int64_t>(parsed));
  }
  case NativeArgValueType::Float: {
    char *end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') {
      throw NativeBailout();
    }
    return NativeValue::floating(parsed);
  }
  case NativeArgValueType::Bool: {
    bool parsed = false;
    if (!native_arg_parse_bool(text, &parsed)) throw NativeBailout();
    return NativeValue::boolean(parsed);
  }
  case NativeArgValueType::Symbol:
    return NativeValue::symbol_ref(native_intern_symbol(text));
  }
  throw NativeBailout();
}

static bool native_arg_simple_equal(const NativeValue &lhs,
                                    const NativeValue &rhs) {
  if (native_value_is_string(lhs) && native_value_is_string(rhs)) {
    return native_string_text_equal(lhs, rhs);
  }
  if (lhs.tag != rhs.tag) return false;
  switch (lhs.tag) {
  case NativeValue::Tag::Null:
    return true;
  case NativeValue::Tag::Bool:
  case NativeValue::Tag::Integer:
  case NativeValue::Tag::String:
  case NativeValue::Tag::Symbol:
    return lhs.scalar_value == rhs.scalar_value;
  case NativeValue::Tag::Float:
    return lhs.float_value == rhs.float_value;
  default:
    return false;
  }
}

static NativeValue native_arg_apply_value(const NativeArgParser::Spec &spec,
                                          const std::string &raw) {
  NativeValue value = native_arg_convert(raw, spec.type);
  if (spec.has_choices) {
    bool ok = false;
    for (const NativeValue &choice : spec.choices) {
      if (native_arg_simple_equal(value, choice)) {
        ok = true;
        break;
      }
    }
    if (!ok) throw NativeBailout();
  }
  return value;
}

static void native_arg_set_value(
    const NativeArgParser::Spec &spec, std::size_t index, NativeValue value,
    std::vector<NativeValue> *scalar_values,
    std::vector<std::vector<NativeValue>> *multi_values,
    std::vector<bool> *seen) {
  if (spec.multiple) {
    (*multi_values)[index].push_back(value);
  } else {
    (*scalar_values)[index] = value;
  }
  (*seen)[index] = true;
}

static const NativeArgParser::Spec *native_arg_find_option(
    const NativeArgParser &parser, const std::string &token, bool *negated,
    std::size_t *index) {
  *negated = false;
  for (std::size_t i = 0; i < parser.specs.size(); ++i) {
    const NativeArgParser::Spec &spec = parser.specs[i];
    if (spec.kind != NativeArgParser::SpecKind::Option &&
        spec.kind != NativeArgParser::SpecKind::Flag) {
      continue;
    }
    for (const std::string &spelling : spec.spellings) {
      if (spelling == token) {
        *index = i;
        return &spec;
      }
    }
    if (spec.kind == NativeArgParser::SpecKind::Flag && spec.negatable &&
        native_arg_starts_with(token, "--no-")) {
      const std::string positive = "--" + token.substr(5);
      for (const std::string &spelling : spec.spellings) {
        if (spelling == positive) {
          *negated = true;
          *index = i;
          return &spec;
        }
      }
    }
  }
  return nullptr;
}

static bool native_arg_env_lookup(const NativeArgParser &parser,
                                  const std::string &name,
                                  std::string *out) {
  for (const auto &entry : parser.env) {
    if (entry.first == name) {
      *out = entry.second;
      return true;
    }
  }
  return false;
}

static std::vector<std::string> native_arg_cmdline(
    const NativeArgParser &parser, NativeArgKwArgs kwargs) {
  const NativeValue *override_value = native_arg_kw(kwargs, "cmdline");
  if (override_value == nullptr) return parser.cmdline;
  return native_arg_string_list(*override_value);
}

static NativeValue native_argparser_parse_or_raise(const NativeValue &receiver,
                                                   NativeArgKwArgs kwargs) {
  native_arg_reject_unknown(kwargs, {"cmdline"});
  const NativeArgParser &parser = as_arg_parser(receiver);
  const std::vector<std::string> cmdline = native_arg_cmdline(parser, kwargs);
  for (const std::string &token : cmdline) {
    if (parser.add_help && (token == "-h" || token == "--help")) {
      throw NativeBailout();
    }
  }
  std::vector<NativeValue> scalar_values(parser.specs.size(),
                                         NativeValue::nullv());
  std::vector<std::vector<NativeValue>> multi_values(parser.specs.size());
  std::vector<bool> seen(parser.specs.size(), false);
  std::vector<std::string> positionals;
  bool parse_options = true;
  for (std::size_t i = 0; i < cmdline.size(); ++i) {
    const std::string &token = cmdline[i];
    if (parse_options && token == "--") {
      parse_options = false;
      continue;
    }
    if (parse_options && native_arg_starts_with(token, "-") &&
        token.size() > 1U) {
      std::string spelling = token;
      std::string attached_value;
      const std::size_t equals = token.find('=');
      if (equals != std::string::npos &&
          native_arg_starts_with(token, "--")) {
        spelling = token.substr(0, equals);
        attached_value = token.substr(equals + 1U);
      }
      bool negated = false;
      std::size_t spec_index = 0;
      const NativeArgParser::Spec *spec =
          native_arg_find_option(parser, spelling, &negated, &spec_index);
      if (spec == nullptr) throw NativeBailout();
      if (spec->kind == NativeArgParser::SpecKind::Flag) {
        NativeValue value = NativeValue::boolean(!negated);
        if (!attached_value.empty()) {
          bool parsed = false;
          if (!native_arg_parse_bool(attached_value, &parsed)) {
            throw NativeBailout();
          }
          value = NativeValue::boolean(parsed);
        }
        native_arg_set_value(*spec, spec_index, value, &scalar_values,
                             &multi_values, &seen);
        continue;
      }
      std::string raw_value = attached_value;
      if (raw_value.empty()) {
        if (i + 1U >= cmdline.size()) throw NativeBailout();
        raw_value = cmdline[++i];
      }
      native_arg_set_value(*spec, spec_index,
                           native_arg_apply_value(*spec, raw_value),
                           &scalar_values, &multi_values, &seen);
      continue;
    }
    positionals.push_back(token);
  }
  std::size_t positional_index = 0;
  bool has_rest = false;
  for (std::size_t i = 0; i < parser.specs.size(); ++i) {
    const NativeArgParser::Spec &spec = parser.specs[i];
    if (spec.kind == NativeArgParser::SpecKind::Positional) {
      if (positional_index >= positionals.size()) continue;
      native_arg_set_value(
          spec, i, native_arg_apply_value(spec, positionals[positional_index]),
          &scalar_values, &multi_values, &seen);
      ++positional_index;
    } else if (spec.kind == NativeArgParser::SpecKind::Rest) {
      has_rest = true;
      while (positional_index < positionals.size()) {
        multi_values[i].push_back(
            native_arg_apply_value(spec, positionals[positional_index]));
        ++positional_index;
      }
      seen[i] = !multi_values[i].empty();
    }
  }
  if (positional_index < positionals.size() && !has_rest) {
    throw NativeBailout();
  }
  for (std::size_t i = 0; i < parser.specs.size(); ++i) {
    const NativeArgParser::Spec &spec = parser.specs[i];
    if (seen[i]) continue;
    if (!spec.env.empty()) {
      std::string env_value;
      if (native_arg_env_lookup(parser, spec.env, &env_value)) {
        native_arg_set_value(spec, i, native_arg_apply_value(spec, env_value),
                             &scalar_values, &multi_values, &seen);
        continue;
      }
    }
    if (spec.has_default) {
      scalar_values[i] = spec.default_value;
      seen[i] = true;
      continue;
    }
    if (spec.kind == NativeArgParser::SpecKind::Flag) {
      scalar_values[i] = NativeValue::boolean(false);
      seen[i] = true;
      continue;
    }
    if (spec.multiple || spec.kind == NativeArgParser::SpecKind::Rest) {
      seen[i] = true;
      continue;
    }
    if (spec.required || spec.kind == NativeArgParser::SpecKind::Positional) {
      throw NativeBailout();
    }
  }
  std::vector<std::pair<std::string, NativeValue>> entries;
  entries.reserve(parser.specs.size());
  for (std::size_t i = 0; i < parser.specs.size(); ++i) {
    const NativeArgParser::Spec &spec = parser.specs[i];
    if (spec.multiple || spec.kind == NativeArgParser::SpecKind::Rest) {
      entries.push_back({spec.name, NativeValue::list(std::move(multi_values[i]))});
    } else {
      entries.push_back({spec.name, scalar_values[i]});
    }
  }
  return NativeValue::map(std::move(entries));
}

enum class NativeTimeSelector {
  Nanosecond,
  Nanoseconds,
  Microsecond,
  Microseconds,
  Millisecond,
  Milliseconds,
  Second,
  Seconds,
  Minute,
  Minutes,
  Hour,
  Hours,
  Day,
  Days,
  Week,
  Weeks,
  Month,
  Months,
  Year,
  Years,
  Iso8601,
  ToStr,
  Inspect,
  UnixSeconds,
  UnixMilliseconds,
  UnixNanoseconds,
  Weekday,
  Yearday,
  FixedPredicate,
  TotalNanoseconds,
};

static bool native_time_selector_is_unit(NativeTimeSelector selector) {
  switch (selector) {
  case NativeTimeSelector::Nanosecond:
  case NativeTimeSelector::Nanoseconds:
  case NativeTimeSelector::Microsecond:
  case NativeTimeSelector::Microseconds:
  case NativeTimeSelector::Millisecond:
  case NativeTimeSelector::Milliseconds:
  case NativeTimeSelector::Second:
  case NativeTimeSelector::Seconds:
  case NativeTimeSelector::Minute:
  case NativeTimeSelector::Minutes:
  case NativeTimeSelector::Hour:
  case NativeTimeSelector::Hours:
  case NativeTimeSelector::Day:
  case NativeTimeSelector::Days:
  case NativeTimeSelector::Week:
  case NativeTimeSelector::Weeks:
  case NativeTimeSelector::Month:
  case NativeTimeSelector::Months:
  case NativeTimeSelector::Year:
  case NativeTimeSelector::Years:
    return true;
  default:
    return false;
  }
}

static NativeValue
native_time_period_unit_scaled(const NativeValue &receiver,
                               std::int64_t months_per,
                               std::int64_t days_per,
                               std::int64_t nanos_per, bool calendar) {
  if (receiver.tag == NativeValue::Tag::Integer) {
    const std::int64_t scalar = receiver.scalar_value;
    return NativeValue::time_period(NativeTimePeriod{
        native_checked_i128(static_cast<__int128>(scalar) * months_per),
        native_checked_i128(static_cast<__int128>(scalar) * days_per),
        native_checked_i128(static_cast<__int128>(scalar) * nanos_per)});
  }
  if (receiver.tag == NativeValue::Tag::Float && !calendar) {
    if (!std::isfinite(receiver.float_value)) throw NativeBailout();
    const long double total =
        static_cast<long double>(receiver.float_value) *
        static_cast<long double>(nanos_per);
    const long double rounded = std::round(total);
    if (std::abs(total - rounded) > 0.001L) throw NativeBailout();
    if (rounded <
            static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
        rounded >
            static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
      throw NativeBailout();
    }
    return NativeValue::time_period(
        NativeTimePeriod{0, 0, native_checked_i128(
                                   static_cast<__int128>(rounded))});
  }
  throw NativeBailout();
}

static NativeValue native_time_period_unit(const NativeValue &receiver,
                                           NativeTimeSelector selector) {
  std::int64_t months_per = 0;
  std::int64_t days_per = 0;
  std::int64_t nanos_per = 0;
  bool calendar = false;
  switch (selector) {
  case NativeTimeSelector::Nanosecond:
  case NativeTimeSelector::Nanoseconds:
    nanos_per = 1;
    break;
  case NativeTimeSelector::Microsecond:
  case NativeTimeSelector::Microseconds:
    nanos_per = 1000;
    break;
  case NativeTimeSelector::Millisecond:
  case NativeTimeSelector::Milliseconds:
    nanos_per = 1000000;
    break;
  case NativeTimeSelector::Second:
  case NativeTimeSelector::Seconds:
    nanos_per = 1000000000;
    break;
  case NativeTimeSelector::Minute:
  case NativeTimeSelector::Minutes:
    nanos_per = 60LL * 1000000000LL;
    break;
  case NativeTimeSelector::Hour:
  case NativeTimeSelector::Hours:
    nanos_per = 60LL * 60LL * 1000000000LL;
    break;
  case NativeTimeSelector::Day:
  case NativeTimeSelector::Days:
    days_per = 1;
    calendar = true;
    break;
  case NativeTimeSelector::Week:
  case NativeTimeSelector::Weeks:
    days_per = 7;
    calendar = true;
    break;
  case NativeTimeSelector::Month:
  case NativeTimeSelector::Months:
    months_per = 1;
    calendar = true;
    break;
  case NativeTimeSelector::Year:
  case NativeTimeSelector::Years:
    months_per = 12;
    calendar = true;
    break;
  default:
    throw NativeBailout();
  }
  return native_time_period_unit_scaled(receiver, months_per, days_per,
                                        nanos_per, calendar);
}

static NativeValue native_time_nullary(const NativeValue &receiver,
                                       NativeTimeSelector selector) {
  if (receiver.tag == NativeValue::Tag::Uuid) {
    switch (selector) {
    case NativeTimeSelector::ToStr:
      return native_uuid_nullary(receiver, "to_str");
    case NativeTimeSelector::Inspect:
      return native_uuid_nullary(receiver, "inspect");
    default:
      throw NativeBailout();
    }
  }
  if ((receiver.tag == NativeValue::Tag::Integer ||
       receiver.tag == NativeValue::Tag::Float) &&
      native_time_selector_is_unit(selector)) {
    return native_time_period_unit(receiver, selector);
  }
  if (receiver.tag == NativeValue::Tag::Time) {
    const NativeTime &time = as_time(receiver);
    switch (selector) {
    case NativeTimeSelector::Iso8601:
    case NativeTimeSelector::ToStr:
    case NativeTimeSelector::Inspect:
      return NativeValue::string_ref(
          native_intern_string(amber::runtime::runtime_time_to_iso8601(time)));
    case NativeTimeSelector::UnixSeconds:
      return NativeValue::integer(time.epoch_seconds);
    case NativeTimeSelector::UnixMilliseconds:
      return NativeValue::integer(native_checked_i128(
          static_cast<__int128>(time.epoch_seconds) * 1000 +
          time.nanosecond / 1000000));
    case NativeTimeSelector::UnixNanoseconds:
      return NativeValue::integer(native_checked_i128(
          static_cast<__int128>(time.epoch_seconds) * kNativeNanosPerSecond +
          time.nanosecond));
    case NativeTimeSelector::Weekday: {
      const std::int64_t days =
          native_floor_div_i64(time.epoch_seconds, kNativeSecondsPerDay);
      return NativeValue::integer(native_floor_mod_i64(days + 3, 7) + 1);
    }
    case NativeTimeSelector::Year:
    case NativeTimeSelector::Month:
    case NativeTimeSelector::Day:
    case NativeTimeSelector::Hour:
    case NativeTimeSelector::Minute:
    case NativeTimeSelector::Second:
    case NativeTimeSelector::Nanosecond:
    case NativeTimeSelector::Yearday: {
      const NativeUtcFields fields = native_utc_fields(time);
      switch (selector) {
      case NativeTimeSelector::Year:
        return NativeValue::integer(fields.date.year);
      case NativeTimeSelector::Month:
        return NativeValue::integer(fields.date.month);
      case NativeTimeSelector::Day:
        return NativeValue::integer(fields.date.day);
      case NativeTimeSelector::Hour:
        return NativeValue::integer(fields.hour);
      case NativeTimeSelector::Minute:
        return NativeValue::integer(fields.minute);
      case NativeTimeSelector::Second:
        return NativeValue::integer(fields.second);
      case NativeTimeSelector::Nanosecond:
        return NativeValue::integer(fields.nanosecond);
      case NativeTimeSelector::Yearday: {
        const std::int64_t jan1 =
            native_days_from_civil(fields.date.year, 1, 1);
        const std::int64_t today = native_days_from_civil(
            fields.date.year, fields.date.month, fields.date.day);
        return NativeValue::integer(today - jan1 + 1);
      }
      default:
        break;
      }
      break;
    }
    default:
      break;
    }
  }
  if (receiver.tag == NativeValue::Tag::TimePeriod) {
    const NativeTimePeriod &period = as_time_period(receiver);
    switch (selector) {
    case NativeTimeSelector::ToStr:
    case NativeTimeSelector::Inspect:
      return NativeValue::heap_string(
          amber::runtime::runtime_time_period_to_string(period));
    case NativeTimeSelector::Months:
      return NativeValue::integer(period.months);
    case NativeTimeSelector::Days:
      return NativeValue::integer(period.days);
    case NativeTimeSelector::Nanoseconds:
      return NativeValue::integer(period.nanoseconds);
    case NativeTimeSelector::FixedPredicate:
      return NativeValue::boolean(period.months == 0);
    case NativeTimeSelector::TotalNanoseconds:
      return NativeValue::integer(
          native_checked_i128(native_fixed_period_nanoseconds(period)));
    default:
      break;
    }
  }
  throw NativeBailout();
}

static NativeValue native_time_add(const NativeValue &lhs,
                                   const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Time && rhs.tag == NativeValue::Tag::TimePeriod) {
    return NativeValue::time(native_apply_period_to_time(as_time(lhs),
                                                         as_time_period(rhs)));
  }
  if (lhs.tag == NativeValue::Tag::TimePeriod && rhs.tag == NativeValue::Tag::Time) {
    return NativeValue::time(native_apply_period_to_time(as_time(rhs),
                                                         as_time_period(lhs)));
  }
  if (lhs.tag == NativeValue::Tag::TimePeriod &&
      rhs.tag == NativeValue::Tag::TimePeriod) {
    return NativeValue::time_period(
        native_period_add(as_time_period(lhs), as_time_period(rhs)));
  }
  throw NativeBailout();
}

static NativeValue native_time_sub(const NativeValue &lhs,
                                   const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Time && rhs.tag == NativeValue::Tag::Time) {
    return NativeValue::time_period(native_period_between(as_time(lhs),
                                                          as_time(rhs)));
  }
  if (lhs.tag == NativeValue::Tag::Time && rhs.tag == NativeValue::Tag::TimePeriod) {
    const NativeTimePeriod negated =
        native_period_add(NativeTimePeriod{}, as_time_period(rhs), -1);
    return NativeValue::time(native_apply_period_to_time(as_time(lhs), negated));
  }
  if (lhs.tag == NativeValue::Tag::TimePeriod &&
      rhs.tag == NativeValue::Tag::TimePeriod) {
    return NativeValue::time_period(
        native_period_add(as_time_period(lhs), as_time_period(rhs), -1));
  }
  throw NativeBailout();
}

static NativeValue native_time_compare_value(const NativeValue &lhs,
                                             const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Time && rhs.tag == NativeValue::Tag::Time) {
    return NativeValue::integer(native_compare_time(as_time(lhs), as_time(rhs)));
  }
  if (lhs.tag == NativeValue::Tag::TimePeriod &&
      rhs.tag == NativeValue::Tag::TimePeriod) {
    return NativeValue::integer(native_compare_i128(
        native_fixed_period_nanoseconds(as_time_period(lhs)),
        native_fixed_period_nanoseconds(as_time_period(rhs))));
  }
  throw NativeBailout();
}

static std::string native_fs_path_text(const NativeValue &value) {
  if (native_value_is_string(value)) return native_string_text(value);
  if (value.tag == NativeValue::Tag::FsPath) return as_fs_path(value).path;
  throw NativeBailout();
}

static NativeValue native_fs_path_new(const NativeValue &module,
                                      const NativeValue &path_value,
                                      bool has_path) {
  if (module.tag != NativeValue::Tag::FsModule || !has_path) {
    throw NativeBailout();
  }
  return NativeValue::fs_path(NativeFsPath{native_fs_path_text(path_value)});
}

static NativeValue native_fs_path_join(
    const NativeValue &receiver, std::initializer_list<NativeValue> parts) {
  if (parts.size() == 0U) throw NativeBailout();
  std::filesystem::path joined(as_fs_path(receiver).path);
  for (const NativeValue &part : parts) {
    joined /= native_fs_path_text(part);
  }
  return NativeValue::fs_path(NativeFsPath{joined.string()});
}

static NativeValue native_fs_path_nullary(const NativeValue &receiver,
                                          const std::string &selector) {
  const std::filesystem::path path(as_fs_path(receiver).path);
  if (selector == "basename") {
    return NativeValue::heap_string(path.filename().string());
  }
  if (selector == "extname") {
    return NativeValue::heap_string(path.extension().string());
  }
  if (selector == "parent") {
    return NativeValue::fs_path(NativeFsPath{path.parent_path().string()});
  }
  if (selector == "absolute?") {
    return NativeValue::boolean(path.is_absolute());
  }
  if (selector == "normalize") {
    return NativeValue::fs_path(NativeFsPath{path.lexically_normal().string()});
  }
  throw NativeBailout();
}

static bool native_fs_read_allowed(const std::string &path) {
  for (const auto &grant : embedded_capability_grants()) {
    if (grant.name != "fs.read") continue;
    if ((grant.flags & amber::capability::kCapabilityFlagWildcardTarget) != 0U ||
        grant.target == "*") {
      return true;
    }
    if (grant.target == path) return true;
    if (!grant.target.empty() && path.size() > grant.target.size() &&
        path.compare(0, grant.target.size(), grant.target) == 0 &&
        path[grant.target.size()] == '/') {
      return true;
    }
  }
  return false;
}

static NativeValue native_json_stream_parse_file(const NativeValue &path_value,
                                                 const NativeValue &jsonl_value,
                                                 const NativeValue &block_value) {
  if (!native_value_is_string(path_value) ||
      jsonl_value.tag != NativeValue::Tag::Bool ||
      jsonl_value.scalar_value == 0) {
    throw NativeBailout();
  }
  const std::string &path = native_string_text(path_value);
  if (!native_fs_read_allowed(path)) throw NativeBailout();
  std::ifstream input(path);
  if (!input) throw NativeBailout();
  std::string line;
  std::int64_t count = 0;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    NativeJsonParser parser(line);
    const NativeValue row = parser.parse_document();
    (void)amber_native_call_closure(block_value, {row});
    ++count;
  }
  return NativeValue::integer(count);
}

)AMBERCPP";
  out << R"AMBERCPP(
static constexpr const char *kNativeBenchmarkSchema = "amber.benchmark.v1";
static constexpr const char *kNativeBenchmarkProfilerSchema =
    "amber.benchmark.profiler.v1";

struct NativeBenchmarkProfileSpan {
  std::string label;
  NativeValue data = NativeValue::nullv();
  std::int64_t elapsed_ns = 0;
  std::int64_t self_ns = 0;
  std::int64_t parent = -1;
  std::int64_t depth = 0;
  std::vector<std::size_t> children;
};

struct NativeBenchmarkProfileState {
  std::int64_t id = 0;
  std::vector<NativeBenchmarkProfileSpan> spans;
  std::vector<std::size_t> stack;
};

static thread_local std::int64_t native_benchmark_next_profile_id = 1;
static thread_local std::vector<NativeBenchmarkProfileState *>
    native_benchmark_profiles;

static std::int64_t native_benchmark_now_ns() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

static NativeValue native_benchmark_string(const std::string &text) {
  return NativeValue::heap_string(text);
}

static std::string native_benchmark_text(const NativeValue &value) {
  if (native_value_is_string(value)) return native_string_text(value);
  if (value.tag == NativeValue::Tag::Symbol) {
    return native_symbol_text(value.scalar_value);
  }
  if (value.tag == NativeValue::Tag::Null) return "";
  throw NativeBailout();
}

static const NativeValue *native_benchmark_kw(
    const std::vector<std::pair<std::string, NativeValue>> &kwargs,
    const std::string &name) {
  for (const auto &entry : kwargs) {
    if (entry.first == name) return &entry.second;
  }
  return nullptr;
}

static bool native_benchmark_bool_kw(
    const std::vector<std::pair<std::string, NativeValue>> &kwargs,
    const std::string &name, bool fallback) {
  const NativeValue *value = native_benchmark_kw(kwargs, name);
  if (value == nullptr) return fallback;
  if (value->tag != NativeValue::Tag::Bool) throw NativeBailout();
  return value->scalar_value != 0;
}

static std::int64_t native_benchmark_int_kw(
    const std::vector<std::pair<std::string, NativeValue>> &kwargs,
    const std::string &name, std::int64_t fallback, std::int64_t min_value) {
  const NativeValue *value = native_benchmark_kw(kwargs, name);
  if (value == nullptr) return fallback;
  if (value->tag != NativeValue::Tag::Integer ||
      value->scalar_value < min_value) {
    throw NativeBailout();
  }
  return value->scalar_value;
}

static std::int64_t native_benchmark_period_kw_ns(
    const std::vector<std::pair<std::string, NativeValue>> &kwargs,
    const std::string &name) {
  const NativeValue *value = native_benchmark_kw(kwargs, name);
  if (value == nullptr || value->tag == NativeValue::Tag::Null) return 0;
  if (value->tag != NativeValue::Tag::TimePeriod) throw NativeBailout();
  const NativeTimePeriod &period = as_time_period(*value);
  if (period.months != 0) throw NativeBailout();
  const __int128 ns =
      static_cast<__int128>(period.days) * 86400 * 1000000000 +
      period.nanoseconds;
  return native_checked_i128(ns < 0 ? 0 : ns);
}

static std::string native_benchmark_format_decimal(double value,
                                                   int precision = 2) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

static std::string native_benchmark_format_ns(std::int64_t ns,
                                              std::string unit = "auto") {
  const double value = static_cast<double>(ns);
  if (unit == "auto") {
    const double abs_value = std::fabs(value);
    if (abs_value < 1000.0) unit = "ns";
    else if (abs_value < 1000000.0) unit = "us";
    else if (abs_value < 1000000000.0) unit = "ms";
    else unit = "s";
  }
  if (unit == "ns") return std::to_string(ns) + " ns";
  if (unit == "us") return native_benchmark_format_decimal(value / 1000.0, 3) + " us";
  if (unit == "ms") return native_benchmark_format_decimal(value / 1000000.0, 3) + " ms";
  if (unit == "s") return native_benchmark_format_decimal(value / 1000000000.0, 3) + " s";
  throw NativeBailout();
}

static NativeValue native_benchmark_map(
    std::initializer_list<std::pair<std::string, NativeValue>> entries) {
  return NativeValue::map(std::vector<std::pair<std::string, NativeValue>>(
      entries.begin(), entries.end()));
}

static const NativeValue *native_benchmark_map_get(const NativeValue &map,
                                                   const std::string &key) {
  if (map.tag != NativeValue::Tag::Map) return nullptr;
  for (const auto &entry : as_map(map).entries) {
    if (native_key_text(entry.first) == key) return &entry.second;
  }
  return nullptr;
}

static bool native_benchmark_has_schema(const NativeValue &value,
                                        const std::string &schema) {
  const NativeValue *raw = native_benchmark_map_get(value, "schema");
  return raw != nullptr && native_value_is_string(*raw) &&
         native_string_text(*raw) == schema;
}

static std::string native_benchmark_kind(const NativeValue &result) {
  if (!native_benchmark_has_schema(result, kNativeBenchmarkSchema)) {
    throw NativeBailout();
  }
  const NativeValue *kind = native_benchmark_map_get(result, "kind");
  if (kind == nullptr || !native_value_is_string(*kind)) {
    throw NativeBailout();
  }
  return native_string_text(*kind);
}

static const NativeValue &native_benchmark_data(const NativeValue &result) {
  const NativeValue *data = native_benchmark_map_get(result, "data");
  if (data == nullptr || data->tag != NativeValue::Tag::Map) {
    throw NativeBailout();
  }
  return *data;
}

static std::int64_t native_benchmark_int_field(const NativeValue &map,
                                               const std::string &key) {
  const NativeValue *value = native_benchmark_map_get(map, key);
  if (value == nullptr) return 0;
  if (value->tag != NativeValue::Tag::Integer) throw NativeBailout();
  return value->scalar_value;
}

static double native_benchmark_float_field(const NativeValue &map,
                                           const std::string &key) {
  const NativeValue *value = native_benchmark_map_get(map, key);
  if (value == nullptr) return 0.0;
  if (value->tag == NativeValue::Tag::Float) return value->float_value;
  if (value->tag == NativeValue::Tag::Integer) {
    return static_cast<double>(value->scalar_value);
  }
  throw NativeBailout();
}

static NativeValue native_benchmark_sanitize(const NativeValue &result) {
  (void)native_benchmark_kind(result);
  std::vector<std::pair<NativeValue, NativeValue>> entries;
  for (const auto &entry : as_map(result).entries) {
    if (native_key_text(entry.first) == "value") continue;
    entries.push_back(entry);
  }
  return NativeValue::map_entries(std::move(entries), as_map(result).strict);
}

static NativeValue native_benchmark_result(
    const std::string &kind, const NativeValue &label, const NativeValue &data,
    const NativeValue &value, bool include_value) {
  std::vector<std::pair<std::string, NativeValue>> entries{
      {"schema", native_benchmark_string(kNativeBenchmarkSchema)},
      {"kind", native_benchmark_string(kind)},
      {"label", label},
      {"data", data},
  };
  if (include_value) entries.push_back({"value", value});
  return NativeValue::map(std::move(entries));
}

static NativeValue native_benchmark_label_arg(
    const std::vector<NativeValue> &args) {
  if (args.empty() || args[0].tag == NativeValue::Tag::Null) {
    return NativeValue::nullv();
  }
  return native_benchmark_string(native_benchmark_text(args[0]));
}

static NativeValue native_benchmark_measurement(
    const NativeValue &label, std::int64_t elapsed_ns,
    const NativeValue &value, const std::string &kind) {
  NativeValue data = native_benchmark_map({
      {"elapsed_ns", NativeValue::integer(elapsed_ns)},
      {"elapsed_text", native_benchmark_string(native_benchmark_format_ns(elapsed_ns))},
      {"iterations", NativeValue::integer(1)},
  });
  return native_benchmark_result(kind, label, data, value, true);
}

static NativeValue native_benchmark_time_period(std::int64_t ns) {
  NativeTimePeriod period;
  period.nanoseconds = ns;
  return NativeValue::time_period(period);
}

static std::size_t native_benchmark_percentile_index(std::size_t size,
                                                     double percentile) {
  if (size == 0U) return 0U;
  const double rank = std::ceil(percentile * static_cast<double>(size));
  const std::size_t index =
      static_cast<std::size_t>(std::max(1.0, rank)) - 1U;
  return std::min(index, size - 1U);
}

static NativeValue native_benchmark_report(
    const NativeValue &label, std::int64_t iterations,
    const std::vector<std::int64_t> &sample_ns) {
  std::int64_t elapsed = 0;
  std::vector<std::int64_t> per_iter;
  for (std::int64_t ns : sample_ns) {
    elapsed = native_checked_i128(static_cast<__int128>(elapsed) + ns);
    per_iter.push_back(ns / std::max<std::int64_t>(1, iterations));
  }
  std::vector<std::int64_t> sorted = per_iter;
  std::sort(sorted.begin(), sorted.end());
  const std::int64_t min_ns = sorted.empty() ? 0 : sorted.front();
  const std::int64_t max_ns = sorted.empty() ? 0 : sorted.back();
  const std::int64_t total_iterations =
      iterations * static_cast<std::int64_t>(sample_ns.size());
  const std::int64_t mean_ns =
      total_iterations <= 0 ? 0 : elapsed / std::max<std::int64_t>(1, total_iterations);
  const auto percentile = [&](double p) {
    return sorted.empty() ? 0 : sorted[native_benchmark_percentile_index(sorted.size(), p)];
  };
  const double seconds = static_cast<double>(elapsed) / 1000000000.0;
  const double ops = seconds <= 0.0 ? 0.0 : static_cast<double>(total_iterations) / seconds;
  std::vector<NativeValue> samples;
  for (std::int64_t ns : sample_ns) samples.push_back(NativeValue::integer(ns));
  NativeValue data = native_benchmark_map({
      {"iterations", NativeValue::integer(total_iterations)},
      {"iterations_per_sample", NativeValue::integer(iterations)},
      {"samples", NativeValue::integer(static_cast<std::int64_t>(sample_ns.size()))},
      {"elapsed_ns", NativeValue::integer(elapsed)},
      {"elapsed_text", native_benchmark_string(native_benchmark_format_ns(elapsed))},
      {"per_iteration_ns", NativeValue::integer(mean_ns)},
      {"mean_ns", NativeValue::integer(mean_ns)},
      {"min_ns", NativeValue::integer(min_ns)},
      {"max_ns", NativeValue::integer(max_ns)},
      {"p50_ns", NativeValue::integer(percentile(0.50))},
      {"p90_ns", NativeValue::integer(percentile(0.90))},
      {"p95_ns", NativeValue::integer(percentile(0.95))},
      {"p99_ns", NativeValue::integer(percentile(0.99))},
      {"ops_per_second", NativeValue::floating(ops)},
      {"sample_ns", NativeValue::list(std::move(samples))},
  });
  return native_benchmark_result("report", label, data, NativeValue::nullv(), false);
}

static NativeValue native_benchmark_profiler_map(std::int64_t id) {
  return native_benchmark_map({
      {"schema", native_benchmark_string(kNativeBenchmarkProfilerSchema)},
      {"id", NativeValue::integer(id)},
  });
}

static NativeBenchmarkProfileState *native_benchmark_active_profile(
    const NativeValue &profiler) {
  if (native_benchmark_profiles.empty() ||
      !native_benchmark_has_schema(profiler, kNativeBenchmarkProfilerSchema)) {
    throw NativeBailout();
  }
  const NativeValue *id = native_benchmark_map_get(profiler, "id");
  if (id == nullptr || id->tag != NativeValue::Tag::Integer) {
    throw NativeBailout();
  }
  NativeBenchmarkProfileState *state = native_benchmark_profiles.back();
  if (state == nullptr || state->id != id->scalar_value) throw NativeBailout();
  return state;
}

static void native_benchmark_finalize_profile(NativeBenchmarkProfileState *state) {
  for (NativeBenchmarkProfileSpan &span : state->spans) {
    std::int64_t children_ns = 0;
    for (std::size_t child : span.children) {
      children_ns = native_checked_i128(static_cast<__int128>(children_ns) +
                                        state->spans[child].elapsed_ns);
    }
    span.self_ns = std::max<std::int64_t>(0, span.elapsed_ns - children_ns);
  }
}

static NativeValue native_benchmark_span_map(
    const NativeBenchmarkProfileState &state, std::size_t index) {
  const NativeBenchmarkProfileSpan &span = state.spans[index];
  std::vector<NativeValue> children;
  for (std::size_t child : span.children) {
    children.push_back(native_benchmark_span_map(state, child));
  }
  return native_benchmark_map({
      {"label", native_benchmark_string(span.label)},
      {"data", span.data},
      {"elapsed_ns", NativeValue::integer(span.elapsed_ns)},
      {"elapsed_text", native_benchmark_string(native_benchmark_format_ns(span.elapsed_ns))},
      {"self_ns", NativeValue::integer(span.self_ns)},
      {"self_text", native_benchmark_string(native_benchmark_format_ns(span.self_ns))},
      {"depth", NativeValue::integer(span.depth)},
      {"parent_index", NativeValue::integer(span.parent)},
      {"children", NativeValue::list(std::move(children))},
  });
}

static NativeValue native_benchmark_profile_spans(
    const NativeBenchmarkProfileState &state) {
  std::vector<NativeValue> spans;
  for (std::size_t i = 0; i < state.spans.size(); ++i) {
    spans.push_back(native_benchmark_span_map(state, i));
  }
  return NativeValue::list(std::move(spans));
}

static NativeValue native_benchmark_profile_summary(
    const NativeBenchmarkProfileState &state) {
  struct Row {
    std::string label;
    std::int64_t count = 0;
    std::int64_t total_ns = 0;
    std::int64_t self_ns = 0;
    std::int64_t min_ns = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_ns = 0;
  };
  std::vector<Row> rows;
  for (const NativeBenchmarkProfileSpan &span : state.spans) {
    auto it = std::find_if(rows.begin(), rows.end(), [&](const Row &row) {
      return row.label == span.label;
    });
    if (it == rows.end()) {
      rows.push_back(Row{span.label});
      it = rows.end() - 1;
    }
    it->count += 1;
    it->total_ns = native_checked_i128(static_cast<__int128>(it->total_ns) +
                                       span.elapsed_ns);
    it->self_ns = native_checked_i128(static_cast<__int128>(it->self_ns) +
                                      span.self_ns);
    it->min_ns = std::min(it->min_ns, span.elapsed_ns);
    it->max_ns = std::max(it->max_ns, span.elapsed_ns);
  }
  std::vector<NativeValue> out;
  for (const Row &row : rows) {
    const std::int64_t mean =
        row.count <= 0 ? 0 : row.total_ns / std::max<std::int64_t>(1, row.count);
    const std::int64_t min_ns =
        row.min_ns == std::numeric_limits<std::int64_t>::max() ? 0 : row.min_ns;
    out.push_back(native_benchmark_map({
        {"label", native_benchmark_string(row.label)},
        {"count", NativeValue::integer(row.count)},
        {"total_ns", NativeValue::integer(row.total_ns)},
        {"self_ns", NativeValue::integer(row.self_ns)},
        {"mean_ns", NativeValue::integer(mean)},
        {"min_ns", NativeValue::integer(min_ns)},
        {"max_ns", NativeValue::integer(row.max_ns)},
    }));
  }
  return NativeValue::list(std::move(out));
}

static NativeValue native_benchmark_compare(const std::vector<NativeValue> &args) {
  std::vector<NativeValue> reports;
  if (args.size() == 1U && args[0].tag == NativeValue::Tag::List) {
    reports = as_list(args[0]).items;
  } else {
    reports = args;
  }
  if (reports.empty()) throw NativeBailout();
  std::int64_t fastest = std::numeric_limits<std::int64_t>::max();
  std::int64_t slowest = std::numeric_limits<std::int64_t>::min();
  std::size_t fastest_index = 0;
  std::size_t slowest_index = 0;
  std::vector<NativeValue> clean;
  for (std::size_t i = 0; i < reports.size(); ++i) {
    if (native_benchmark_kind(reports[i]) != "report") throw NativeBailout();
    const NativeValue &data = native_benchmark_data(reports[i]);
    const std::int64_t mean = native_benchmark_int_field(data, "mean_ns");
    if (mean < fastest) { fastest = mean; fastest_index = i; }
    if (mean > slowest) { slowest = mean; slowest_index = i; }
    clean.push_back(native_benchmark_sanitize(reports[i]));
  }
  std::vector<NativeValue> relative;
  for (std::size_t i = 0; i < reports.size(); ++i) {
    const NativeValue &data = native_benchmark_data(reports[i]);
    std::string label = "case " + std::to_string(i + 1U);
    if (const NativeValue *raw = native_benchmark_map_get(reports[i], "label")) {
      if (native_value_is_string(*raw)) label = native_string_text(*raw);
    }
    const std::int64_t mean = native_benchmark_int_field(data, "mean_ns");
    relative.push_back(native_benchmark_map({
        {"label", native_benchmark_string(label)},
        {"mean_ns", NativeValue::integer(mean)},
        {"ratio_to_fastest", NativeValue::floating(
            fastest <= 0 ? 1.0 : static_cast<double>(mean) / static_cast<double>(fastest))},
    }));
  }
  NativeValue data = native_benchmark_map({
      {"cases", NativeValue::list(std::move(clean))},
      {"fastest_index", NativeValue::integer(static_cast<std::int64_t>(fastest_index))},
      {"slowest_index", NativeValue::integer(static_cast<std::int64_t>(slowest_index))},
      {"relative", NativeValue::list(std::move(relative))},
  });
  return native_benchmark_result("compare_report", NativeValue::nullv(), data,
                                 NativeValue::nullv(), false);
}

static std::string native_benchmark_label(const NativeValue &result,
                                          std::size_t index) {
  const NativeValue *label = native_benchmark_map_get(result, "label");
  if (label != nullptr && native_value_is_string(*label)) {
    return native_string_text(*label);
  }
  return "case " + std::to_string(index + 1U);
}

static NativeValue native_benchmark_format(
    const NativeValue &result,
    const std::vector<std::pair<std::string, NativeValue>> &kwargs,
    const std::string &default_layout) {
  (void)default_layout;
  const std::string unit = native_benchmark_kw(kwargs, "unit") == nullptr
                               ? "auto"
                               : native_benchmark_text(*native_benchmark_kw(kwargs, "unit"));
  const std::string style = native_benchmark_kw(kwargs, "style") == nullptr
                                ? "plain"
                                : native_benchmark_text(*native_benchmark_kw(kwargs, "style"));
  const std::string highlight = native_benchmark_kw(kwargs, "highlight") == nullptr
                                    ? "none"
                                    : native_benchmark_text(*native_benchmark_kw(kwargs, "highlight"));
  const std::string kind = native_benchmark_kind(result);
  std::ostringstream out;
  if (kind == "measurement") {
    const NativeValue &data = native_benchmark_data(result);
    out << "label | elapsed\n"
        << native_benchmark_label(result, 0) << " | "
        << native_benchmark_format_ns(
               native_benchmark_int_field(data, "elapsed_ns"), unit);
  } else if (kind == "report") {
    const NativeValue &data = native_benchmark_data(result);
    out << "case | iterations | mean | p95 | ops/s\n"
        << native_benchmark_label(result, 0) << " | "
        << native_benchmark_int_field(data, "iterations") << " | "
        << native_benchmark_format_ns(native_benchmark_int_field(data, "mean_ns"), unit) << " | "
        << native_benchmark_format_ns(native_benchmark_int_field(data, "p95_ns"), unit) << " | "
        << native_benchmark_format_decimal(native_benchmark_float_field(data, "ops_per_second"));
  } else if (kind == "compare_report") {
    const NativeValue &data = native_benchmark_data(result);
    const NativeValue *cases_value = native_benchmark_map_get(data, "cases");
    if (cases_value == nullptr || cases_value->tag != NativeValue::Tag::List) {
      throw NativeBailout();
    }
    const auto &cases = as_list(*cases_value).items;
    const std::int64_t fastest_index =
        native_benchmark_int_field(data, "fastest_index");
    std::int64_t fastest_mean = 0;
    if (fastest_index >= 0 && static_cast<std::size_t>(fastest_index) < cases.size()) {
      fastest_mean = native_benchmark_int_field(
          native_benchmark_data(cases[static_cast<std::size_t>(fastest_index)]), "mean_ns");
    }
    out << "case | iterations | mean | p95 | ops/s | relative";
    for (std::size_t i = 0; i < cases.size(); ++i) {
      const NativeValue &case_data = native_benchmark_data(cases[i]);
      std::ostringstream row;
      const std::int64_t mean = native_benchmark_int_field(case_data, "mean_ns");
      const double ratio = fastest_mean <= 0
                               ? 1.0
                               : static_cast<double>(mean) / static_cast<double>(fastest_mean);
      row << native_benchmark_label(cases[i], i) << " | "
          << native_benchmark_int_field(case_data, "iterations") << " | "
          << native_benchmark_format_ns(mean, unit) << " | "
          << native_benchmark_format_ns(native_benchmark_int_field(case_data, "p95_ns"), unit) << " | "
          << native_benchmark_format_decimal(native_benchmark_float_field(case_data, "ops_per_second")) << " | "
          << native_benchmark_format_decimal(ratio) << "x";
      const bool bold = (style == "ansi" || style == "xterm") &&
                        highlight == "best" &&
                        static_cast<std::int64_t>(i) == fastest_index;
      out << "\n" << (bold ? "\x1b[1m" : "") << row.str()
          << (bold ? "\x1b[22m" : "");
    }
  } else if (kind == "profile") {
    const NativeValue &data = native_benchmark_data(result);
    const NativeValue *summary = native_benchmark_map_get(data, "summary");
    if (summary != nullptr && summary->tag == NativeValue::Tag::List &&
        !as_list(*summary).items.empty()) {
      out << "section | count | total | self | mean | max";
      for (const NativeValue &row : as_list(*summary).items) {
        out << "\n"
            << native_benchmark_text(*native_benchmark_map_get(row, "label")) << " | "
            << native_benchmark_int_field(row, "count") << " | "
            << native_benchmark_format_ns(native_benchmark_int_field(row, "total_ns"), unit) << " | "
            << native_benchmark_format_ns(native_benchmark_int_field(row, "self_ns"), unit) << " | "
            << native_benchmark_format_ns(native_benchmark_int_field(row, "mean_ns"), unit) << " | "
            << native_benchmark_format_ns(native_benchmark_int_field(row, "max_ns"), unit);
      }
    } else {
      out << "profile | total\n" << native_benchmark_label(result, 0) << " | "
          << native_benchmark_format_ns(native_benchmark_int_field(data, "total_ns"), unit);
    }
  } else {
    throw NativeBailout();
  }
  return native_benchmark_string(out.str());
}

static NativeValue native_benchmark_top_value_or_null(
    const NativeValue &result, const std::string &key) {
  const NativeValue *value = native_benchmark_map_get(result, key);
  return value == nullptr ? NativeValue::nullv() : *value;
}

static NativeValue native_benchmark_data_value_or_null(
    const NativeValue &result, const std::string &key) {
  const NativeValue *value = native_benchmark_map_get(native_benchmark_data(result), key);
  return value == nullptr ? NativeValue::nullv() : *value;
}

static NativeValue native_benchmark_period_field(const NativeValue &result,
                                                 const std::string &key) {
  return native_benchmark_time_period(
      native_benchmark_int_field(native_benchmark_data(result), key));
}

static NativeValue native_benchmark_sample_times(const NativeValue &result) {
  const NativeValue *raw = native_benchmark_map_get(native_benchmark_data(result), "sample_ns");
  if (raw == nullptr || raw->tag != NativeValue::Tag::List) throw NativeBailout();
  std::vector<NativeValue> periods;
  for (const NativeValue &sample : as_list(*raw).items) {
    if (sample.tag != NativeValue::Tag::Integer) throw NativeBailout();
    periods.push_back(native_benchmark_time_period(sample.scalar_value));
  }
  return NativeValue::list(std::move(periods));
}

static NativeValue native_benchmark_indexed_case(const NativeValue &result,
                                                 const std::string &index_key) {
  const NativeValue &data = native_benchmark_data(result);
  const std::int64_t index = native_benchmark_int_field(data, index_key);
  const NativeValue *cases_value = native_benchmark_map_get(data, "cases");
  if (cases_value == nullptr || cases_value->tag != NativeValue::Tag::List ||
      index < 0) {
    return NativeValue::nullv();
  }
  const auto &cases = as_list(*cases_value).items;
  if (static_cast<std::size_t>(index) >= cases.size()) return NativeValue::nullv();
  return cases[static_cast<std::size_t>(index)];
}

static NativeValue native_benchmark_profile_find(const NativeValue &profile,
                                                 const NativeValue &label_value) {
  const std::string label = native_benchmark_text(label_value);
  const NativeValue *spans_value =
      native_benchmark_map_get(native_benchmark_data(profile), "spans");
  if (spans_value == nullptr || spans_value->tag != NativeValue::Tag::List) {
    throw NativeBailout();
  }
  for (const NativeValue &span : as_list(*spans_value).items) {
    if (span.tag != NativeValue::Tag::Map) continue;
    const NativeValue *span_label = native_benchmark_map_get(span, "label");
    if (span_label != nullptr && native_benchmark_text(*span_label) == label) {
      return span;
    }
  }
  return NativeValue::nullv();
}

static NativeValue native_benchmark_compact_text(const NativeValue &result,
                                                 const std::string &kind) {
  const NativeValue &data = native_benchmark_data(result);
  std::string text;
  if (kind == "measurement") {
    text = "Benchmark.measurement(" + native_benchmark_label(result, 0) +
           ", elapsed=" +
           native_benchmark_format_ns(native_benchmark_int_field(data, "elapsed_ns")) +
           ")";
  } else if (kind == "report") {
    text = "Benchmark.report(" + native_benchmark_label(result, 0) +
           ", iterations=" +
           std::to_string(native_benchmark_int_field(data, "iterations")) +
           ", mean=" +
           native_benchmark_format_ns(native_benchmark_int_field(data, "mean_ns")) +
           ")";
  } else if (kind == "compare_report") {
    const NativeValue *cases_value = native_benchmark_map_get(data, "cases");
    const std::size_t count =
        (cases_value != nullptr && cases_value->tag == NativeValue::Tag::List)
            ? as_list(*cases_value).items.size()
            : 0U;
    text = "Benchmark.compare_report(cases=" + std::to_string(count) + ")";
  } else if (kind == "profile") {
    text = "Benchmark.profile(" + native_benchmark_label(result, 0) +
           ", total=" +
           native_benchmark_format_ns(native_benchmark_int_field(data, "total_ns")) +
           ")";
  } else {
    throw NativeBailout();
  }
  return native_benchmark_string(text);
}

static NativeValue native_benchmark_accessor(
    const NativeValue &result, const std::string &selector,
    const std::vector<NativeValue> &args) {
  const std::string kind = native_benchmark_kind(result);
  if (selector == "label") return native_benchmark_top_value_or_null(result, "label");
  if (selector == "kind") return native_benchmark_string(kind);
  if (selector == "data") return native_benchmark_data(result);
  if (selector == "value") return native_benchmark_top_value_or_null(result, "value");
  if (selector == "to_str" || selector == "inspect") {
    return native_benchmark_compact_text(result, kind);
  }

  if (kind == "measurement") {
    if (selector == "elapsed") return native_benchmark_period_field(result, "elapsed_ns");
    if (selector == "elapsed_ns" || selector == "iterations") {
      return native_benchmark_data_value_or_null(result, selector);
    }
  }

  if (kind == "report") {
    if (selector == "elapsed") return native_benchmark_period_field(result, "elapsed_ns");
    if (selector == "per_iteration") return native_benchmark_period_field(result, "per_iteration_ns");
    if (selector == "mean" || selector == "min" || selector == "max" ||
        selector == "p50" || selector == "p90" || selector == "p95" ||
        selector == "p99") {
      return native_benchmark_period_field(result, selector + "_ns");
    }
    if (selector == "sample_times") return native_benchmark_sample_times(result);
    if (selector == "iterations" || selector == "samples" ||
        selector == "elapsed_ns" || selector == "per_iteration_ns" ||
        selector == "mean_ns" || selector == "min_ns" ||
        selector == "max_ns" || selector == "p50_ns" ||
        selector == "p90_ns" || selector == "p95_ns" ||
        selector == "p99_ns" || selector == "ops_per_second" ||
        selector == "sample_ns") {
      return native_benchmark_data_value_or_null(result, selector);
    }
  }

  if (kind == "compare_report") {
    if (selector == "cases" || selector == "relative") {
      return native_benchmark_data_value_or_null(result, selector);
    }
    if (selector == "fastest") return native_benchmark_indexed_case(result, "fastest_index");
    if (selector == "slowest") return native_benchmark_indexed_case(result, "slowest_index");
  }

  if (kind == "profile") {
    if (selector == "total") return native_benchmark_period_field(result, "total_ns");
    if (selector == "total_ns" || selector == "spans" || selector == "summary") {
      return native_benchmark_data_value_or_null(result, selector);
    }
    if (selector == "find") {
      if (args.size() != 1U) throw NativeBailout();
      return native_benchmark_profile_find(result, args[0]);
    }
  }
  throw NativeBailout();
}

static NativeValue native_benchmark_send(
    const NativeValue &receiver, const std::string &selector,
    std::initializer_list<NativeValue> args_init,
    std::initializer_list<std::pair<std::string, NativeValue>> kwargs_init,
    const NativeValue &block_value, bool has_block) {
  const std::vector<NativeValue> args(args_init.begin(), args_init.end());
  const std::vector<std::pair<std::string, NativeValue>> kwargs(
      kwargs_init.begin(), kwargs_init.end());

  if (selector == "section") {
    if (!has_block || args.size() != 1U) throw NativeBailout();
    NativeBenchmarkProfileState *state = native_benchmark_active_profile(receiver);
    NativeBenchmarkProfileSpan span;
    span.label = native_benchmark_text(args[0]);
    if (const NativeValue *data = native_benchmark_kw(kwargs, "data")) span.data = *data;
    span.parent = state->stack.empty() ? -1 : static_cast<std::int64_t>(state->stack.back());
    span.depth = static_cast<std::int64_t>(state->stack.size());
    const std::size_t index = state->spans.size();
    state->spans.push_back(span);
    if (span.parent >= 0) {
      state->spans[static_cast<std::size_t>(span.parent)].children.push_back(index);
    }
    state->stack.push_back(index);
    const std::int64_t start = native_benchmark_now_ns();
    NativeValue value = amber_native_call_closure(block_value, {});
    const std::int64_t finish = native_benchmark_now_ns();
    state->stack.pop_back();
    state->spans[index].elapsed_ns = std::max<std::int64_t>(0, finish - start);
    return value;
  }

  const bool receiver_is_module =
      receiver.tag == NativeValue::Tag::BenchmarkModule;
  if (selector == "time" || selector == "measure") {
    if (!receiver_is_module || !has_block || args.size() > 1U ||
        native_benchmark_bool_kw(kwargs, "gc", false)) {
      throw NativeBailout();
    }
    const NativeValue label = native_benchmark_label_arg(args);
    const std::int64_t start = native_benchmark_now_ns();
    NativeValue value = amber_native_call_closure(block_value, {});
    const std::int64_t elapsed = std::max<std::int64_t>(0, native_benchmark_now_ns() - start);
    if (selector == "time") return native_benchmark_time_period(elapsed);
    return native_benchmark_measurement(label, elapsed, value, "measurement");
  }
  if (selector == "run") {
    if (!receiver_is_module || !has_block || args.size() > 1U ||
        native_benchmark_bool_kw(kwargs, "gc", false)) {
      throw NativeBailout();
    }
    const NativeValue label = native_benchmark_label_arg(args);
    std::int64_t iterations =
        native_benchmark_int_kw(kwargs, "iterations", 1, 0);
    const std::int64_t warmup = native_benchmark_int_kw(kwargs, "warmup", 0, 0);
    const std::int64_t samples = native_benchmark_int_kw(kwargs, "samples", 1, 1);
    const std::int64_t min_time_ns =
        native_benchmark_period_kw_ns(kwargs, "min_time");
    if (iterations == 0 && min_time_ns == 0) throw NativeBailout();
    iterations = std::max<std::int64_t>(1, iterations);
    for (std::int64_t i = 0; i < warmup; ++i) {
      (void)amber_native_call_closure(block_value, {NativeValue::integer(i)});
    }
    std::vector<std::int64_t> sample_ns;
    for (std::int64_t sample = 0; sample < samples; ++sample) {
      std::int64_t elapsed = 0;
      do {
        const std::int64_t start = native_benchmark_now_ns();
        for (std::int64_t i = 0; i < iterations; ++i) {
          (void)amber_native_call_closure(block_value, {NativeValue::integer(i)});
        }
        elapsed = std::max<std::int64_t>(0, native_benchmark_now_ns() - start);
        if (min_time_ns > 0 && elapsed < min_time_ns) {
          iterations = std::max<std::int64_t>(iterations + 1, iterations * 2);
        }
      } while (min_time_ns > 0 && elapsed < min_time_ns);
      sample_ns.push_back(elapsed);
    }
    return native_benchmark_report(label, iterations, sample_ns);
  }
  if (selector == "profile") {
    if (!receiver_is_module || !has_block || args.size() > 1U ||
        native_benchmark_bool_kw(kwargs, "gc", false)) {
      throw NativeBailout();
    }
    const NativeValue label = native_benchmark_label_arg(args);
    NativeBenchmarkProfileState state;
    state.id = native_benchmark_next_profile_id++;
    const NativeValue profiler = native_benchmark_profiler_map(state.id);
    native_benchmark_profiles.push_back(&state);
    const std::int64_t start = native_benchmark_now_ns();
    NativeValue value = amber_native_call_closure(block_value, {profiler});
    const std::int64_t total = std::max<std::int64_t>(0, native_benchmark_now_ns() - start);
    native_benchmark_profiles.pop_back();
    native_benchmark_finalize_profile(&state);
    NativeValue data = native_benchmark_map({
        {"total_ns", NativeValue::integer(total)},
        {"total_text", native_benchmark_string(native_benchmark_format_ns(total))},
        {"spans", native_benchmark_profile_spans(state)},
        {"summary", native_benchmark_profile_summary(state)},
    });
    return native_benchmark_result("profile", label, data, value, true);
  }
  if (selector == "compare") {
    if (!receiver_is_module || has_block) throw NativeBailout();
    return native_benchmark_compare(args);
  }
  if (selector == "from_map") {
    if (!receiver_is_module || args.size() != 1U) throw NativeBailout();
    return native_benchmark_sanitize(args[0]);
  }
  if (selector == "from_json") {
    if (!receiver_is_module || args.size() != 1U ||
        !native_value_is_string(args[0])) {
      throw NativeBailout();
    }
    return native_benchmark_sanitize(native_json_parse_value(args[0]));
  }

  const bool result_accessor =
      selector == "label" || selector == "kind" || selector == "data" ||
      selector == "value" || selector == "elapsed" ||
      selector == "elapsed_ns" || selector == "iterations" ||
      selector == "samples" || selector == "per_iteration" ||
      selector == "per_iteration_ns" || selector == "mean" ||
      selector == "mean_ns" || selector == "min" || selector == "min_ns" ||
      selector == "max" || selector == "max_ns" || selector == "p50" ||
      selector == "p50_ns" || selector == "p90" || selector == "p90_ns" ||
      selector == "p95" || selector == "p95_ns" || selector == "p99" ||
      selector == "p99_ns" || selector == "ops_per_second" ||
      selector == "sample_times" || selector == "sample_ns" ||
      selector == "cases" || selector == "fastest" ||
      selector == "slowest" || selector == "relative" ||
      selector == "total" || selector == "total_ns" ||
      selector == "spans" || selector == "summary" ||
      selector == "find" || selector == "to_str" || selector == "inspect";
  if (result_accessor) {
    if (receiver_is_module) {
      if (args.empty()) throw NativeBailout();
      std::vector<NativeValue> accessor_args(args.begin() + 1, args.end());
      return native_benchmark_accessor(args[0], selector, accessor_args);
    }
    if (selector != "find" && !args.empty()) throw NativeBailout();
    if (!native_benchmark_has_schema(receiver, kNativeBenchmarkSchema)) {
      // Not a benchmark result: shared selectors keep the generic
      // tag-dispatch semantics instead of bailing out (inspect/to_str via
      // the tag-generic Time dispatcher, min/max via the sequence extremum
      // family). Selectors with no generic native owner (label, value,
      // kind, find, ...) still bail so the VM reports the send error.
      if (selector == "inspect") {
        return native_time_nullary(receiver, NativeTimeSelector::Inspect);
      }
      if (selector == "to_str") {
        return native_time_nullary(receiver, NativeTimeSelector::ToStr);
      }
      if (selector == "min" || selector == "max") {
        return native_sequence_extreme(receiver, selector);
      }
      throw NativeBailout();
    }
    return native_benchmark_accessor(receiver, selector, args);
  }

  NativeValue result = receiver;
  if (receiver_is_module) {
    if (args.size() != 1U) throw NativeBailout();
    result = args[0];
  } else if (!args.empty()) {
    throw NativeBailout();
  }
  if (selector == "map" || selector == "to_map") {
    const NativeValue *mode = native_benchmark_kw(kwargs, "value");
    if (mode != nullptr && native_benchmark_text(*mode) == "raw") return result;
    return native_benchmark_sanitize(result);
  }
  if (selector == "to_json") {
    if (!native_benchmark_has_schema(result, kNativeBenchmarkSchema)) {
      const bool pretty = native_benchmark_bool_kw(kwargs, "pretty", false);
      return native_json_generate_value(result, pretty);
    }
    const bool pretty = native_benchmark_bool_kw(kwargs, "pretty", false);
    return native_json_generate_value(native_benchmark_sanitize(result), pretty);
  }
  if (selector == "format" || selector == "table" || selector == "pretty") {
    return native_benchmark_format(result, kwargs, selector == "table" ? "table" : "summary");
  }
  throw NativeBailout();
}

)AMBERCPP";
  out << "static NativeValue native_math_send(const NativeValue &module, "
         "const std::string &selector, "
         "std::initializer_list<NativeValue> args) {\n";
  out << "  if (module.tag != NativeValue::Tag::MathModule) "
         "throw NativeBailout();\n";
  out << "  const NativeValue *argv = args.begin();\n";
  out << "  if (selector == \"PI\" && args.size() == 0U) return "
         "NativeValue::floating(3.141592653589793238462643383279502884);\n";
  out << "  if (selector == \"E\" && args.size() == 0U) return "
         "NativeValue::floating(2.718281828459045235360287471352662498);\n";
  out << "  const auto math_arg = [&](std::size_t index) -> double {\n";
  out << "    if (index >= args.size() || !numeric_tag(argv[index])) "
         "throw NativeBailout();\n";
  out << "    return as_double_numeric(argv[index]);\n";
  out << "  };\n";
  out << "  if (selector == \"abs\" && args.size() == 1U) {\n";
  out << "    const NativeValue &value = argv[0];\n";
  out << "    if (value.tag == NativeValue::Tag::Integer) {\n";
  out << "      if (value.scalar_value == "
         "std::numeric_limits<std::int64_t>::min()) throw NativeBailout();\n";
  out << "      return value.scalar_value < 0 ? "
         "NativeValue::integer(-value.scalar_value) : value;\n";
  out << "    }\n";
  out << "    return NativeValue::floating(std::fabs(math_arg(0U)));\n";
  out << "  }\n";
  out << "  if (selector == \"sign\" && args.size() == 1U) {\n";
  out << "    const double value = math_arg(0U);\n";
  out << "    return NativeValue::integer(value > 0.0 ? 1 : "
         "(value < 0.0 ? -1 : 0));\n";
  out << "  }\n";
  out << "  if ((selector == \"min\" || selector == \"max\") && "
         "args.size() == 2U) {\n";
  out << "    const double lhs = math_arg(0U);\n";
  out << "    const double rhs = math_arg(1U);\n";
  out << "    const bool pick_first = selector == \"min\" ? "
         "(lhs <= rhs) : (lhs >= rhs);\n";
  out << "    return pick_first ? argv[0] : argv[1];\n";
  out << "  }\n";
  out << "  if ((selector == \"pow\" || selector == \"hypot\" || "
         "selector == \"atan2\") && args.size() == 2U) {\n";
  out << "    const double lhs = math_arg(0U);\n";
  out << "    const double rhs = math_arg(1U);\n";
  out << "    if (selector == \"pow\") return "
         "NativeValue::floating(std::pow(lhs, rhs));\n";
  out << "    if (selector == \"hypot\") return "
         "NativeValue::floating(std::hypot(lhs, rhs));\n";
  out << "    return NativeValue::floating(std::atan2(lhs, rhs));\n";
  out << "  }\n";
  out << "  if (args.size() != 1U) throw NativeBailout();\n";
  out << "  const double value = math_arg(0U);\n";
  out << "  if (selector == \"sqrt\") return "
         "NativeValue::floating(std::sqrt(value));\n";
  out << "  if (selector == \"cbrt\") return "
         "NativeValue::floating(std::cbrt(value));\n";
  out << "  if (selector == \"exp\") return "
         "NativeValue::floating(std::exp(value));\n";
  out << "  if (selector == \"log2\") return "
         "NativeValue::floating(std::log2(value));\n";
  out << "  if (selector == \"log10\") return "
         "NativeValue::floating(std::log10(value));\n";
  out << "  if (selector == \"sin\") return "
         "NativeValue::floating(std::sin(value));\n";
  out << "  if (selector == \"cos\") return "
         "NativeValue::floating(std::cos(value));\n";
  out << "  if (selector == \"tan\") return "
         "NativeValue::floating(std::tan(value));\n";
  out << "  if (selector == \"asin\") return "
         "NativeValue::floating(std::asin(value));\n";
  out << "  if (selector == \"acos\") return "
         "NativeValue::floating(std::acos(value));\n";
  out << "  if (selector == \"atan\") return "
         "NativeValue::floating(std::atan(value));\n";
  out << "  if (selector == \"floor\") return "
         "NativeValue::floating(std::floor(value));\n";
  out << "  if (selector == \"ceil\") return "
         "NativeValue::floating(std::ceil(value));\n";
  out << "  if (selector == \"round\") return "
         "NativeValue::floating(std::round(value));\n";
  out << "  if (selector == \"trunc\") return "
         "NativeValue::floating(std::trunc(value));\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_add(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Time || "
         "lhs.tag == NativeValue::Tag::TimePeriod || "
         "rhs.tag == NativeValue::Tag::Time || "
         "rhs.tag == NativeValue::Tag::TimePeriod) return "
         "native_time_add(lhs, rhs);\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) return "
         "NativeValue::integer(profile_add_int64(lhs.scalar_value, "
         "rhs.scalar_value));\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) return "
         "NativeValue::floating(as_double_numeric(lhs) + "
         "as_double_numeric(rhs));\n";
  out << "  if (native_value_is_string(lhs)) return "
         "native_string_concat(lhs, rhs);\n";
  out << "  if (native_is_sequence(lhs)) return "
         "native_sequence_concat(lhs, rhs);\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_sub(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Time || "
         "lhs.tag == NativeValue::Tag::TimePeriod || "
         "rhs.tag == NativeValue::Tag::Time || "
         "rhs.tag == NativeValue::Tag::TimePeriod) return "
         "native_time_sub(lhs, rhs);\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) return "
         "NativeValue::integer(profile_sub_int64(lhs.scalar_value, "
         "rhs.scalar_value));\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) return "
         "NativeValue::floating(as_double_numeric(lhs) - "
         "as_double_numeric(rhs));\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_mul(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) return "
         "NativeValue::integer(profile_mul_int64(lhs.scalar_value, "
         "rhs.scalar_value));\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) return "
         "NativeValue::floating(as_double_numeric(lhs) * "
         "as_double_numeric(rhs));\n";
  out << "  if (native_is_sequence(lhs)) return "
         "native_sequence_repeat(lhs, rhs);\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_pow(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) {\n";
  out << "    if (rhs.scalar_value < 0) return NativeValue::floating("
         "std::pow(static_cast<double>(lhs.scalar_value), "
         "static_cast<double>(rhs.scalar_value)));\n";
  out << "    return NativeValue::integer(profile_pow_int64(lhs.scalar_value, "
         "rhs.scalar_value));\n";
  out << "  }\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) return "
         "NativeValue::floating(std::pow(as_double_numeric(lhs), "
         "as_double_numeric(rhs)));\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_div(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) return "
         "NativeValue::integer(profile_div_int64(lhs.scalar_value, "
         "rhs.scalar_value));\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) {\n";
  out << "    const double divisor = as_double_numeric(rhs);\n";
  out << "    if (divisor == 0.0) throw NativeBailout();\n";
  out << "    return NativeValue::floating(as_double_numeric(lhs) / "
         "divisor);\n";
  out << "  }\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_mod(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) {\n";
  out << "    if (rhs.scalar_value == 0) throw NativeBailout();\n";
  out << "    return NativeValue::integer(floor_mod_int64(lhs.scalar_value, "
         "rhs.scalar_value));\n";
  out << "  }\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) {\n";
  out << "    const double divisor = as_double_numeric(rhs);\n";
  out << "    if (divisor == 0.0) throw NativeBailout();\n";
  out << "    return NativeValue::floating(floor_mod_double_native("
         "as_double_numeric(lhs), divisor));\n";
  out << "  }\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_floor_div(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) return "
         "NativeValue::integer(profile_floor_div_int64(lhs.scalar_value, "
         "rhs.scalar_value));\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) {\n";
  out << "    const double divisor = as_double_numeric(rhs);\n";
  out << "    if (divisor == 0.0) throw NativeBailout();\n";
  out << "    return NativeValue::floating(std::floor(as_double_numeric(lhs) "
         "/ divisor));\n";
  out << "  }\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_lt(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Time || "
         "lhs.tag == NativeValue::Tag::TimePeriod || "
         "rhs.tag == NativeValue::Tag::Time || "
         "rhs.tag == NativeValue::Tag::TimePeriod) return "
         "NativeValue::boolean("
         "native_time_compare_value(lhs, rhs).scalar_value < 0);\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) return "
         "NativeValue::boolean(lhs.scalar_value < rhs.scalar_value);\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) return "
         "NativeValue::boolean(as_double_numeric(lhs) < "
         "as_double_numeric(rhs));\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_gt(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Time || "
         "lhs.tag == NativeValue::Tag::TimePeriod || "
         "rhs.tag == NativeValue::Tag::Time || "
         "rhs.tag == NativeValue::Tag::TimePeriod) return "
         "NativeValue::boolean("
         "native_time_compare_value(lhs, rhs).scalar_value > 0);\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) return "
         "NativeValue::boolean(lhs.scalar_value > rhs.scalar_value);\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) return "
         "NativeValue::boolean(as_double_numeric(lhs) > "
         "as_double_numeric(rhs));\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_le(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Time || "
         "lhs.tag == NativeValue::Tag::TimePeriod || "
         "rhs.tag == NativeValue::Tag::Time || "
         "rhs.tag == NativeValue::Tag::TimePeriod) return "
         "NativeValue::boolean("
         "native_time_compare_value(lhs, rhs).scalar_value <= 0);\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) return "
         "NativeValue::boolean(lhs.scalar_value <= rhs.scalar_value);\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) return "
         "NativeValue::boolean(as_double_numeric(lhs) <= "
         "as_double_numeric(rhs));\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_ge(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Time || "
         "lhs.tag == NativeValue::Tag::TimePeriod || "
         "rhs.tag == NativeValue::Tag::Time || "
         "rhs.tag == NativeValue::Tag::TimePeriod) return "
         "NativeValue::boolean("
         "native_time_compare_value(lhs, rhs).scalar_value >= 0);\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) return "
         "NativeValue::boolean(lhs.scalar_value >= rhs.scalar_value);\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) return "
         "NativeValue::boolean(as_double_numeric(lhs) >= "
         "as_double_numeric(rhs));\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_eq(const NativeValue &lhs, "
         "const NativeValue &rhs, bool negate) {\n";
  // Mirrors value_equals for scalar kinds: numeric pairs compare by value
  // (Int/Int exact, otherwise double); other scalar pairs require matching
  // tags; deep kinds (lists, closures) stay on the VM.
  out << "  if ((lhs.tag == NativeValue::Tag::Time && "
         "rhs.tag == NativeValue::Tag::Time) || "
         "(lhs.tag == NativeValue::Tag::TimePeriod && "
         "rhs.tag == NativeValue::Tag::TimePeriod)) {\n";
  out << "    const bool equal = "
         "native_time_compare_value(lhs, rhs).scalar_value == 0;\n";
  out << "    return NativeValue::boolean(negate ? !equal : equal);\n";
  out << "  }\n";
  out << "  if (lhs.tag == NativeValue::Tag::Uuid && "
         "rhs.tag == NativeValue::Tag::Uuid) {\n";
  out << "    const bool equal = as_uuid(lhs).bytes == as_uuid(rhs).bytes;\n";
  out << "    return NativeValue::boolean(negate ? !equal : equal);\n";
  out << "  }\n";
  out << "  if (lhs.tag == NativeValue::Tag::Regexp && "
         "rhs.tag == NativeValue::Tag::Regexp) {\n";
  out << "    const NativeRegexp &left = as_regexp(lhs);\n";
  out << "    const NativeRegexp &right = as_regexp(rhs);\n";
  out << "    const bool equal = left.source == right.source && "
         "left.flags == right.flags;\n";
  out << "    return NativeValue::boolean(negate ? !equal : equal);\n";
  out << "  }\n";
  out << "  if (lhs.tag == NativeValue::Tag::RegexpMatch && "
         "rhs.tag == NativeValue::Tag::RegexpMatch) {\n";
  out << "    const bool equal = lhs.heap_value == rhs.heap_value;\n";
  out << "    return NativeValue::boolean(negate ? !equal : equal);\n";
  out << "  }\n";
  out << "  if (lhs.tag == NativeValue::Tag::List || "
         "lhs.tag == NativeValue::Tag::Tuple || "
         "lhs.tag == NativeValue::Tag::Set || "
         "lhs.tag == NativeValue::Tag::Closure || "
         "lhs.tag == NativeValue::Tag::Map || "
         "lhs.tag == NativeValue::Tag::MathModule || "
         "lhs.tag == NativeValue::Tag::JsonModule || "
         "lhs.tag == NativeValue::Tag::YamlModule || "
         "lhs.tag == NativeValue::Tag::BytesModule || "
         "lhs.tag == NativeValue::Tag::Base64Module || "
         "lhs.tag == NativeValue::Tag::Base64UrlModule || "
         "lhs.tag == NativeValue::Tag::HexModule || "
         "lhs.tag == NativeValue::Tag::DigestModule || "
         "lhs.tag == NativeValue::Tag::BenchmarkModule || "
         "lhs.tag == NativeValue::Tag::UrlModule || "
         "lhs.tag == NativeValue::Tag::ArgParserModule || "
         "lhs.tag == NativeValue::Tag::FsModule || "
         "lhs.tag == NativeValue::Tag::SecureRandomModule || "
         "lhs.tag == NativeValue::Tag::RangeModule || "
         "lhs.tag == NativeValue::Tag::TimeModule || "
         "lhs.tag == NativeValue::Tag::TimePeriodModule || "
         "lhs.tag == NativeValue::Tag::Bytes || "
         "lhs.tag == NativeValue::Tag::Range || "
         "lhs.tag == NativeValue::Tag::ArgParser || "
         "lhs.tag == NativeValue::Tag::FsPath || "
         "lhs.tag == NativeValue::Tag::Time || "
         "lhs.tag == NativeValue::Tag::TimePeriod || "
         "rhs.tag == NativeValue::Tag::List || "
         "rhs.tag == NativeValue::Tag::Tuple || "
         "rhs.tag == NativeValue::Tag::Set || "
         "rhs.tag == NativeValue::Tag::Map || "
         "rhs.tag == NativeValue::Tag::MathModule || "
         "rhs.tag == NativeValue::Tag::JsonModule || "
         "rhs.tag == NativeValue::Tag::YamlModule || "
         "rhs.tag == NativeValue::Tag::BytesModule || "
         "rhs.tag == NativeValue::Tag::Base64Module || "
         "rhs.tag == NativeValue::Tag::Base64UrlModule || "
         "rhs.tag == NativeValue::Tag::HexModule || "
         "rhs.tag == NativeValue::Tag::DigestModule || "
         "rhs.tag == NativeValue::Tag::BenchmarkModule || "
         "rhs.tag == NativeValue::Tag::UrlModule || "
         "rhs.tag == NativeValue::Tag::ArgParserModule || "
         "rhs.tag == NativeValue::Tag::FsModule || "
         "rhs.tag == NativeValue::Tag::SecureRandomModule || "
         "rhs.tag == NativeValue::Tag::RangeModule || "
         "rhs.tag == NativeValue::Tag::TimeModule || "
         "rhs.tag == NativeValue::Tag::TimePeriodModule || "
         "rhs.tag == NativeValue::Tag::Bytes || "
         "rhs.tag == NativeValue::Tag::Range || "
         "rhs.tag == NativeValue::Tag::ArgParser || "
         "rhs.tag == NativeValue::Tag::FsPath || "
         "rhs.tag == NativeValue::Tag::Time || "
         "rhs.tag == NativeValue::Tag::TimePeriod || "
         "rhs.tag == NativeValue::Tag::Closure) throw NativeBailout();\n";
  out << "  bool equal;\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) equal = "
         "lhs.scalar_value == rhs.scalar_value;\n";
  out << "  else if (numeric_tag(lhs) && numeric_tag(rhs)) equal = "
         "as_double_numeric(lhs) == as_double_numeric(rhs);\n";
  out << "  else if (native_value_is_string(lhs) && "
         "native_value_is_string(rhs)) equal = "
         "native_string_text_equal(lhs, rhs);\n";
  out << "  else if (lhs.tag != rhs.tag) equal = false;\n";
  out << "  else if (lhs.tag == NativeValue::Tag::Null) equal = true;\n";
  out << "  else equal = lhs.scalar_value == rhs.scalar_value;\n";
  out << "  return NativeValue::boolean(negate ? !equal : equal);\n";
  out << "}\n\n";
  out << "static bool native_parse_int_text(const std::string &text, "
         "std::int64_t *out_value) {\n";
  out << "  if (text.empty()) return false;\n";
  out << "  const char *begin = text.data();\n";
  out << "  const char *end = text.data() + text.size();\n";
  out << "  const auto parsed = std::from_chars(begin, end, *out_value, 10);\n";
  out << "  return parsed.ec == std::errc{} && parsed.ptr == end;\n";
  out << "}\n\n";
  out << "static bool native_parse_float_text(const std::string &text, "
         "double *out_value) {\n";
  out << "  if (text.empty()) return false;\n";
  out << "  for (unsigned char c : text) {\n";
  out << "    if (std::isspace(c) != 0) return false;\n";
  out << "  }\n";
  out << "  try {\n";
  out << "    std::size_t consumed = 0;\n";
  out << "    const double parsed = std::stod(text, &consumed);\n";
  out << "    if (consumed != text.size()) return false;\n";
  out << "    *out_value = parsed;\n";
  out << "    return true;\n";
  out << "  } catch (const std::exception &) {\n";
  out << "    return false;\n";
  out << "  }\n";
  out << "}\n\n";
  out << "static std::string native_value_to_debug_string("
         "const NativeValue &value);\n";
  out << "static NativeValue native_to_str(const NativeValue &value) {\n";
  out << "  if (value.tag == NativeValue::Tag::Map && "
         "native_benchmark_has_schema(value, kNativeBenchmarkSchema)) {\n";
  out << "    return native_benchmark_compact_text(value, "
         "native_benchmark_kind(value));\n";
  out << "  }\n";
  out << "  switch (value.tag) {\n";
  out << "  case NativeValue::Tag::String:\n";
  out << "  case NativeValue::Tag::HeapString: return value;\n";
  out << "  case NativeValue::Tag::Symbol: return "
         "NativeValue::string_ref(native_intern_string("
         "native_symbol_text(value.scalar_value)));\n";
  out << "  case NativeValue::Tag::Null: return "
         "NativeValue::string_ref(native_intern_string(\"null\"));\n";
  out << "  case NativeValue::Tag::Bool: return "
         "NativeValue::string_ref(native_intern_string("
         "value.scalar_value != 0 ? \"true\" : \"false\"));\n";
  out << "  case NativeValue::Tag::Integer: return "
         "NativeValue::heap_string("
         "std::to_string(value.scalar_value));\n";
  // Float to_str uses shortest-round-trip formatting (display_float_text),
  // unlike the precision-6 debug print.
  out << "  case NativeValue::Tag::Float: {\n";
  out << "    char buffer[128];\n";
  out << "    const auto converted = std::to_chars(buffer, "
         "buffer + sizeof(buffer), value.float_value);\n";
  out << "    if (converted.ec != std::errc{}) throw NativeBailout();\n";
  out << "    return NativeValue::heap_string("
         "std::string(buffer, converted.ptr));\n";
  out << "  }\n";
  out << "  case NativeValue::Tag::Bytes: return "
         "NativeValue::heap_string(as_bytes(value).bytes);"
         "\n";
  out << "  case NativeValue::Tag::FsPath: return "
         "NativeValue::heap_string(as_fs_path(value).path);"
         "\n";
  out << "  case NativeValue::Tag::Regexp: return "
         "NativeValue::heap_string(native_regexp_pattern_to_string("
         "as_regexp(value)));"
         "\n";
  out << "  case NativeValue::Tag::RegexpMatch: return "
         "NativeValue::heap_string(native_regexp_match_to_string("
         "as_regexp_match(value)));"
         "\n";
  out << "  case NativeValue::Tag::List:\n";
  out << "  case NativeValue::Tag::Tuple:\n";
  out << "  case NativeValue::Tag::Set:\n";
  out << "  case NativeValue::Tag::Map:\n";
  out << "  case NativeValue::Tag::Range:\n";
  out << "  case NativeValue::Tag::ArgParser:\n";
  out << "    return NativeValue::heap_string("
         "native_value_to_debug_string(value));\n";
  out << "  case NativeValue::Tag::StrType:\n";
  out << "  case NativeValue::Tag::IntType:\n";
  out << "  case NativeValue::Tag::BigIntType:\n";
  out << "  case NativeValue::Tag::FloatType:\n";
  out << "  case NativeValue::Tag::BoolType:\n";
  out << "  case NativeValue::Tag::SymbolType:\n";
  out << "  case NativeValue::Tag::NullType:\n";
  out << "  case NativeValue::Tag::ObjectType:\n";
  out << "  case NativeValue::Tag::MathModule:\n";
  out << "  case NativeValue::Tag::JsonModule:\n";
  out << "  case NativeValue::Tag::YamlModule:\n";
  out << "  case NativeValue::Tag::BytesModule:\n";
  out << "  case NativeValue::Tag::Base64Module:\n";
  out << "  case NativeValue::Tag::Base64UrlModule:\n";
  out << "  case NativeValue::Tag::HexModule:\n";
  out << "  case NativeValue::Tag::DigestModule:\n";
  out << "  case NativeValue::Tag::BenchmarkModule:\n";
  out << "  case NativeValue::Tag::UrlModule:\n";
  out << "  case NativeValue::Tag::ArgParserModule:\n";
  out << "  case NativeValue::Tag::RegexpModule:\n";
  out << "  case NativeValue::Tag::FsModule:\n";
  out << "  case NativeValue::Tag::SecureRandomModule:\n";
  out << "  case NativeValue::Tag::UuidModule:\n";
  out << "  case NativeValue::Tag::RangeModule:\n";
  out << "  case NativeValue::Tag::TimeModule:\n";
  out << "  case NativeValue::Tag::TimePeriodModule:\n";
  out << "    return NativeValue::string_ref(native_intern_string("
         "std::string(\"<type \") + native_type_tag_name(value) + \">\"));\n";
  out << "  case NativeValue::Tag::Uuid:\n";
  out << "    return native_uuid_nullary(value, \"to_str\");\n";
  out << "  case NativeValue::Tag::Time:\n";
  out << "  case NativeValue::Tag::TimePeriod:\n";
  out << "    return native_time_nullary(value, NativeTimeSelector::ToStr);\n";
  out << "  default: throw NativeBailout();\n";
  out << "  }\n";
  out << "}\n\n";
  out << "static NativeValue native_to_int(const NativeValue &value) {\n";
  out << "  if (value.tag == NativeValue::Tag::Integer) return value;\n";
  out << "  if (value.tag == NativeValue::Tag::Float) {\n";
  out << "    if (!std::isfinite(value.float_value)) throw NativeBailout();\n";
  out << "    const long double raw = "
         "static_cast<long double>(value.float_value);\n";
  out << "    if (raw < static_cast<long double>("
         "std::numeric_limits<std::int64_t>::min()) || raw > "
         "static_cast<long double>(std::numeric_limits<std::int64_t>::max())) "
         "throw NativeBailout();\n";
  out << "    const std::int64_t as_int = "
         "static_cast<std::int64_t>(value.float_value);\n";
  out << "    if (static_cast<double>(as_int) != value.float_value) "
         "throw NativeBailout();\n";
  out << "    return NativeValue::integer(as_int);\n";
  out << "  }\n";
  out << "  if (native_value_is_string(value)) {\n";
  out << "    std::int64_t parsed = 0;\n";
  out << "    if (!native_parse_int_text(native_string_text(value), &parsed)) "
         "throw NativeBailout();\n";
  out << "    return NativeValue::integer(parsed);\n";
  out << "  }\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue native_to_float(const NativeValue &value) {\n";
  out << "  if (value.tag == NativeValue::Tag::Float) return value;\n";
  out << "  if (value.tag == NativeValue::Tag::Integer) return "
         "NativeValue::floating(static_cast<double>(value.scalar_value));\n";
  out << "  if (native_value_is_string(value)) {\n";
  out << "    double parsed = 0.0;\n";
  out << "    if (!native_parse_float_text(native_string_text(value), "
         "&parsed)) "
         "throw NativeBailout();\n";
  out << "    return NativeValue::floating(parsed);\n";
  out << "  }\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue native_to_bool(const NativeValue &value) {\n";
  out << "  if (value.tag == NativeValue::Tag::Bool) return value;\n";
  out << "  if (native_value_is_string(value)) {\n";
  out << "    const std::string &text = native_string_text(value);\n";
  out << "    if (text == \"true\") return NativeValue::boolean(true);\n";
  out << "    if (text == \"false\") return NativeValue::boolean(false);\n";
  out << "  }\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue native_to_symbol(const NativeValue &value) {\n";
  out << "  if (value.tag == NativeValue::Tag::Symbol) return value;\n";
  out << "  if (native_value_is_string(value)) {\n";
  out << "    const std::string &text = native_string_text(value);\n";
  out << "    if (text.empty()) throw NativeBailout();\n";
  out << "    return NativeValue::symbol_ref(native_intern_symbol(text));\n";
  out << "  }\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << "static NativeValue numeric_cmp(const NativeValue &lhs, "
         "const NativeValue &rhs) {\n";
  out << "  if (lhs.tag == NativeValue::Tag::Time || "
         "lhs.tag == NativeValue::Tag::TimePeriod || "
         "rhs.tag == NativeValue::Tag::Time || "
         "rhs.tag == NativeValue::Tag::TimePeriod) return "
         "native_time_compare_value(lhs, rhs);\n";
  out << "  if (lhs.tag == NativeValue::Tag::Integer && "
         "rhs.tag == NativeValue::Tag::Integer) return "
         "NativeValue::integer(compare_int64(lhs.scalar_value, "
         "rhs.scalar_value));\n";
  out << "  if (numeric_tag(lhs) && numeric_tag(rhs)) return "
         "NativeValue::integer(compare_double_native("
         "as_double_numeric(lhs), as_double_numeric(rhs)));\n";
  out << "  throw NativeBailout();\n";
  out << "}\n\n";
  out << R"AMBERCPP(static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_add(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(
        profile_add_int64(lhs.scalar_value, rhs.scalar_value));
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    return NativeValue::floating(as_double_numeric(lhs) +
                                 as_double_numeric(rhs));
  }
  return numeric_add(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_sub(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(
        profile_sub_int64(lhs.scalar_value, rhs.scalar_value));
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    return NativeValue::floating(as_double_numeric(lhs) -
                                 as_double_numeric(rhs));
  }
  return numeric_sub(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_mul(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(
        profile_mul_int64(lhs.scalar_value, rhs.scalar_value));
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    return NativeValue::floating(as_double_numeric(lhs) *
                                 as_double_numeric(rhs));
  }
  return numeric_mul(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_div(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(
        profile_div_int64(lhs.scalar_value, rhs.scalar_value));
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    const double divisor = as_double_numeric(rhs);
    if (divisor == 0.0) throw NativeBailout();
    return NativeValue::floating(as_double_numeric(lhs) / divisor);
  }
  return numeric_div(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_mod(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    if (rhs.scalar_value == 0) throw NativeBailout();
    return NativeValue::integer(
        floor_mod_int64(lhs.scalar_value, rhs.scalar_value));
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    const double divisor = as_double_numeric(rhs);
    if (divisor == 0.0) throw NativeBailout();
    return NativeValue::floating(
        floor_mod_double_native(as_double_numeric(lhs), divisor));
  }
  return numeric_mod(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_floor_div(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(
        profile_floor_div_int64(lhs.scalar_value, rhs.scalar_value));
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    const double divisor = as_double_numeric(rhs);
    if (divisor == 0.0) throw NativeBailout();
    return NativeValue::floating(std::floor(as_double_numeric(lhs) / divisor));
  }
  return numeric_floor_div(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_pow(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    if (rhs.scalar_value < 0) {
      return NativeValue::floating(std::pow(
          static_cast<double>(lhs.scalar_value),
          static_cast<double>(rhs.scalar_value)));
    }
    return NativeValue::integer(
        profile_pow_int64(lhs.scalar_value, rhs.scalar_value));
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    return NativeValue::floating(
        std::pow(as_double_numeric(lhs), as_double_numeric(rhs)));
  }
  return numeric_pow(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_lt(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::boolean(lhs.scalar_value < rhs.scalar_value);
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    return NativeValue::boolean(as_double_numeric(lhs) <
                                as_double_numeric(rhs));
  }
  return numeric_lt(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_gt(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::boolean(lhs.scalar_value > rhs.scalar_value);
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    return NativeValue::boolean(as_double_numeric(lhs) >
                                as_double_numeric(rhs));
  }
  return numeric_gt(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_le(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::boolean(lhs.scalar_value <= rhs.scalar_value);
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    return NativeValue::boolean(as_double_numeric(lhs) <=
                                as_double_numeric(rhs));
  }
  return numeric_le(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_ge(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::boolean(lhs.scalar_value >= rhs.scalar_value);
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    return NativeValue::boolean(as_double_numeric(lhs) >=
                                as_double_numeric(rhs));
  }
  return numeric_ge(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_eq(
    const NativeValue &lhs, const NativeValue &rhs, bool negate) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    const bool equal = lhs.scalar_value == rhs.scalar_value;
    return NativeValue::boolean(negate ? !equal : equal);
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    const bool equal = as_double_numeric(lhs) == as_double_numeric(rhs);
    return NativeValue::boolean(negate ? !equal : equal);
  }
  return numeric_eq(lhs, rhs, negate);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_cmp(
    const NativeValue &lhs, const NativeValue &rhs) {
  if (lhs.tag == NativeValue::Tag::Integer &&
      rhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(compare_int64(lhs.scalar_value,
                                              rhs.scalar_value));
  }
  if (numeric_tag(lhs) && numeric_tag(rhs)) {
    return NativeValue::integer(compare_double_native(
        as_double_numeric(lhs), as_double_numeric(rhs)));
  }
  return numeric_cmp(lhs, rhs);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_add_int_rhs(
    const NativeValue &lhs, std::int64_t rhs) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(profile_add_int64(lhs.scalar_value, rhs));
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    return NativeValue::floating(lhs.float_value + static_cast<double>(rhs));
  }
  return numeric_add(lhs, NativeValue::integer(rhs));
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_sub_int_rhs(
    const NativeValue &lhs, std::int64_t rhs) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(profile_sub_int64(lhs.scalar_value, rhs));
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    return NativeValue::floating(lhs.float_value - static_cast<double>(rhs));
  }
  return numeric_sub(lhs, NativeValue::integer(rhs));
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_mul_int_rhs(
    const NativeValue &lhs, std::int64_t rhs) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(profile_mul_int64(lhs.scalar_value, rhs));
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    return NativeValue::floating(lhs.float_value * static_cast<double>(rhs));
  }
  return numeric_mul(lhs, NativeValue::integer(rhs));
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_div_int_rhs(
    const NativeValue &lhs, std::int64_t rhs) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(profile_div_int64(lhs.scalar_value, rhs));
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    const double divisor = static_cast<double>(rhs);
    if (divisor == 0.0) throw NativeBailout();
    return NativeValue::floating(lhs.float_value / divisor);
  }
  return numeric_div(lhs, NativeValue::integer(rhs));
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_mod_int_rhs(
    const NativeValue &lhs, std::int64_t rhs) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    if (rhs == 0) throw NativeBailout();
    return NativeValue::integer(floor_mod_int64(lhs.scalar_value, rhs));
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    const double divisor = static_cast<double>(rhs);
    if (divisor == 0.0) throw NativeBailout();
    return NativeValue::floating(floor_mod_double_native(lhs.float_value,
                                                         divisor));
  }
  return numeric_mod(lhs, NativeValue::integer(rhs));
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_floor_div_int_rhs(
    const NativeValue &lhs, std::int64_t rhs) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(profile_floor_div_int64(lhs.scalar_value, rhs));
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    const double divisor = static_cast<double>(rhs);
    if (divisor == 0.0) throw NativeBailout();
    return NativeValue::floating(std::floor(lhs.float_value / divisor));
  }
  return numeric_floor_div(lhs, NativeValue::integer(rhs));
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_lt_int_rhs(
    const NativeValue &lhs, std::int64_t rhs) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::boolean(lhs.scalar_value < rhs);
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    return NativeValue::boolean(lhs.float_value < static_cast<double>(rhs));
  }
  return numeric_lt(lhs, NativeValue::integer(rhs));
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_gt_int_rhs(
    const NativeValue &lhs, std::int64_t rhs) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::boolean(lhs.scalar_value > rhs);
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    return NativeValue::boolean(lhs.float_value > static_cast<double>(rhs));
  }
  return numeric_gt(lhs, NativeValue::integer(rhs));
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_le_int_rhs(
    const NativeValue &lhs, std::int64_t rhs) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::boolean(lhs.scalar_value <= rhs);
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    return NativeValue::boolean(lhs.float_value <= static_cast<double>(rhs));
  }
  return numeric_le(lhs, NativeValue::integer(rhs));
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_ge_int_rhs(
    const NativeValue &lhs, std::int64_t rhs) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::boolean(lhs.scalar_value >= rhs);
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    return NativeValue::boolean(lhs.float_value >= static_cast<double>(rhs));
  }
  return numeric_ge(lhs, NativeValue::integer(rhs));
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_eq_int_rhs(
    const NativeValue &lhs, std::int64_t rhs, bool negate) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    const bool equal = lhs.scalar_value == rhs;
    return NativeValue::boolean(negate ? !equal : equal);
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    const bool equal = lhs.float_value == static_cast<double>(rhs);
    return NativeValue::boolean(negate ? !equal : equal);
  }
  return numeric_eq(lhs, NativeValue::integer(rhs), negate);
}

static AMBER_NATIVE_ALWAYS_INLINE NativeValue native_numeric_fast_cmp_int_rhs(
    const NativeValue &lhs, std::int64_t rhs) {
  if (lhs.tag == NativeValue::Tag::Integer) {
    return NativeValue::integer(compare_int64(lhs.scalar_value, rhs));
  }
  if (lhs.tag == NativeValue::Tag::Float) {
    return NativeValue::integer(
        compare_double_native(lhs.float_value, static_cast<double>(rhs)));
  }
  return numeric_cmp(lhs, NativeValue::integer(rhs));
}

)AMBERCPP";
  for (std::uint32_t code_id : plan.native_code_ids) {
    out << "static NativeValue " << native_cpp_function_name(code_id)
        << "(std::initializer_list<NativeValue> args, "
           "NativeClosure *current_closure);\n";
  }
  if (!plan.vm_callable_code_ids.empty()) {
    out << "static NativeValue amber_vm_fallback_call(std::uint32_t code_id, "
           "std::initializer_list<NativeValue> args);\n";
  }
  out << "\nstatic NativeValue amber_native_call_code("
         "std::uint32_t code_id, std::initializer_list<NativeValue> args, "
         "NativeClosure *current_closure) {\n";
  out << "  switch (code_id) {\n";
  for (std::uint32_t code_id : plan.native_code_ids) {
    out << "  case " << code_id << ": return "
        << native_cpp_function_name(code_id) << "(args, current_closure);\n";
  }
  for (std::uint32_t code_id : plan.vm_callable_code_ids) {
    out << "  case " << code_id << ": return amber_vm_fallback_call(" << code_id
        << ", args);\n";
  }
  out << "  default: throw NativeBailout();\n";
  out << "  }\n";
  out << "}\n\n";
  out << "static NativeValue amber_native_call_closure("
         "const NativeValue &value, std::initializer_list<NativeValue> args) "
         "{\n";
  out << "  NativeClosure *closure = as_closure(value);\n";
  out << "  return amber_native_call_code(closure->code_id, args, closure);\n";
  out << "}\n\n";
  for (const amber::bytecode::BcCode &code : module.code_objects) {
    if (plan.native_code_ids.find(code.code_id) != plan.native_code_ids.end()) {
      out << emit_native_cpp_code_function(module, code);
    }
  }
  out << "static std::vector<std::uint8_t> embedded_bytecode() {\n";
  out << "  const std::string hex(kEmbeddedBytecodeHex);\n";
  out << "  std::vector<std::uint8_t> bytes;\n";
  out << "  bytes.reserve(hex.size() / 2U);\n";
  out << "  auto digit = [](char c) -> int {\n";
  out << "    if (c >= '0' && c <= '9') return c - '0';\n";
  out << "    if (c >= 'a' && c <= 'f') return 10 + c - 'a';\n";
  out << "    if (c >= 'A' && c <= 'F') return 10 + c - 'A';\n";
  out << "    return -1;\n";
  out << "  };\n";
  out << "  for (std::size_t i = 0; i + 1U < hex.size(); i += 2U) {\n";
  out << "    bytes.push_back(static_cast<std::uint8_t>((digit(hex[i]) << "
         "4U) | digit(hex[i + 1U])));\n";
  out << "  }\n";
  out << "  return bytes;\n";
  out << "}\n\n";
  out << "static const amber::bytecode::BcMethod *zero_arg_method_by_name("
         "const amber::bytecode::BcModule &module, const std::string &name) "
         "{\n";
  out << "  for (const amber::bytecode::BcMethod &method : module.methods) {\n";
  out << "    if (method.selector_sym_id < module.symbols.size() && "
         "module.symbols[method.selector_sym_id] == name && "
         "method.params.empty() && "
         "(method.flags & (amber::bytecode::kMethodFlagInstance | "
         "amber::bytecode::kMethodFlagClass | "
         "amber::bytecode::kMethodFlagPropertyGetter | "
         "amber::bytecode::kMethodFlagPropertySetter)) == 0U) return "
         "&method;\n";
  out << "  }\n";
  out << "  return nullptr;\n";
  out << "}\n\n";
  out << "static void print_fault(const amber::runtime::ExecutionResult "
         "&result) {\n";
  out << "  if (!result.fault.has_value()) return;\n";
  out << "  std::cerr << result.fault->error_name << \": \" << "
         "result.fault->message << \"\\n\";\n";
  out << "  if (!result.fault->trace_text.empty()) {\n";
  out << "    std::cerr << result.fault->trace_text;\n";
  out << "    if (result.fault->trace_text.back() != '\\n') "
         "std::cerr << \"\\n\";\n";
  out << "  }\n";
  out << "}\n\n";
  out << "static bool should_print_vm_value(const amber::runtime::Value "
         "&value) { return !value.is_null() && !value.is_closure(); }\n\n";
  out << "static int run_vm_entry() {\n";
  out << "  const std::vector<std::uint8_t> bytes = embedded_bytecode();\n";
  out << "  amber::bytecode::DecodeResult decoded = "
         "amber::bytecode::deserialize_module(bytes);\n";
  out << "  if (!decoded.ok()) { std::cerr << "
         "amber::bytecode::verify_errors_to_json(decoded.errors); return 1; "
         "}\n";
  out << "  amber::runtime::RuntimeWorldOptions options;\n";
  out << "  options.capability_grants = embedded_capability_grants();\n";
  out << "  amber::runtime::RuntimeWorld world(decoded.module, "
         "std::move(options));\n";
  out << "  amber::runtime::ExecutionResult result;\n";
  if (artifact.entry_mode != EntryExecutionMode::MainOnly) {
    out << "  if (decoded.module.init.has_entry_code_id) {\n";
    out << "    result = world.execute(decoded.module.init.entry_code_id);\n";
    out << "    if (!result.ok()) { print_fault(result); return 1; }\n";
    out << "  }\n";
  }
  if (artifact.entry_mode == EntryExecutionMode::MainAfterInit) {
    out << "  result = world.execute(" << main_code_id << ");\n";
  }
  if (artifact.entry_mode == EntryExecutionMode::MainOnly) {
    out << "  result = world.execute(" << main_code_id << ");\n";
  }
  out << "  if (!result.ok()) { print_fault(result); return 1; }\n";
  out << "  if (should_print_vm_value(result.value)) std::cout << "
         "amber::runtime::value_to_debug_string(result.value, "
         "&decoded.module, &result.runtime_strings, "
         "&result.runtime_symbols) << \"\\n\";\n";
  out << "  return 0;\n";
  out << "}\n\n";
  if (!plan.vm_callable_code_ids.empty()) {
    // Per-function VM fallback: a lazily-created embedded world (module init
    // NOT run — vm-callable functions cannot reach module state) executes
    // single code objects across an immutable-value bridge. Faults and
    // non-convertible argument/result values throw NativeBailout, which remains
    // a sound whole-program restart because vm-callable functions are
    // effect-free by construction.
    out << "struct AmberVmFallbackState {\n";
    out << "  amber::bytecode::DecodeResult decoded;\n";
    out << "  std::unique_ptr<amber::runtime::RuntimeWorld> world;\n";
    out << "};\n\n";
    out << "static AmberVmFallbackState &amber_vm_fallback_state() {\n";
    out << "  static AmberVmFallbackState *state = [] {\n";
    out << "    auto *out_state = new AmberVmFallbackState();\n";
    out << "    out_state->decoded = "
           "amber::bytecode::deserialize_module(embedded_bytecode());\n";
    out << "    if (!out_state->decoded.ok()) throw NativeBailout();\n";
    out << "    out_state->world = "
           "std::make_unique<amber::runtime::RuntimeWorld>("
           "out_state->decoded.module);\n";
    out << "    return out_state;\n";
    out << "  }();\n";
    out << "  return *state;\n";
    out << "}\n\n";
    out << "static amber::runtime::RuntimeWorld &amber_vm_fallback_world() "
           "{\n";
    out << "  return *amber_vm_fallback_state().world;\n";
    out << "}\n\n";
    out << "static const std::vector<std::string> &"
           "amber_vm_fallback_module_strings() {\n";
    out << "  return amber_vm_fallback_state().decoded.module.strings;\n";
    out << "}\n\n";
    out << "static const std::vector<std::string> &"
           "amber_vm_fallback_module_symbols() {\n";
    out << "  return amber_vm_fallback_state().decoded.module.symbols;\n";
    out << "}\n\n";
    out << "static NativeValue amber_vm_fallback_result("
           "const amber::runtime::Value &value, "
           "const std::vector<std::string> &vm_strings, "
           "const std::vector<std::string> &vm_symbols) {\n";
    out << "  if (value.is_null()) return NativeValue::nullv();\n";
    out << "  if (value.is_bool()) return "
           "NativeValue::boolean(value.as_bool());\n";
    out << "  if (value.is_integer()) return "
           "NativeValue::integer(value.as_integer());\n";
    out << "  if (value.is_float()) return "
           "NativeValue::floating(value.as_float());\n";
    // VM string ids are scoped to the fallback execution; strings cross the
    // bridge by content and re-intern into the native table.
    out << "  if (value.is_string()) {\n";
    out << "    const std::uint32_t string_id = "
           "value.as_string().string_id;\n";
    out << "    if (string_id >= vm_strings.size()) throw NativeBailout();\n";
    out << "    return NativeValue::string_ref(native_intern_string("
           "vm_strings[string_id]));\n";
    out << "  }\n";
    out << "  if (value.is_symbol()) {\n";
    out << "    const std::uint32_t symbol_id = "
           "value.as_symbol().symbol_id;\n";
    out << "    if (symbol_id >= vm_symbols.size()) throw NativeBailout();\n";
    out << "    return NativeValue::symbol_ref(native_intern_symbol("
           "vm_symbols[symbol_id]));\n";
    out << "  }\n";
    out << "  if (value.is_uuid()) {\n";
    out << "    const auto uuid = value.as_uuid();\n";
    out << "    if (uuid == nullptr) throw NativeBailout();\n";
    out << "    return NativeValue::uuid(*uuid);\n";
    out << "  }\n";
    out << "  if (value.is_list()) {\n";
    out << "    const auto list = value.as_list();\n";
    out << "    if (list == nullptr) throw NativeBailout();\n";
    out << "    std::vector<NativeValue> items;\n";
    out << "    items.reserve(list->items.size());\n";
    out << "    for (const amber::runtime::Value &item : list->items) {\n";
    out << "      items.push_back(amber_vm_fallback_result(item, "
           "vm_strings, vm_symbols));\n";
    out << "    }\n";
    out << "    return NativeValue::list(std::move(items));\n";
    out << "  }\n";
    out << "  if (value.is_tuple()) {\n";
    out << "    const auto tuple = value.as_tuple();\n";
    out << "    if (tuple == nullptr) throw NativeBailout();\n";
    out << "    std::vector<NativeValue> items;\n";
    out << "    items.reserve(tuple->items.size());\n";
    out << "    for (const amber::runtime::Value &item : tuple->items) {\n";
    out << "      items.push_back(amber_vm_fallback_result(item, "
           "vm_strings, vm_symbols));\n";
    out << "    }\n";
    out << "    return NativeValue::tuple(std::move(items));\n";
    out << "  }\n";
    out << "  if (value.is_set()) {\n";
    out << "    const auto set = value.as_set();\n";
    out << "    if (set == nullptr) throw NativeBailout();\n";
    out << "    std::vector<NativeValue> items;\n";
    out << "    items.reserve(set->items.size());\n";
    out << "    for (const amber::runtime::Value &item : set->items) {\n";
    out << "      items.push_back(amber_vm_fallback_result(item, "
           "vm_strings, vm_symbols));\n";
    out << "    }\n";
    out << "    return native_set_from_items(std::move(items));\n";
    out << "  }\n";
    out << "  if (value.is_map()) {\n";
    out << "    const auto map = value.as_map();\n";
    out << "    if (map == nullptr) throw NativeBailout();\n";
    out << "    std::vector<std::pair<NativeValue, NativeValue>> entries;\n";
    out << "    entries.reserve(map->entries.size());\n";
    out << "    for (const amber::runtime::MapEntry &entry : map->entries) {\n";
    out << "      entries.emplace_back("
           "amber_vm_fallback_result(entry.key, vm_strings, vm_symbols), "
           "amber_vm_fallback_result(entry.value, vm_strings, vm_symbols));\n";
    out << "    }\n";
    out << "    return NativeValue::map_entries(std::move(entries), "
           "map->strict);\n";
    out << "  }\n";
    out << "  throw NativeBailout();\n";
    out << "}\n\n";
    out << "static NativeValue amber_vm_fallback_call(std::uint32_t code_id, "
           "std::initializer_list<NativeValue> args) {\n";
    out << "  std::vector<amber::runtime::Value> vm_args;\n";
    out << "  vm_args.reserve(args.size());\n";
    out << "  for (const NativeValue &arg : args) {\n";
    out << "    switch (arg.tag) {\n";
    out << "    case NativeValue::Tag::Null: "
           "vm_args.push_back(amber::runtime::Value::null()); break;\n";
    out << "    case NativeValue::Tag::Bool: "
           "vm_args.push_back(amber::runtime::Value::boolean("
           "arg.scalar_value != 0)); break;\n";
    out << "    case NativeValue::Tag::Integer: "
           "vm_args.push_back(amber::runtime::Value::integer("
           "arg.scalar_value)); break;\n";
    out << "    case NativeValue::Tag::Float: "
           "vm_args.push_back(amber::runtime::Value::floating("
           "arg.float_value)); break;\n";
    out << "    case NativeValue::Tag::Uuid: "
           "vm_args.push_back(amber::runtime::Value::uuid("
           "std::make_shared<amber::runtime::RuntimeUuidValue>("
           "as_uuid(arg)))); break;\n";
    out << "    default: throw NativeBailout();\n";
    out << "    }\n";
    out << "  }\n";
    out << "  const amber::runtime::ExecutionResult result = "
           "amber_vm_fallback_world().execute(code_id, vm_args);\n";
    out << "  if (!result.ok()) throw NativeBailout();\n";
    out << "  return amber_vm_fallback_result(result.value, "
           "result.runtime_strings.empty() "
           "? amber_vm_fallback_module_strings() "
           ": result.runtime_strings, "
           "result.runtime_symbols.empty() "
           "? amber_vm_fallback_module_symbols() "
           ": result.runtime_symbols);\n";
    out << "}\n\n";
  }
  out << "static std::string native_value_to_debug_string("
         "const NativeValue &value) {\n";
  out << "  switch (value.tag) {\n";
  out << "  case NativeValue::Tag::Null: return \"null\";\n";
  out << "  case NativeValue::Tag::Bool: return value.scalar_value != 0 ? "
         "\"true\" : \"false\";\n";
  out << "  case NativeValue::Tag::Integer: return "
         "std::to_string(value.scalar_value);\n";
  // Mirrors value_to_debug_string: default ostream precision, so integral
  // floats print without a decimal point.
  out << "  case NativeValue::Tag::Float: {\n";
  out << "    std::ostringstream text;\n";
  out << "    text << value.float_value;\n";
  out << "    return text.str();\n";
  out << "  }\n";
  // Mirrors value_to_debug_string: quotes around the raw text, no escaping.
  out << "  case NativeValue::Tag::String:\n";
  out << "  case NativeValue::Tag::HeapString: return \"\\\"\" + "
         "native_string_text(value) + \"\\\"\";\n";
  out << "  case NativeValue::Tag::Symbol: return \":\" + "
         "native_symbol_text(value.scalar_value);\n";
  out << "  case NativeValue::Tag::StrType:\n";
  out << "  case NativeValue::Tag::IntType:\n";
  out << "  case NativeValue::Tag::BigIntType:\n";
  out << "  case NativeValue::Tag::FloatType:\n";
  out << "  case NativeValue::Tag::BoolType:\n";
  out << "  case NativeValue::Tag::SymbolType:\n";
  out << "  case NativeValue::Tag::NullType:\n";
  out << "  case NativeValue::Tag::ObjectType:\n";
  out << "    return std::string(\"<type \") + native_type_tag_name(value) + "
         "\">\";\n";
  out << "  case NativeValue::Tag::MathModule: return \"Math\";\n";
  out << "  case NativeValue::Tag::JsonModule: return \"Json\";\n";
  out << "  case NativeValue::Tag::YamlModule: return \"Yaml\";\n";
  out << "  case NativeValue::Tag::BytesModule: return \"Bytes\";\n";
  out << "  case NativeValue::Tag::Base64Module: return \"Base64\";\n";
  out << "  case NativeValue::Tag::Base64UrlModule: return \"Base64Url\";\n";
  out << "  case NativeValue::Tag::HexModule: return \"Hex\";\n";
  out << "  case NativeValue::Tag::DigestModule: return \"Digest\";\n";
  out << "  case NativeValue::Tag::BenchmarkModule: return \"Benchmark\";\n";
  out << "  case NativeValue::Tag::UrlModule: return \"Url\";\n";
  out << "  case NativeValue::Tag::ArgParserModule: return \"ArgParser\";\n";
  out << "  case NativeValue::Tag::RegexpModule: return \"Regexp\";\n";
  out << "  case NativeValue::Tag::FsModule: return \"fs\";\n";
  out << "  case NativeValue::Tag::SecureRandomModule: return "
         "\"SecureRandom\";\n";
  out << "  case NativeValue::Tag::UuidModule: return \"Uuid\";\n";
  out << "  case NativeValue::Tag::RangeModule: return \"Range\";\n";
  out << "  case NativeValue::Tag::TimeModule: return \"Time\";\n";
  out << "  case NativeValue::Tag::TimePeriodModule: return \"TimePeriod\";\n";
  out << "  case NativeValue::Tag::Bytes: return \"<bytes \" + "
         "std::to_string(as_bytes(value).bytes.size()) + \">\";\n";
  out << "  case NativeValue::Tag::Range: return \"<instance Range>\";\n";
  out << "  case NativeValue::Tag::ArgParser: return "
         "\"<instance ArgParser>\";\n";
  out << "  case NativeValue::Tag::FsPath: return as_fs_path(value).path;\n";
  out << "  case NativeValue::Tag::Regexp: return "
         "native_regexp_pattern_to_string(as_regexp(value));\n";
  out << "  case NativeValue::Tag::RegexpMatch: return "
         "native_regexp_match_to_string(as_regexp_match(value));\n";
  out << "  case NativeValue::Tag::Uuid: return "
         "amber::runtime::runtime_uuid_to_string(as_uuid(value));\n";
  out << "  case NativeValue::Tag::Time: return "
         "amber::runtime::runtime_time_to_iso8601(as_time(value));\n";
  out << "  case NativeValue::Tag::TimePeriod: return "
         "amber::runtime::runtime_time_period_to_string("
         "as_time_period(value));\n";
  out << "  case NativeValue::Tag::Closure: return \"<closure>\";\n";
  out << "  case NativeValue::Tag::List: {\n";
  out << "    const auto &items = as_list(value).items;\n";
  out << "    std::ostringstream text; text << \"[\";\n";
  out << "    for (std::size_t i = 0; i < items.size(); ++i) {\n";
  out << "      if (i != 0U) text << \", \";\n";
  out << "      text << native_value_to_debug_string(items[i]);\n";
  out << "    }\n";
  out << "    text << \"]\"; return text.str();\n";
  out << "  }\n";
  out << "  case NativeValue::Tag::Tuple: {\n";
  out << "    const auto &items = as_tuple(value).items;\n";
  out << "    std::ostringstream text; text << \"(\";\n";
  out << "    for (std::size_t i = 0; i < items.size(); ++i) {\n";
  out << "      if (i != 0U) text << \", \";\n";
  out << "      text << native_value_to_debug_string(items[i]);\n";
  out << "    }\n";
  out << "    text << \")\"; return text.str();\n";
  out << "  }\n";
  out << "  case NativeValue::Tag::Set: {\n";
  out << "    const auto &items = as_set(value).items;\n";
  out << "    std::ostringstream text; text << \"Set{\";\n";
  out << "    for (std::size_t i = 0; i < items.size(); ++i) {\n";
  out << "      if (i != 0U) text << \", \";\n";
  out << "      text << native_value_to_debug_string(items[i]);\n";
  out << "    }\n";
  out << "    text << \"}\"; return text.str();\n";
  out << "  }\n";
  out << "  case NativeValue::Tag::Map: {\n";
  out << "    const auto &entries = as_map(value).entries;\n";
  out << "    std::ostringstream text; text << \"{\";\n";
  out << "    for (std::size_t i = 0; i < entries.size(); ++i) {\n";
  out << "      if (i != 0U) text << \", \";\n";
  out << "      text << native_value_to_debug_string(entries[i].first) << "
         "\": \" << native_value_to_debug_string(entries[i].second);\n";
  out << "    }\n";
  out << "    text << \"}\"; return text.str();\n";
  out << "  }\n";
  out << "  }\n";
  out << "  throw NativeBailout();\n";
  out << "}\n";
  out << "static void print_native_value(const NativeValue &value) {\n";
  out << "  if (value.tag == NativeValue::Tag::Null || "
         "value.tag == NativeValue::Tag::Closure) return;\n";
  out << "  std::cout << native_value_to_debug_string(value) << \"\\n\";\n";
  out << "}\n\n";

  // Native-extension registration table (native-packages 5c-ii). The build
  // manifest names the logical->symbol bindings, foreign-handle types, and
  // package errors; emit a startup function that builds a
  // RuntimeNativePackageDescriptor and stages it in the process-global
  // NativeExtRegistry. RuntimeWorld merges that startup contribution with the
  // module's bytecode binding attrs before registering the package descriptor
  // into its active registries. Bytecode builds never run this, so a
  // `native def` there falls back to its Amber body. Symbols are declared with
  // the free-thunk prototype and reinterpret_cast at registration: C linkage
  // matches by name, and the VM casts back to the method/destructor shape it
  // knows from the bytecode binding.
  const bool has_native_extensions = !native_extensions.empty();
  if (has_native_extensions) {
    const auto cpp_string = [](const std::string &text) {
      std::string escaped;
      for (const char c : text) {
        if (c == '\\' || c == '"') {
          escaped.push_back('\\');
          escaped.push_back(c);
        } else if (c == '\n') {
          escaped += "\\n";
        } else if (c == '\r') {
          escaped += "\\r";
        } else if (c == '\t') {
          escaped += "\\t";
        } else {
          escaped.push_back(c);
        }
      }
      return escaped;
    };
    const auto exit_code_literal = [](const std::string &text) {
      if (text.empty()) {
        return std::string();
      }
      std::size_t parsed = 0;
      try {
        const std::int64_t value = std::stoll(text, &parsed, 10);
        if (parsed == text.size()) {
          return std::to_string(value);
        }
      } catch (const std::exception &) {
      }
      return std::string();
    };
    std::vector<std::string> declared_symbols;
    const auto declare_once = [&](const std::string &symbol) {
      if (std::find(declared_symbols.begin(), declared_symbols.end(), symbol) !=
          declared_symbols.end()) {
        return;
      }
      declared_symbols.push_back(symbol);
      out << "extern \"C\" AmberStatus " << symbol
          << "(AmberCtx *, const AmberValue *, std::size_t, AmberValue *);\n";
    };
    const std::vector<std::string> direct_symbols =
        direct_native_symbols(module, native_extensions);
    for (const amber::pkg::PackageNativeExtension &extension :
         native_extensions) {
      for (const amber::pkg::PackageNativeSymbol &symbol : extension.symbols) {
        declare_once(symbol.symbol);
      }
    }
    for (const std::string &symbol : direct_symbols) {
      declare_once(symbol);
    }
    out << "\nstatic void amber_register_native_extensions() {\n";
    out << "  amber::runtime::RuntimeNativePackageDescriptor package;\n";
    for (const std::string &symbol : direct_symbols) {
      out << "  package.thunks.push_back({\"" << cpp_string(symbol)
          << "\", reinterpret_cast<void *>(&" << symbol << ")});\n";
    }
    std::unordered_map<std::string, std::string> logical_to_symbol;
    for (const amber::pkg::PackageNativeExtension &extension :
         native_extensions) {
      for (const amber::pkg::PackageNativeSymbol &symbol : extension.symbols) {
        logical_to_symbol[symbol.logical] = symbol.symbol;
        out << "  package.thunks.push_back({\"" << cpp_string(symbol.logical)
            << "\", reinterpret_cast<void *>(&" << symbol.symbol << ")});\n";
      }
      for (const amber::pkg::PackageNativeType &type : extension.types) {
        out << "  {\n";
        out << "    amber::runtime::NativeTypeDescriptor descriptor;\n";
        out << "    descriptor.tag = \"" << cpp_string(type.tag) << "\";\n";
        const std::string destructor_symbol =
            logical_to_symbol.count(type.destructor) != 0U
                ? logical_to_symbol[type.destructor]
                : (is_c_symbol_name(type.destructor) ? type.destructor
                                                     : std::string());
        if (type.ownership == "owned") {
          out << "    descriptor.ownership = amber::runtime::"
                 "RuntimeForeignHandle::Ownership::Owned;\n";
          if (!destructor_symbol.empty()) {
            out << "    descriptor.owned_destructor = reinterpret_cast<void "
                   "(*)(void *, void *)>(&"
                << destructor_symbol << ");\n";
          }
        } else if (type.ownership == "collected") {
          out << "    descriptor.ownership = amber::runtime::"
                 "RuntimeForeignHandle::Ownership::Collected;\n";
          if (!destructor_symbol.empty()) {
            out << "    descriptor.collected_reclaim = reinterpret_cast<void "
                   "(*)(void *)>(&"
                << destructor_symbol << ");\n";
          }
        } else {
          out << "    descriptor.ownership = amber::runtime::"
                 "RuntimeForeignHandle::Ownership::Borrowed;\n";
        }
        out << "    package.types.push_back(std::move(descriptor));\n";
        out << "  }\n";
      }
      for (const amber::pkg::PackageNativeError &error : extension.errors) {
        out << "  {\n";
        out << "    amber::runtime::RuntimeNativePackageErrorDescriptor "
               "descriptor;\n";
        out << "    descriptor.name = \"" << cpp_string(error.name) << "\";\n";
        out << "    descriptor.parent = \""
            << cpp_string(error.parent.empty() ? "NativeError" : error.parent)
            << "\";\n";
        if (!error.default_message.empty()) {
          out << "    descriptor.default_message = \""
              << cpp_string(error.default_message) << "\";\n";
        }
        const std::string default_exit_code =
            exit_code_literal(error.default_exit_code);
        if (!default_exit_code.empty()) {
          out << "    descriptor.default_exit_code = " << default_exit_code
              << ";\n";
        }
        out << "    package.errors.push_back(std::move(descriptor));\n";
        out << "  }\n";
      }
    }
    out << "  amber::runtime::NativeExtRegistry::global().register_package("
           "std::move(package));\n";
    out << "}\n\n";
  }

  out << "} // namespace\n\n";
  out << "int main() {\n";
  if (has_native_extensions) {
    out << "  amber_register_native_extensions();\n";
  }
  out << "  try {\n";
  if (artifact.entry_mode != EntryExecutionMode::MainOnly &&
      artifact.has_entry_init_code_id) {
    out << "    NativeValue init_result = amber_native_call_code("
        << init_code_id << ", {}, nullptr);\n";
    if (artifact.entry_mode == EntryExecutionMode::Init) {
      out << "    print_native_value(init_result);\n";
    } else {
      out << "    (void)init_result;\n";
    }
  }
  if (artifact.entry_mode == EntryExecutionMode::MainAfterInit ||
      artifact.entry_mode == EntryExecutionMode::MainOnly) {
    out << "    NativeValue result = amber_native_call_code(" << main_code_id
        << ", {}, nullptr);\n";
    out << "    print_native_value(result);\n";
  }
  out << "    return 0;\n";
  out << "  } catch (const NativeBailout &) {\n";
  out << "    return run_vm_entry();\n";
  out << "  } catch (const std::exception &error) {\n";
  out << "    std::cerr << \"NativeCodeError: \" << error.what() << "
         "\"\\n\";\n";
  out << "    return 1;\n";
  out << "  }\n";
  out << "}\n";

  plan.source = out.str();
  return plan;
}

std::string native_cpp_coverage_to_dump(const NativeCppBuildPlan &plan) {
  const std::size_t total = plan.coverage.size();
  const std::size_t direct = plan.native_code_ids.size();
  const std::size_t bridge = plan.vm_callable_code_ids.size();
  const std::size_t fallback = total - direct - bridge;
  std::ostringstream out;
  out << "\ncpp-bytecode-direct-v1 coverage\n";
  out << "  entry=" << (plan.entry_native ? "direct-native" : "fallback")
      << " direct=" << direct << " vm_bridge=" << bridge
      << " fallback=" << fallback << " total=" << total << "\n";
  if (!plan.fallback_reason.empty()) {
    out << "  fallback_reason=" << plan.fallback_reason << "\n";
  }
  for (const NativeCoverageRecord &record : plan.coverage) {
    out << "  c" << record.code_id << " kind=" << record.code_kind
        << " mode=" << record.mode;
    if (!record.reason.empty()) {
      out << " reason=\"" << record.reason << "\"";
    }
    out << "\n";
  }
  return out.str();
}

std::filesystem::path detect_native_runtime_root(const std::string &argv0) {
  if (const char *env = std::getenv("AMBER_NATIVE_RUNTIME_ROOT")) {
    const std::filesystem::path root(env);
    if (std::filesystem::exists(root / "runtime" / "vm.cpp")) {
      return root;
    }
  }
  std::vector<std::filesystem::path> candidates;
  candidates.push_back(std::filesystem::current_path());
  try {
    const std::filesystem::path executable =
        std::filesystem::absolute(argv0).lexically_normal();
    candidates.push_back(executable.parent_path().parent_path());
    candidates.push_back(executable.parent_path().parent_path().parent_path());
  } catch (const std::exception &) {
  }
  for (const std::filesystem::path &candidate : candidates) {
    if (std::filesystem::exists(candidate / "runtime" / "context.cpp") &&
        std::filesystem::exists(candidate / "runtime" / "text.cpp") &&
        std::filesystem::exists(candidate / "runtime" / "vm.cpp") &&
        std::filesystem::exists(candidate / "bytecode" / "format.cpp")) {
      return candidate;
    }
  }
  throw std::runtime_error(
      "failed to locate Amber runtime sources for native build; set "
      "AMBER_NATIVE_RUNTIME_ROOT");
}

std::string choose_native_cxx() {
  if (const char *env = std::getenv("AMBER_NATIVE_CXX")) {
    if (*env != '\0') {
      return env;
    }
  }
  if (const char *env = std::getenv("CXX")) {
    if (*env != '\0') {
      return env;
    }
  }
  return "clang++";
}

std::string shell_command(const std::vector<std::string> &parts) {
  std::ostringstream out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0U) {
      out << " ";
    }
    out << shell_single_quote(parts[i]);
  }
  return out.str();
}

std::vector<std::string>
native_runtime_sources(const std::filesystem::path &root) {
  const std::vector<std::string> relative = {
      "bytecode/format.cpp",
      "frontend/lexer/token.cpp",
      "frontend/ast/expr.cpp",
      "profile/capabilities.cpp",
      "profile/effects.cpp",
      "profile/replay.cpp",
      "profile/data.cpp",
      "profile/wasm_accel.cpp",
      "profile/modern.cpp",
      "buildsys/build.cpp",
      "optimizer/native.cpp",
      "package/package.cpp",
      "runtime/context.cpp",
      "runtime/text.cpp",
      "runtime/watch.cpp",
      "runtime/value.cpp",
      "runtime/value_display.cpp",
      "runtime/errors.cpp",
      "runtime/numeric.cpp",
      "runtime/objects.cpp",
      "runtime/heap.cpp",
      "runtime/concurrency.cpp",
      "runtime/world.cpp",
      "runtime/io.cpp",
      "runtime/reactor.cpp",
      "runtime/digest.cpp",
      "runtime/http_codec.cpp",
      "runtime/net_http.cpp",
      "runtime/net_http_transport.cpp",
      "runtime/vm.cpp",
      "runtime/stdlib_registry.cpp",
      "runtime/stdlib_io.cpp",
      "runtime/stdlib_fs.cpp",
      "runtime/stdlib_net.cpp",
      "runtime/stdlib_net_http.cpp",
      "runtime/stdlib_task.cpp",
      "runtime/stdlib_math.cpp",
      "runtime/stdlib_json.cpp",
      "runtime/stdlib_codecs.cpp",
      "runtime/stdlib_digest.cpp",
      "runtime/stdlib_benchmark.cpp",
      "runtime/stdlib_secure_random.cpp",
      "runtime/stdlib_argparser.cpp",
      "runtime/stdlib_regexp.cpp",
      "runtime/stdlib_uuid.cpp",
      "runtime/stdlib_time.cpp",
      "runtime/stdlib_url.cpp",
      "runtime/stdlib_yaml.cpp",
      "runtime/amber_ext.cpp",
      "runtime/module_loader.cpp",
      "runtime/native_bridge.cpp",
  };
  std::vector<std::string> out;
  out.reserve(relative.size());
  for (const std::string &path : relative) {
    out.push_back((root / path).string());
  }
  return out;
}

// The runtime sources are identical for every native build, so they are
// compiled once into an archive cached under a key derived from the
// compiler, flags, runtime sources, and all repo headers (over-hashing only
// causes a spare rebuild, never a stale archive). The final link force-loads
// the archive so static-initializer side effects keep parity with linking
// every translation unit directly.
const std::vector<std::string> &native_runtime_compile_flags() {
  static const std::vector<std::string> flags = {
      "-std=c++17",
      "-O2",
      "-DNDEBUG",
#ifdef AMBER_VALUE_REPR_TAGGED
      // Propagate the host amberc's Value representation (PLAN Phase 4
      // prototype) so the native runtime archive and the generated C++ share
      // its ABI. The flag is hashed into the archive cache key, so tagged and
      // variant archives never alias.
      "-DAMBER_VALUE_REPR_TAGGED",
#endif
  };
  return flags;
}

std::filesystem::path native_runtime_cache_root() {
  if (const char *env = std::getenv("AMBER_NATIVE_RT_CACHE")) {
    if (*env != '\0') {
      return env;
    }
  }
  if (const char *home = std::getenv("HOME")) {
    if (*home != '\0') {
      return std::filesystem::path(home) / ".cache" / "amber" / "native-rt";
    }
  }
  return std::filesystem::temp_directory_path() / "amber-native-rt";
}

std::string capture_command_output(const std::string &command) {
  std::string out;
  FILE *pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return out;
  }
  char buffer[256];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    out += buffer;
  }
  pclose(pipe);
  return out;
}

std::string
native_runtime_cache_key(const std::string &cxx,
                         const std::filesystem::path &runtime_root,
                         const std::vector<std::string> &runtime_sources) {
  std::ostringstream material;
  material << "amber.native-rt.v1\n";
  material << cxx << "\n";
  material << capture_command_output(shell_single_quote(cxx) + " --version")
           << "\n";
  for (const std::string &flag : native_runtime_compile_flags()) {
    material << flag << "\n";
  }

  std::vector<std::filesystem::path> hashed_files;
  for (const std::string &source : runtime_sources) {
    hashed_files.push_back(source);
  }
  const std::vector<std::string> header_dirs = {
      "runtime", "bytecode",  "frontend", "profile",
      "package", "optimizer", "buildsys", "frozen"};
  for (const std::string &dir : header_dirs) {
    const std::filesystem::path root = runtime_root / dir;
    if (!std::filesystem::exists(root)) {
      continue;
    }
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(root)) {
      if (entry.is_regular_file() && entry.path().extension() == ".h") {
        hashed_files.push_back(entry.path());
      }
    }
  }
  std::sort(hashed_files.begin(), hashed_files.end());
  for (const std::filesystem::path &path : hashed_files) {
    material << path.lexically_relative(runtime_root).generic_string() << "\n";
    material << amber::lexer::sha256_hex(read_file(path.string())) << "\n";
  }
  return amber::lexer::sha256_hex(material.str());
}

std::size_t native_runtime_archive_jobs(std::size_t source_count) {
  if (source_count <= 1U) {
    return source_count;
  }
  if (const char *env = std::getenv("AMBER_NATIVE_RT_JOBS")) {
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(env, &end, 10);
    if (end != env && parsed > 0UL) {
      return std::min<std::size_t>(source_count,
                                   static_cast<std::size_t>(parsed));
    }
  }
  const unsigned int hardware = std::thread::hardware_concurrency();
  const std::size_t jobs =
      hardware == 0U ? 1U : static_cast<std::size_t>(hardware);
  return std::min<std::size_t>(source_count, std::min<std::size_t>(jobs, 8U));
}

std::filesystem::path
ensure_native_runtime_archive(const std::string &cxx,
                              const std::filesystem::path &runtime_root,
                              const std::vector<std::string> &runtime_sources) {
  const std::string key =
      native_runtime_cache_key(cxx, runtime_root, runtime_sources);
  const std::filesystem::path cache_root = native_runtime_cache_root();
  const std::filesystem::path final_path = cache_root / key / "libamber_rt.a";
  if (std::filesystem::exists(final_path)) {
    return final_path;
  }

  const std::filesystem::path temp_dir =
      cache_root / (key + ".tmp." + std::to_string(::getpid()));
  std::filesystem::create_directories(temp_dir);

  std::vector<std::string> objects;
  objects.reserve(runtime_sources.size());
  std::vector<std::string> compile_commands;
  compile_commands.reserve(runtime_sources.size());
  const std::vector<std::string> &flags = native_runtime_compile_flags();
  const std::size_t jobs = native_runtime_archive_jobs(runtime_sources.size());
  for (std::size_t i = 0; i < runtime_sources.size(); ++i) {
    const std::filesystem::path object =
        temp_dir / ("rt" + std::to_string(i) + ".o");
    std::vector<std::string> command = {cxx};
    command.insert(command.end(), flags.begin(), flags.end());
    command.push_back("-I");
    command.push_back(runtime_root.string());
    command.push_back("-c");
    command.push_back(runtime_sources[i]);
    command.push_back("-o");
    command.push_back(object.string());
    compile_commands.push_back(shell_command(command));
    objects.push_back(object.string());
  }
  for (std::size_t start = 0; start < compile_commands.size(); start += jobs) {
    const std::size_t end =
        std::min<std::size_t>(compile_commands.size(), start + jobs);
    std::ostringstream batch;
    batch << "status=0\n";
    for (std::size_t i = start; i < end; ++i) {
      batch << "(" << compile_commands[i] << ") &\n";
      batch << "p" << i << "=$!\n";
    }
    for (std::size_t i = start; i < end; ++i) {
      batch << "wait \"$p" << i << "\" || status=1\n";
    }
    batch << "exit \"$status\"\n";
    const std::string rendered_batch = batch.str();
    if (std::system(rendered_batch.c_str()) != 0) {
      std::filesystem::remove_all(temp_dir);
      throw std::runtime_error("native runtime archive compile failed: " +
                               compile_commands[start]);
    }
  }

  const std::filesystem::path temp_archive = temp_dir / "libamber_rt.a";
  std::vector<std::string> archive_command = {"ar", "rcs",
                                              temp_archive.string()};
  archive_command.insert(archive_command.end(), objects.begin(), objects.end());
  const std::string rendered_archive = shell_command(archive_command);
  if (std::system(rendered_archive.c_str()) != 0) {
    std::filesystem::remove_all(temp_dir);
    throw std::runtime_error("native runtime archive creation failed: " +
                             rendered_archive);
  }

  std::filesystem::create_directories(final_path.parent_path());
  std::error_code rename_error;
  std::filesystem::rename(temp_archive, final_path, rename_error);
  std::filesystem::remove_all(temp_dir);
  if (rename_error && !std::filesystem::exists(final_path)) {
    throw std::runtime_error("native runtime archive install failed: " +
                             final_path.string());
  }
  return final_path;
}

// A conservative guard on author-supplied cxxflags: includes, defines, search
// paths, and libraries have dedicated manifest fields, and a few flags load
// arbitrary code into the compiler. Reject those; pass everything else through.
// (native-packages design §9: constrain flags, fold them into the digest.)
bool native_cxxflag_rejected(const std::string &flag) {
  static const std::vector<std::string> kRejectedPrefixes = {
      "-I",       "-D",       "-L", "-l",     "-include", "-imacros",
      "-isystem", "-fplugin", "-B", "-specs", "-Xclang"};
  for (const std::string &prefix : kRejectedPrefixes) {
    if (flag.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return flag.find('/') != std::string::npos;
}

NativeExecutableBuildResult build_native_executable(
    const std::string &argv0, const RunnableModuleArtifact &artifact,
    const std::filesystem::path &output_path,
    const std::filesystem::path &native_source_path,
    const std::vector<amber::pkg::PackageNativeExtension> &native_extensions =
        {},
    const std::filesystem::path &native_base_dir = {}) {
  NativeCppBuildPlan plan =
      build_native_cpp_plan(artifact, artifact.module, native_extensions);
  const std::filesystem::path parent = output_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  const std::filesystem::path source_parent = native_source_path.parent_path();
  if (!source_parent.empty()) {
    std::filesystem::create_directories(source_parent);
  }
  write_file(native_source_path.string(), plan.source);

  const std::filesystem::path runtime_root = detect_native_runtime_root(argv0);
  const std::string cxx = choose_native_cxx();
  const std::vector<std::string> runtime_sources =
      native_runtime_sources(runtime_root);

  std::filesystem::path runtime_archive;
  try {
    runtime_archive =
        ensure_native_runtime_archive(cxx, runtime_root, runtime_sources);
  } catch (const std::exception &) {
    // Fall back to compiling the runtime sources into the executable
    // directly (the pre-archive behavior) when the archive cannot be built,
    // e.g. `ar` is unavailable.
    runtime_archive.clear();
  }

  std::vector<std::string> command = {cxx};
  const std::vector<std::string> &flags = native_runtime_compile_flags();
  command.insert(command.end(), flags.begin(), flags.end());
  command.push_back("-I");
  command.push_back(runtime_root.string());
  command.push_back(native_source_path.string());

  // Native extension sources are pre-compiled to object files in their own
  // invocations so each unit gets correct language/standard flags: a
  // `language = "c"` unit must compile as C (so its `extern "C"` thunks keep C
  // linkage), which cannot share the launcher link command's `-std=c++17`.
  // Declared symbols and link libraries are gathered for the post-link check
  // and the link line below.
  std::vector<std::string> link_libraries;
  std::vector<std::string> declared_symbols;
  const std::vector<std::string> direct_symbols =
      direct_native_symbols(artifact.module, native_extensions);
  declared_symbols.insert(declared_symbols.end(), direct_symbols.begin(),
                          direct_symbols.end());
  std::vector<std::string> extension_objects;
  std::size_t extension_object_index = 0;
  for (const amber::pkg::PackageNativeExtension &extension :
       native_extensions) {
    std::vector<std::string> unit_flags;
    for (const std::string &flag : extension.cxxflags) {
      if (native_cxxflag_rejected(flag)) {
        throw std::runtime_error(
            "native extension '" + extension.name +
            "' uses a disallowed cxxflag (use the include_dirs/defines/"
            "link_libraries fields instead): " +
            flag);
      }
      unit_flags.push_back(flag);
    }
    for (const std::string &include_dir : extension.include_dirs) {
      unit_flags.push_back("-I");
      unit_flags.push_back((native_base_dir / include_dir).string());
    }
    for (const std::string &define : extension.defines) {
      unit_flags.push_back("-D" + define);
    }
    const bool compile_as_c =
        extension.language == "c" || extension.language == "C";
    for (const std::string &source : extension.sources) {
      const std::filesystem::path object_path =
          source_parent /
          ("amber_ext_" + std::to_string(extension_object_index++) + ".o");
      std::vector<std::string> compile = {cxx};
      if (compile_as_c) {
        compile.push_back("-x");
        compile.push_back("c");
        compile.push_back("-std=c11");
      } else {
        compile.push_back("-std=c++17");
      }
      compile.push_back("-O3");
      compile.push_back("-I");
      compile.push_back(runtime_root.string());
      compile.insert(compile.end(), unit_flags.begin(), unit_flags.end());
      compile.push_back("-c");
      compile.push_back((native_base_dir / source).string());
      compile.push_back("-o");
      compile.push_back(object_path.string());
      const std::string rendered_compile = shell_command(compile);
      if (std::system(rendered_compile.c_str()) != 0) {
        throw std::runtime_error("native extension compile failed: " +
                                 rendered_compile);
      }
      extension_objects.push_back(object_path.string());
    }
    for (const std::string &library : extension.link_libraries) {
      link_libraries.push_back(library);
    }
    for (const amber::pkg::PackageNativeSymbol &symbol : extension.symbols) {
      declared_symbols.push_back(symbol.symbol);
    }
  }
  command.insert(command.end(), extension_objects.begin(),
                 extension_objects.end());

  if (!runtime_archive.empty()) {
#if defined(__APPLE__)
    command.push_back("-Wl,-force_load," + runtime_archive.string());
#else
    command.push_back("-Wl,--whole-archive");
    command.push_back(runtime_archive.string());
    command.push_back("-Wl,--no-whole-archive");
#endif
  } else {
    command.insert(command.end(), runtime_sources.begin(),
                   runtime_sources.end());
  }
  for (const std::string &library : link_libraries) {
    command.push_back("-l" + library);
  }
  command.push_back("-pthread");
  command.push_back("-o");
  command.push_back(output_path.string());

  const std::string rendered = shell_command(command);
  const int exit_code = std::system(rendered.c_str());
  if (exit_code != 0) {
    throw std::runtime_error("native C++ build failed: " + rendered);
  }

  // Symbol-presence check: every declared native symbol must be defined in the
  // linked executable, caught here with a clear diagnostic rather than as an
  // undefined-reference once the Amber bindings start calling them.
  // Best-effort: skipped when `nm` is unavailable.
  if (!declared_symbols.empty()) {
    const std::filesystem::path nm_dump = output_path.string() + ".nm.txt";
    const std::string nm_command = shell_command({"nm", output_path.string()}) +
                                   " > " + shell_command({nm_dump.string()}) +
                                   " 2>/dev/null";
    const int nm_exit = std::system(nm_command.c_str());
    if (nm_exit == 0 && std::filesystem::exists(nm_dump)) {
      const std::string symbols_text = read_file(nm_dump.string());
      std::filesystem::remove(nm_dump);
      std::vector<std::string> missing;
      for (const std::string &symbol : declared_symbols) {
        if (symbols_text.find(symbol) == std::string::npos) {
          missing.push_back(symbol);
        }
      }
      if (!missing.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < missing.size(); ++i) {
          joined += (i == 0U ? "" : ", ") + missing[i];
        }
        throw std::runtime_error(
            "declared native symbols missing from the linked executable: " +
            joined);
      }
    } else {
      std::filesystem::remove(nm_dump);
    }
  }

  NativeExecutableBuildResult result;
  result.output_path = output_path.string();
  result.source_path = native_source_path.string();
  result.hash = amber::lexer::sha256_hex(read_file(output_path.string()));
  result.backend = plan.backend;
  result.cxx = cxx;
  result.native_code_count = plan.native_code_ids.size();
  result.vm_callable_code_count = plan.vm_callable_code_ids.size();
  result.fallback_code_count = artifact.module.code_objects.size() -
                               result.native_code_count -
                               result.vm_callable_code_count;
  result.total_code_count = artifact.module.code_objects.size();
  result.entry_native = plan.entry_native;
  result.full_native_coverage =
      plan.entry_native &&
      result.native_code_count == artifact.module.code_objects.size();
  result.uses_bytecode_fallback = plan.uses_bytecode_fallback;
  result.fallback_reason = plan.fallback_reason;
  result.coverage = plan.coverage;
  return result;
}

struct EmbeddedExecutable {
  std::string module_name;
  EntryExecutionMode entry_mode = EntryExecutionMode::Init;
  std::vector<std::uint8_t> bytes;
};

EmbeddedExecutable parse_embedded_executable(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open embedded executable: " + path);
  }

  EmbeddedExecutable executable;
  bool in_payload = false;
  bool in_bytecode = false;
  bool saw_marker = false;
  bool saw_end = false;
  std::string bytecode_hex;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!in_payload) {
      if (line == "__AMBER_EXECUTABLE_V1__") {
        in_payload = true;
        saw_marker = true;
      }
      continue;
    }
    if (line == "__END_AMBER_EXECUTABLE_V1__") {
      saw_end = true;
      break;
    }
    if (line == "bytecode") {
      in_bytecode = true;
      continue;
    }
    if (!in_bytecode) {
      if (line.compare(0, 7, "module ") == 0) {
        executable.module_name = hex_text_to_string(line.substr(7));
      } else if (line.compare(0, 5, "mode ") == 0) {
        executable.entry_mode = parse_entry_mode(line.substr(5));
      }
      continue;
    }
    bytecode_hex += line;
  }

  if (!saw_marker || !saw_end || executable.module_name.empty() ||
      bytecode_hex.empty()) {
    throw std::runtime_error("invalid amber executable wrapper: " + path);
  }
  executable.bytes = hex_text_to_bytes(bytecode_hex);
  return executable;
}

int run_embedded_command(int argc, char **argv) {
  if (argc < 3 || std::string(argv[1]) != "run-embedded") {
    usage(std::cerr);
    return 2;
  }
  const EmbeddedExecutable executable = parse_embedded_executable(argv[2]);
  return run_runnable_module(executable.module_name, executable.entry_mode,
                             executable.bytes);
}

struct SourceBuildCliOptions {
  std::string out_path;
  std::string out_dir;
  std::string target = "native";
  std::string entry = "auto";
  std::vector<amber::capability::CapabilityRequest> capability_grants;
  bool require_full_native = false;
};

SourceBuildCliOptions parse_source_build_options(int argc, char **argv,
                                                 int start_index) {
  SourceBuildCliOptions options;
  for (int i = start_index; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "-o" || arg == "--out") && i + 1 < argc) {
      options.out_path = argv[++i];
    } else if (arg == "--out-dir" && i + 1 < argc) {
      options.out_dir = argv[++i];
    } else if (arg == "--target" && i + 1 < argc) {
      options.target = argv[++i];
      if (options.target != "native" && options.target != "native-debug" &&
          options.target != "bytecode-wrapper") {
        throw std::runtime_error("unknown source build target: " +
                                 options.target);
      }
    } else if (arg == "--entry" && i + 1 < argc) {
      options.entry = argv[++i];
      if (options.entry != "auto" && options.entry != "init" &&
          options.entry != "main" && options.entry != "main-only") {
        throw std::runtime_error("unknown source build entry mode: " +
                                 options.entry);
      }
    } else if (arg == "--grant" && i + 1 < argc) {
      amber::capability::CapabilityRequest grant;
      amber::capability::CapabilityDiagnostic diagnostic;
      if (!amber::capability::parse_cli_grant(argv[++i], &grant, &diagnostic)) {
        throw std::runtime_error(diagnostic.message);
      }
      options.capability_grants.push_back(std::move(grant));
    } else if (arg == "--require-full-native") {
      options.require_full_native = true;
    } else {
      throw std::runtime_error("unknown source build option: " + arg);
    }
  }
  return options;
}

std::filesystem::path
default_executable_path_for(const std::string &source_path,
                            const SourceBuildCliOptions &options) {
  if (!options.out_path.empty()) {
    return std::filesystem::path(options.out_path);
  }
  const std::filesystem::path source(source_path);
  const std::string stem =
      source.stem().string().empty() ? "amber-program" : source.stem().string();
  if (!options.out_dir.empty()) {
    return std::filesystem::path(options.out_dir) / stem;
  }
  std::filesystem::path output = source;
  output.replace_extension("");
  return output;
}

std::string
full_native_requirement_diagnostic(const NativeExecutableBuildResult &native) {
  if (native.full_native_coverage) {
    return {};
  }
  std::ostringstream out;
  out << "full native coverage required, got " << native.native_code_count
      << "/" << native.total_code_count << " direct-native code objects";
  if (native.vm_callable_code_count != 0U) {
    out << ", " << native.vm_callable_code_count << " VM-bridge code objects";
  }
  if (native.fallback_code_count != 0U) {
    out << ", " << native.fallback_code_count << " fallback code objects";
  }
  if (!native.entry_native) {
    out << "; entry is not direct-native";
  }
  if (!native.fallback_reason.empty()) {
    out << ": " << native.fallback_reason;
  }
  return out.str();
}

std::string executable_build_result_to_json(
    bool ok, const std::string &source_path, const std::string &output_path,
    const std::string &module_name, EntryExecutionMode entry_mode,
    const std::string &target, const NativeExecutableBuildResult *native,
    const std::string &diagnostic = {}) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.executable.build.v1\",\n";
  out << "  \"status\": \"" << (ok ? "ok" : "error") << "\",\n";
  out << "  \"source\": \"" << json_escape(source_path) << "\",\n";
  out << "  \"output\": \"" << json_escape(output_path) << "\",\n";
  out << "  \"module\": \"" << json_escape(module_name) << "\",\n";
  out << "  \"entry\": \"" << entry_mode_name(entry_mode) << "\",\n";
  out << "  \"target\": \"" << json_escape(target) << "\",\n";
  out << "  \"native_backend\": \""
      << json_escape(native == nullptr ? "" : native->backend) << "\",\n";
  out << "  \"native_source\": \""
      << json_escape(native == nullptr ? "" : native->source_path) << "\",\n";
  out << "  \"native_hash\": \""
      << json_escape(native == nullptr ? "" : native->hash) << "\",\n";
  out << "  \"native_cxx\": \""
      << json_escape(native == nullptr ? "" : native->cxx) << "\",\n";
  out << "  \"native_entry\": "
      << (native != nullptr && native->entry_native ? "true" : "false")
      << ",\n";
  out << "  \"native_full_coverage\": "
      << (native != nullptr && native->full_native_coverage ? "true" : "false")
      << ",\n";
  out << "  \"native_code_count\": "
      << (native == nullptr ? 0U : native->native_code_count) << ",\n";
  out << "  \"vm_fallback_code_count\": "
      << (native == nullptr ? 0U : native->vm_callable_code_count) << ",\n";
  out << "  \"native_fallback_code_count\": "
      << (native == nullptr ? 0U : native->fallback_code_count) << ",\n";
  out << "  \"bytecode_code_count\": "
      << (native == nullptr ? 0U : native->total_code_count) << ",\n";
  out << "  \"bytecode_fallback\": "
      << (native != nullptr && native->uses_bytecode_fallback ? "true"
                                                              : "false")
      << ",\n";
  out << "  \"native_fallback_reason\": \""
      << json_escape(native == nullptr ? "" : native->fallback_reason)
      << "\",\n";
  out << "  \"native_coverage\": [";
  if (native != nullptr) {
    for (std::size_t i = 0; i < native->coverage.size(); ++i) {
      if (i != 0U) {
        out << ",";
      }
      const NativeCoverageRecord &record = native->coverage[i];
      out << "\n    {\"code_id\":" << record.code_id << ",\"kind\":\""
          << json_escape(record.code_kind) << "\",\"mode\":\""
          << json_escape(record.mode) << "\",\"reason\":\""
          << json_escape(record.reason) << "\"}";
    }
  }
  if (native != nullptr && !native->coverage.empty()) {
    out << "\n  ";
  }
  out << "]";
  if (!ok) {
    out << ",\n  \"diagnostic\": \"" << json_escape(diagnostic) << "\"\n";
  } else {
    out << "\n";
  }
  out << "}\n";
  return out.str();
}

int run_source_build_command(int argc, char **argv) {
  const std::string source_path = argv[2];
  SourceBuildCliOptions options = parse_source_build_options(argc, argv, 3);
  const std::filesystem::path output_path =
      default_executable_path_for(source_path, options);
  try {
    if (options.target == "bytecode-wrapper" &&
        !options.capability_grants.empty()) {
      throw std::runtime_error(
          "source build --grant is not supported with bytecode-wrapper target");
    }
    if (options.target == "bytecode-wrapper" && options.require_full_native) {
      throw std::runtime_error(
          "--require-full-native requires --target native or native-debug");
    }
    const std::optional<EntryExecutionMode> forced_entry =
        options.entry == "auto"
            ? std::nullopt
            : std::optional<EntryExecutionMode>(
                  parse_source_build_entry_mode(options.entry));
    const RunnableModuleArtifact artifact = compile_source_to_runnable_module(
        source_path, forced_entry, options.capability_grants);
    const std::filesystem::path parent = output_path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    NativeExecutableBuildResult native_result;
    const NativeExecutableBuildResult *native_json = nullptr;
    if (options.target == "bytecode-wrapper") {
      const std::string script =
          render_executable_script(executable_amberc_ref(argv[0]), artifact);
      write_file(output_path.string(), script);
      std::filesystem::permissions(output_path,
                                   std::filesystem::perms::owner_read |
                                       std::filesystem::perms::owner_write |
                                       std::filesystem::perms::owner_exec |
                                       std::filesystem::perms::group_read |
                                       std::filesystem::perms::group_exec |
                                       std::filesystem::perms::others_read |
                                       std::filesystem::perms::others_exec,
                                   std::filesystem::perm_options::replace);
    } else {
      const std::filesystem::path native_source_path =
          output_path.string() + ".native.cpp";
      native_result = build_native_executable(argv[0], artifact, output_path,
                                              native_source_path);
      native_json = &native_result;
      if (options.require_full_native) {
        const std::string diagnostic =
            full_native_requirement_diagnostic(native_result);
        if (!diagnostic.empty()) {
          std::cout << executable_build_result_to_json(
              false, source_path, output_path.string(), artifact.module_name,
              artifact.entry_mode, options.target, native_json, diagnostic);
          return 1;
        }
      }
    }
    std::cout << executable_build_result_to_json(
        true, source_path, output_path.string(), artifact.module_name,
        artifact.entry_mode, options.target, native_json);
    return 0;
  } catch (const std::exception &error) {
    std::cout << executable_build_result_to_json(
        false, source_path, output_path.string(), {}, EntryExecutionMode::Init,
        options.target, nullptr, error.what());
    return 1;
  }
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
    amber::pkg::PackageBuildOptions options =
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
    options.target_triple = native_target_triple();
    options.native_blobs =
        collect_native_blobs(manifest.manifest.native_extensions, root_dir);
    // Cross-module macro staging (§11): parse-only pre-pass over the package
    // modules harvests every `export macro` table before anything compiles.
    amber::macros::MacroProviderMap macro_providers;
    for (const amber::pkg::PackageModule &module : manifest.manifest.modules) {
      std::vector<amber::macros::MacroExport> exports = harvest_macro_exports(
          read_file(join_path(root_dir, module.path)), module.path);
      if (!exports.empty()) {
        macro_providers[module.name] = std::move(exports);
      }
    }
    for (const amber::pkg::PackageModule &module : manifest.manifest.modules) {
      amber::pkg::PackageModuleBlob blob;
      blob.name = module.name;
      blob.path = module.path;
      blob.bytes = compile_source_to_bytecode(
          join_path(root_dir, module.path), module.name,
          manifest.manifest.capabilities, &macro_providers);
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
  std::string target = "both";
  bool cache_enabled = true;
  bool require_full_native = false;
};

BuildCliOptions parse_build_options(int argc, char **argv, int start_index) {
  BuildCliOptions options;
  for (int i = start_index; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out-dir" && i + 1 < argc) {
      options.out_dir = argv[++i];
    } else if (arg == "--cache-dir" && i + 1 < argc) {
      options.cache_dir = argv[++i];
    } else if (arg == "--target" && i + 1 < argc) {
      options.target = argv[++i];
      if (options.target != "bytecode" && options.target != "native" &&
          options.target != "both") {
        throw std::runtime_error("unknown build target: " + options.target);
      }
    } else if (arg == "--no-cache") {
      options.cache_enabled = false;
    } else if (arg == "--require-full-native") {
      options.require_full_native = true;
    } else {
      throw std::runtime_error("unknown build option: " + arg);
    }
  }
  if (options.require_full_native && options.target == "bytecode") {
    throw std::runtime_error(
        "--require-full-native requires --target native or both");
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
  if (!profiles.numeric_int.empty() || !profiles.numeric_overflow.empty()) {
    out << "numeric.int=" << profiles.numeric_int << "\n";
    out << "numeric.overflow=" << profiles.numeric_overflow << "\n";
  }
  return out.str();
}

std::string module_attr_text(const amber::bytecode::BcModule &module,
                             const std::string &key) {
  for (const amber::bytecode::AttrEntry &attr : module.attrs) {
    if (attr.key_str_id < module.strings.size() &&
        module.strings[attr.key_str_id] == key) {
      return attr.value_str_id < module.strings.size()
                 ? module.strings[attr.value_str_id]
                 : std::string{};
    }
  }
  return {};
}

// amber.numeric-profile.v1: the manifest is the reproducibility anchor. A
// source preamble must agree with profiles.numeric when both are present;
// when only the manifest selects a profile, it is stamped into the module.
void reconcile_manifest_numeric_profile(
    amber::bytecode::BcModule *bc_module,
    const amber::build::BuildProfileSet &profiles,
    const std::string &module_path) {
  if (profiles.numeric_int.empty() && profiles.numeric_overflow.empty()) {
    return;
  }
  const std::string manifest_int =
      profiles.numeric_int.empty() ? "Int64" : profiles.numeric_int;
  const std::string manifest_overflow =
      profiles.numeric_overflow.empty() ? "checked" : profiles.numeric_overflow;
  const std::string module_int =
      module_attr_text(*bc_module, "amber.numeric.int");
  const std::string module_overflow =
      module_attr_text(*bc_module, "amber.numeric.overflow");
  if (!module_int.empty() || !module_overflow.empty()) {
    if (module_int != manifest_int || module_overflow != manifest_overflow) {
      throw std::runtime_error(
          "module numeric preamble (int: " + module_int +
          ", overflow: " + module_overflow +
          ") does not match manifest profiles.numeric (int: " + manifest_int +
          ", overflow: " + manifest_overflow + ") in " + module_path);
    }
    return;
  }
  add_module_attr(bc_module, "amber.numeric.int", manifest_int);
  add_module_attr(bc_module, "amber.numeric.overflow", manifest_overflow);
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

// Harvest a module's `export macro` table for cross-module staging (§11).
// Parse-only; diagnostics are left to the module's own compile.
std::vector<amber::macros::MacroExport>
harvest_macro_exports(const std::string &source, const std::string &path) {
  amber::lexer::LexResult lex_result = lex_source(source, path);
  if (!lex_result.ok()) {
    return {};
  }
  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    return {};
  }
  return amber::macros::collect_macro_exports(parse_result.items);
}

// Stage a provider from its built artifact's persisted macro section (§11)
// when the artifact is fresh — its recorded source hash matches the current
// source — skipping the source re-parse. Returns nullopt when the artifact
// is missing, stale, has no macro section (older artifact or no macros), or
// the section fails to decode; the caller falls back to the source harvest.
std::optional<std::vector<amber::macros::MacroExport>>
macro_exports_from_artifact(const std::filesystem::path &artifact_path,
                            const std::string &source_hash,
                            const std::string &module_path) {
  std::error_code exists_error;
  if (!std::filesystem::exists(artifact_path, exists_error) || exists_error) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes;
  try {
    bytes = read_bytes(artifact_path.string());
  } catch (const std::exception &) {
    return std::nullopt;
  }
  amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(bytes);
  if (!decoded.ok()) {
    return std::nullopt;
  }
  if (module_attr_text(decoded.module, "amber.build.source_hash") !=
      source_hash) {
    return std::nullopt;
  }
  const std::string payload =
      module_attr_text(decoded.module, amber::macros::kMacroExportsAttrKey);
  if (payload.empty()) {
    return std::nullopt;
  }
  std::string error;
  std::vector<amber::macros::MacroExport> exports =
      amber::macros::parse_macro_exports(payload, module_path, &error);
  if (!error.empty()) {
    return std::nullopt;
  }
  return exports;
}

amber::build::BuildArtifactRecord build_one_module(
    const amber::build::BuildModule &module,
    const std::filesystem::path &root_dir, const std::filesystem::path &out_dir,
    const std::filesystem::path &cache_dir,
    const amber::build::BuildProfileSet &profiles,
    const std::vector<BuiltStdlibAbi> &stdlib_abis, bool cache_enabled,
    const amber::macros::MacroProviderMap *macro_providers = nullptr,
    const std::string &macro_material = {}) {
  const std::filesystem::path source_path = root_dir / module.path;
  const std::string source = read_file(source_path.string());
  const std::string source_hash = amber::lexer::sha256_hex(source);
  // `macro_material` folds the staged macro providers (name + source hash per
  // provider) into the cache key: expansion output — not just linkage —
  // depends on provider macro sources (DESIGN-macro-system §11).
  const std::string cache_key = amber::lexer::sha256_hex(
      "amber.build.cache.v1\n" + module.name + "\n" + module.path + "\n" +
      source_hash + "\n" + profile_material(profiles) +
      stdlib_abi_material(stdlib_abis) + macro_material);
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
    amber::bytecode::BcModule bc_module = compile_source_text_to_module(
        source, module.path, module.name, {}, macro_providers);
    bc_module.required_features = profiles.required_features;
    bc_module.optional_features = profiles.optional_features;
    bc_module.forbidden_features = profiles.forbidden_features;
    bc_module.profile_flags = amber::build::profile_flags_for(profiles);
    if (!module.stdlib) {
      add_stdlib_dependencies(&bc_module, stdlib_abis);
    }
    reconcile_manifest_numeric_profile(&bc_module, profiles, module.path);
    add_module_attr(&bc_module, "amber.build.module", module.name);
    add_module_attr(&bc_module, "amber.build.source_hash", source_hash);
    add_module_attr(&bc_module, "amber.build.cache_key", cache_key);
    // Persisted artifact macro section (§11): a macro-providing module embeds
    // its `export macro` table in the artifact, so a later build's staging
    // pre-pass can read the exports from the artifact instead of re-parsing
    // provider source.
    if (macro_providers != nullptr) {
      const auto exports = macro_providers->find(module.name);
      if (exports != macro_providers->end() && !exports->second.empty()) {
        add_module_attr(
            &bc_module, amber::macros::kMacroExportsAttrKey,
            amber::macros::serialize_macro_exports(exports->second, source));
      }
    }
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

struct NativeGraphModule {
  std::string name;
  std::string path;
  std::vector<std::uint8_t> bytes;
  amber::bytecode::BcModule module;
  bool stdlib = false;
};

struct NativeGraphLinkResult {
  bool ok = false;
  RunnableModuleArtifact artifact;
  std::vector<amber::build::BuildDiagnostic> diagnostics;
};

struct NativeGraphExportRef {
  std::string kind;
  std::uint32_t code_id = 0;
  std::uint32_t method_index = 0;
  std::uint32_t class_index = 0;
};

std::string bc_string_or_empty(const amber::bytecode::BcModule &module,
                               std::uint32_t id) {
  return id < module.strings.size() ? module.strings[id] : std::string{};
}

std::string bc_symbol_or_empty(const amber::bytecode::BcModule &module,
                               std::uint32_t id) {
  return id < module.symbols.size() ? module.symbols[id] : std::string{};
}

std::vector<std::string> split_tab_fields(const std::string &text) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t tab = text.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(text.substr(start));
      break;
    }
    fields.push_back(text.substr(start, tab - start));
    start = tab + 1U;
  }
  return fields;
}

bool parse_u32_text(const std::string &text, std::uint32_t *out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10U + static_cast<std::uint64_t>(c - '0');
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
  }
  *out = static_cast<std::uint32_t>(value);
  return true;
}

bool integer_k_opcode(amber::bytecode::Opcode opcode) {
  using amber::bytecode::Opcode;
  switch (opcode) {
  case Opcode::IAddK:
  case Opcode::ISubK:
  case Opcode::ILtK:
  case Opcode::IGtK:
  case Opcode::IMulK:
  case Opcode::IDivK:
  case Opcode::IModK:
  case Opcode::IFloorDivK:
  case Opcode::ILeK:
  case Opcode::IGeK:
  case Opcode::IEqK:
  case Opcode::INeK:
  case Opcode::ICmpK:
  case Opcode::IBitAndK:
  case Opcode::IBitOrK:
  case Opcode::IBitXorK:
  case Opcode::IShlK:
  case Opcode::IShrK:
    return true;
  default:
    return false;
  }
}

struct NativeGraphModuleState {
  const NativeGraphModule *input = nullptr;
  std::unordered_map<std::uint32_t, std::uint32_t> strings;
  std::unordered_map<std::uint32_t, std::uint32_t> symbols;
  std::unordered_map<std::uint32_t, std::uint32_t> constants;
  std::unordered_map<std::uint32_t, std::uint32_t> code_ids;
  std::unordered_map<std::uint32_t, std::uint32_t> code_pc_offsets;
  std::unordered_map<std::uint32_t, std::uint32_t> code_pc_insert_points;
  std::uint32_t method_offset = 0;
  std::uint32_t class_offset = 0;
  std::uint32_t pattern_offset = 0;
  std::map<std::string, NativeGraphExportRef> exports;
};

class NativeGraphModuleBuilder {
public:
  explicit NativeGraphModuleBuilder(std::vector<NativeGraphModule> order)
      : order_(std::move(order)) {
    out_.format_version = {1, 0};
    out_.language_version = {1, 0};
  }

  NativeGraphLinkResult
  build(const std::string &root_module, EntryExecutionMode entry_mode,
        const std::vector<amber::capability::CapabilityRequest>
            &capability_grants) {
    NativeGraphLinkResult result;
    if (order_.empty()) {
      result.diagnostics.push_back(
          {"BuildError", "native graph is empty", root_module});
      return result;
    }
    for (const NativeGraphModule &module : order_) {
      NativeGraphModuleState state;
      state.input = &module;
      state.method_offset = static_cast<std::uint32_t>(out_.methods.size());
      state.class_offset = static_cast<std::uint32_t>(out_.classes.size());
      state.pattern_offset =
          static_cast<std::uint32_t>(out_.pattern_programs.size());
      for (const amber::bytecode::BcCode &code : module.module.code_objects) {
        state.code_ids[code.code_id] = next_code_id_++;
      }
      states_[module.name] = std::move(state);
    }

    for (const NativeGraphModule &module : order_) {
      NativeGraphModuleState &state = states_.at(module.name);
      copy_module(state);
    }

    synthesize_merged_init();
    add_module_attr(&out_, "amber.build.graph", "merged-native-v1");
    add_module_attr(&out_, "amber.build.graph.root", root_module);
    add_module_attr(&out_, "amber.build.graph.modules",
                    std::to_string(order_.size()));
    normalize_string_vector(&out_.required_features);
    normalize_string_vector(&out_.optional_features);
    normalize_string_vector(&out_.forbidden_features);

    amber::bytecode::DecodeResult decoded = amber::bytecode::deserialize_module(
        amber::bytecode::serialize_module(out_));
    if (!decoded.ok()) {
      result.diagnostics.push_back(
          {"BuildError", amber::bytecode::verify_errors_to_json(decoded.errors),
           root_module});
      return result;
    }

    const auto root_found = states_.find(root_module);
    if (root_found == states_.end()) {
      result.diagnostics.push_back(
          {"BuildError",
           "root module is missing from native graph: " + root_module,
           root_module});
      return result;
    }
    const NativeGraphModuleState &root_state = root_found->second;
    std::uint32_t root_main_code = 0;
    bool has_root_main = false;
    for (std::size_t i = 0; i < root_state.input->module.methods.size(); ++i) {
      const amber::bytecode::BcMethod &method =
          root_state.input->module.methods[i];
      const std::string selector =
          bc_symbol_or_empty(root_state.input->module, method.selector_sym_id);
      if (selector == "main" && method.params.empty() &&
          (method.flags & (amber::bytecode::kMethodFlagInstance |
                           amber::bytecode::kMethodFlagClass |
                           amber::bytecode::kMethodFlagPropertyGetter |
                           amber::bytecode::kMethodFlagPropertySetter)) == 0U) {
        const auto id_found = root_state.code_ids.find(method.entry_code_id);
        if (id_found != root_state.code_ids.end()) {
          root_main_code = id_found->second;
          has_root_main = true;
        }
        break;
      }
    }

    result.artifact.module_name = root_module;
    result.artifact.entry_mode = entry_mode;
    result.artifact.module = std::move(decoded.module);
    result.artifact.bytes =
        amber::bytecode::serialize_module(result.artifact.module);
    result.artifact.capability_grants = capability_grants;
    result.artifact.whole_graph_native = true;
    result.artifact.graph_module_count = order_.size();
    result.artifact.has_entry_init_code_id =
        result.artifact.module.init.has_entry_code_id;
    result.artifact.entry_init_code_id =
        result.artifact.module.init.entry_code_id;
    result.artifact.has_entry_main_code_id = has_root_main;
    result.artifact.entry_main_code_id = root_main_code;
    result.ok = true;
    return result;
  }

private:
  std::uint32_t intern_string(const std::string &text) {
    const auto found = string_ids_.find(text);
    if (found != string_ids_.end()) {
      return found->second;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(out_.strings.size());
    out_.strings.push_back(text);
    string_ids_[text] = id;
    return id;
  }

  std::uint32_t intern_symbol(const std::string &text) {
    const auto found = symbol_ids_.find(text);
    if (found != symbol_ids_.end()) {
      return found->second;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(out_.symbols.size());
    out_.symbols.push_back(text);
    symbol_ids_[text] = id;
    return id;
  }

  std::uint32_t intern_path_constant(const std::string &path) {
    amber::bytecode::Constant constant;
    constant.kind = amber::bytecode::ConstantKind::Path;
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t end = path.find('.', start);
      const std::string segment = end == std::string::npos
                                      ? path.substr(start)
                                      : path.substr(start, end - start);
      if (!segment.empty()) {
        constant.items.push_back(intern_symbol(segment));
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1U;
    }
    if (constant.items.empty()) {
      constant.items.push_back(intern_symbol(path));
    }
    const std::uint32_t id = static_cast<std::uint32_t>(out_.const_pool.size());
    out_.const_pool.push_back(std::move(constant));
    return id;
  }

  std::uint32_t map_string(NativeGraphModuleState &state, std::uint32_t id) {
    const auto found = state.strings.find(id);
    if (found != state.strings.end()) {
      return found->second;
    }
    const std::uint32_t mapped =
        intern_string(bc_string_or_empty(state.input->module, id));
    state.strings[id] = mapped;
    return mapped;
  }

  std::uint32_t map_symbol(NativeGraphModuleState &state, std::uint32_t id) {
    const auto found = state.symbols.find(id);
    if (found != state.symbols.end()) {
      return found->second;
    }
    const std::uint32_t mapped =
        intern_symbol(bc_symbol_or_empty(state.input->module, id));
    state.symbols[id] = mapped;
    return mapped;
  }

  std::uint32_t map_code(NativeGraphModuleState &state,
                         std::uint32_t id) const {
    const auto found = state.code_ids.find(id);
    return found == state.code_ids.end() ? 0U : found->second;
  }

  std::uint32_t map_constant(NativeGraphModuleState &state, std::uint32_t id) {
    const auto found = state.constants.find(id);
    if (found != state.constants.end()) {
      return found->second;
    }
    if (id >= state.input->module.const_pool.size()) {
      return 0;
    }
    amber::bytecode::Constant constant = state.input->module.const_pool[id];
    switch (constant.kind) {
    case amber::bytecode::ConstantKind::SymbolRef:
      constant.ref_id = map_symbol(state, constant.ref_id);
      break;
    case amber::bytecode::ConstantKind::StringRef:
      constant.ref_id = map_string(state, constant.ref_id);
      break;
    case amber::bytecode::ConstantKind::CodeRef:
      constant.ref_id = map_code(state, constant.ref_id);
      break;
    case amber::bytecode::ConstantKind::KeySet:
    case amber::bytecode::ConstantKind::Path:
      for (std::uint32_t &item : constant.items) {
        item = map_symbol(state, item);
      }
      break;
    case amber::bytecode::ConstantKind::Null:
    case amber::bytecode::ConstantKind::Bool:
    case amber::bytecode::ConstantKind::Integer:
    case amber::bytecode::ConstantKind::Float:
      break;
    }
    const std::uint32_t mapped =
        static_cast<std::uint32_t>(out_.const_pool.size());
    out_.const_pool.push_back(std::move(constant));
    state.constants[id] = mapped;
    return mapped;
  }

  std::uint32_t class_index(NativeGraphModuleState &state,
                            std::uint32_t old_index) const {
    return state.class_offset + old_index;
  }

  std::uint32_t method_index(NativeGraphModuleState &state,
                             std::uint32_t old_index) const {
    return state.method_offset + old_index;
  }

  std::uint32_t pattern_index(NativeGraphModuleState &state,
                              std::uint32_t old_index) const {
    return state.pattern_offset + old_index;
  }

  static void normalize_string_vector(std::vector<std::string> *values) {
    std::sort(values->begin(), values->end());
    values->erase(std::unique(values->begin(), values->end()), values->end());
  }

  void remap_symbol_operand(NativeGraphModuleState &state,
                            amber::bytecode::Instruction &insn,
                            std::size_t index) {
    if (index < insn.operands.size()) {
      insn.operands[index].value = map_symbol(
          state, static_cast<std::uint32_t>(insn.operands[index].value));
    }
  }

  void remap_constant_operand(NativeGraphModuleState &state,
                              amber::bytecode::Instruction &insn,
                              std::size_t index) {
    if (index < insn.operands.size()) {
      insn.operands[index].value = map_constant(
          state, static_cast<std::uint32_t>(insn.operands[index].value));
    }
  }

  void shift_target(amber::bytecode::Instruction &insn, std::size_t index,
                    std::uint32_t offset) {
    if (offset == 0U || index >= insn.operands.size()) {
      return;
    }
    insn.operands[index].value += offset;
  }

  void remap_instruction(NativeGraphModuleState &state,
                         amber::bytecode::Instruction &insn,
                         std::uint32_t pc_offset) {
    using amber::bytecode::Opcode;
    switch (insn.opcode) {
    case Opcode::LoadK:
      remap_constant_operand(state, insn, 1);
      break;
    case Opcode::LookupConst:
      remap_constant_operand(state, insn, 1);
      break;
    case Opcode::TypeCheck:
      remap_constant_operand(state, insn, 1);
      break;
    case Opcode::MakeClosure:
      if (insn.operands.size() > 1U) {
        insn.operands[1].value =
            map_code(state, static_cast<std::uint32_t>(insn.operands[1].value));
      }
      break;
    case Opcode::LoadIvar:
    case Opcode::LoadCvar:
      remap_symbol_operand(state, insn, 2);
      break;
    case Opcode::StoreIvar:
    case Opcode::StoreCvar:
      remap_symbol_operand(state, insn, 1);
      break;
    case Opcode::MakeMap: {
      if (insn.operands.size() >= 2U) {
        const std::uint32_t count =
            static_cast<std::uint32_t>(insn.operands[1].value) &
            amber::bytecode::kMapCountMask;
        for (std::uint32_t i = 0; i < count; ++i) {
          remap_symbol_operand(state, insn, 2U + i * 2U);
        }
      }
      break;
    }
    case Opcode::MakeMapSpread: {
      if (insn.operands.size() >= 2U) {
        const std::uint32_t count =
            static_cast<std::uint32_t>(insn.operands[1].value) &
            amber::bytecode::kMapCountMask;
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::size_t base = 2U + i * 3U;
          if (base + 1U < insn.operands.size() &&
              insn.operands[base].value ==
                  amber::bytecode::kMapSpreadEntrySymbol) {
            remap_symbol_operand(state, insn, base + 1U);
          }
        }
      }
      break;
    }
    case Opcode::Send:
    case Opcode::SendDyn:
    case Opcode::SendSpread:
    case Opcode::SendDynSpread: {
      const bool spread = insn.opcode == Opcode::SendSpread ||
                          insn.opcode == Opcode::SendDynSpread;
      if (insn.opcode == Opcode::Send || insn.opcode == Opcode::SendSpread) {
        remap_symbol_operand(state, insn, 2);
      }
      if (insn.operands.size() > 3U) {
        const std::uint32_t pos_count =
            static_cast<std::uint32_t>(insn.operands[3].value);
        const std::size_t per_arg = spread ? 2U : 1U;
        const std::size_t kw_index = 4U + pos_count * per_arg;
        if (kw_index < insn.operands.size()) {
          const std::uint32_t kw_count =
              static_cast<std::uint32_t>(insn.operands[kw_index].value);
          for (std::uint32_t i = 0; i < kw_count; ++i) {
            const std::size_t key_index =
                kw_index + 1U + i * (spread ? 3U : 2U) + (spread ? 1U : 0U);
            remap_symbol_operand(state, insn, key_index);
          }
        }
      }
      break;
    }
    case Opcode::CallSpread: {
      if (insn.operands.size() > 2U) {
        const std::uint32_t pos_count =
            static_cast<std::uint32_t>(insn.operands[2].value);
        const std::size_t kw_index = 3U + pos_count * 2U;
        if (kw_index < insn.operands.size()) {
          const std::uint32_t kw_count =
              static_cast<std::uint32_t>(insn.operands[kw_index].value);
          for (std::uint32_t i = 0; i < kw_count; ++i) {
            remap_symbol_operand(state, insn, kw_index + 1U + i * 3U + 1U);
          }
        }
      }
      break;
    }
    case Opcode::Call: {
      if (insn.operands.size() > 2U) {
        const std::uint32_t pos_count =
            static_cast<std::uint32_t>(insn.operands[2].value);
        const std::size_t kw_index = 3U + pos_count;
        if (kw_index < insn.operands.size()) {
          const std::uint32_t kw_count =
              static_cast<std::uint32_t>(insn.operands[kw_index].value);
          for (std::uint32_t i = 0; i < kw_count; ++i) {
            remap_symbol_operand(state, insn, kw_index + 1U + i * 2U);
          }
        }
      }
      break;
    }
    case Opcode::PCheckEq:
      remap_constant_operand(state, insn, 1);
      shift_target(insn, 2, pc_offset);
      break;
    case Opcode::PHasKey:
      remap_symbol_operand(state, insn, 1);
      shift_target(insn, 2, pc_offset);
      break;
    case Opcode::PGetKey:
      remap_symbol_operand(state, insn, 2);
      break;
    case Opcode::Jump:
      shift_target(insn, 0, pc_offset);
      break;
    case Opcode::JumpIfTrue:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull:
      shift_target(insn, 1, pc_offset);
      break;
    default:
      if (integer_k_opcode(insn.opcode)) {
        remap_constant_operand(state, insn, 2);
      }
      break;
    }
  }

  std::optional<NativeGraphExportRef>
  resolve_import(const std::string &dependency,
                 const std::string &source) const {
    const auto dep_state = states_.find(dependency);
    if (dep_state == states_.end()) {
      return std::nullopt;
    }
    const auto export_found = dep_state->second.exports.find(source);
    if (export_found == dep_state->second.exports.end()) {
      return std::nullopt;
    }
    return export_found->second;
  }

  std::vector<amber::bytecode::Instruction>
  import_seed_instructions(NativeGraphModuleState &state) {
    std::vector<amber::bytecode::Instruction> seeds;
    const std::string prefix = "amber.import.alias:";
    for (const amber::bytecode::AttrEntry &attr : state.input->module.attrs) {
      const std::string key =
          bc_string_or_empty(state.input->module, attr.key_str_id);
      const std::string value =
          bc_string_or_empty(state.input->module, attr.value_str_id);
      if (key.rfind(prefix, 0) != 0) {
        continue;
      }
      const std::vector<std::string> fields = split_tab_fields(value);
      if (fields.empty()) {
        continue;
      }
      if (fields[0] == "M") {
        if (fields.size() != 3U) {
          continue;
        }
        std::uint32_t slot = 0;
        if (!parse_u32_text(fields[2], &slot)) {
          continue;
        }
        const auto dep_state = states_.find(fields[1]);
        if (dep_state == states_.end() || !dep_state->second.input->stdlib) {
          throw std::runtime_error(
              "whole-graph native does not support non-stdlib module import "
              "aliases in executable package objects: " +
              key.substr(prefix.size()));
        }
        const std::uint32_t const_id = intern_path_constant(fields[1]);
        seeds.push_back({amber::bytecode::Opcode::LookupConst,
                         {{slot, false}, {const_id, false}}});
        continue;
      }
      if (fields[0] != "F" || fields.size() != 4U) {
        continue;
      }
      std::uint32_t slot = 0;
      if (!parse_u32_text(fields[3], &slot)) {
        continue;
      }
      const std::optional<NativeGraphExportRef> target =
          resolve_import(fields[1], fields[2]);
      if (!target.has_value()) {
        throw std::runtime_error("whole-graph native import `" + fields[2] +
                                 "` from module `" + fields[1] +
                                 "` is not resolved");
      }
      // Resolve the import to the exporting module's ALREADY-BUILT binding
      // (via LookupConst -> module_bindings) rather than rebuilding a closure
      // here. A rebuilt `MakeClosure(code_id, 0 captures)` dropped the callee's
      // upvalues, so an imported function that closes over other module-level
      // bindings ran with no captures ("capture slot out of range"). The
      // exporting module's init runs first (dependency order) and persists the
      // real closure (with captures) under `<module>:<name>`, which the
      // extended lookup_constant recovers. Classes resolve the same way.
      if (target->kind == "method" || target->kind == "code" ||
          target->kind == "class") {
        const std::uint32_t const_id =
            intern_path_constant(fields[1] + "." + fields[2]);
        seeds.push_back({amber::bytecode::Opcode::LookupConst,
                         {{slot, false}, {const_id, false}}});
      }
    }
    return seeds;
  }

  void shift_instruction_targets_from(amber::bytecode::BcCode *code,
                                      std::uint32_t from_pc,
                                      std::uint32_t offset) {
    if (offset == 0U) {
      return;
    }
    const auto shift_operand = [&](amber::bytecode::Instruction &insn,
                                   std::size_t index) {
      if (index >= insn.operands.size() ||
          insn.operands[index].signed_immediate ||
          insn.operands[index].value < 0) {
        return;
      }
      const std::uint32_t target =
          static_cast<std::uint32_t>(insn.operands[index].value);
      if (target >= from_pc) {
        insn.operands[index].value = static_cast<std::int64_t>(target + offset);
      }
    };
    for (amber::bytecode::Instruction &insn : code->instructions) {
      switch (insn.opcode) {
      case amber::bytecode::Opcode::Jump:
        shift_operand(insn, 0);
        break;
      case amber::bytecode::Opcode::JumpIfTrue:
      case amber::bytecode::Opcode::JumpIfFalse:
      case amber::bytecode::Opcode::JumpIfNull:
        shift_operand(insn, 1);
        break;
      default:
        break;
      }
    }
  }

  std::uint32_t
  import_seed_insert_pc(const amber::bytecode::BcCode &code) const {
    std::set<std::uint32_t> import_slots;
    for (const amber::bytecode::SlotLayoutEntry &entry : code.local_layout) {
      const std::string binding =
          entry.binding_kind_str_id < out_.strings.size()
              ? out_.strings[entry.binding_kind_str_id]
              : std::string();
      if (binding == "import_alias") {
        import_slots.insert(entry.slot);
      }
    }
    std::uint32_t pc = 0;
    while (pc < code.instructions.size()) {
      const amber::bytecode::Instruction &insn = code.instructions[pc];
      if (insn.opcode != amber::bytecode::Opcode::LoadNull ||
          insn.operands.empty() || insn.operands[0].signed_immediate ||
          insn.operands[0].value < 0) {
        break;
      }
      const std::uint32_t slot =
          static_cast<std::uint32_t>(insn.operands[0].value);
      if (import_slots.find(slot) == import_slots.end()) {
        break;
      }
      ++pc;
    }
    return pc;
  }

  void shift_code_pcs(amber::bytecode::BcCode *code, std::uint32_t offset,
                      std::uint32_t from_pc = 0) {
    if (offset == 0U) {
      return;
    }
    for (amber::bytecode::HandlerEntry &entry : code->handler_table) {
      if (entry.protected_from >= from_pc) {
        entry.protected_from += offset;
      }
      if (entry.protected_to >= from_pc) {
        entry.protected_to += offset;
      }
      if (entry.handler_pc >= from_pc) {
        entry.handler_pc += offset;
      }
    }
    for (amber::bytecode::CacheSiteEntry &entry : code->call_site_table) {
      if (entry.pc >= from_pc) {
        entry.pc += offset;
      }
    }
    for (amber::bytecode::CacheSiteEntry &entry : code->ivar_site_table) {
      if (entry.pc >= from_pc) {
        entry.pc += offset;
      }
    }
    for (amber::bytecode::SafepointEntry &entry : code->safepoint_table) {
      if (entry.pc >= from_pc) {
        entry.pc += offset;
      }
    }
    for (amber::bytecode::SourceSpanEntry &entry : code->source_spans) {
      if (entry.pc_from >= from_pc) {
        entry.pc_from += offset;
      }
      if (entry.pc_to > from_pc) {
        entry.pc_to += offset;
      }
    }
  }

  amber::bytecode::BcCode remap_code(NativeGraphModuleState &state,
                                     const amber::bytecode::BcCode &source,
                                     std::uint32_t pc_offset) {
    amber::bytecode::BcCode code = source;
    code.code_id = map_code(state, source.code_id);
    for (amber::bytecode::SlotLayoutEntry &entry : code.local_layout) {
      entry.name_str_id = map_string(state, entry.name_str_id);
      entry.role_str_id = map_string(state, entry.role_str_id);
      entry.binding_kind_str_id = map_string(state, entry.binding_kind_str_id);
    }
    for (amber::bytecode::CaptureLayoutEntry &entry : code.capture_layout) {
      entry.name_str_id = map_string(state, entry.name_str_id);
      entry.source_kind_str_id = map_string(state, entry.source_kind_str_id);
      entry.source_name_str_id = map_string(state, entry.source_name_str_id);
    }
    for (amber::bytecode::Instruction &insn : code.instructions) {
      remap_instruction(state, insn, pc_offset);
    }
    for (amber::bytecode::HandlerEntry &entry : code.handler_table) {
      entry.handler_code_id = map_code(state, entry.handler_code_id);
    }
    for (amber::bytecode::CacheSiteEntry &entry : code.call_site_table) {
      entry.symbol_id = map_symbol(state, entry.symbol_id);
    }
    for (amber::bytecode::CacheSiteEntry &entry : code.ivar_site_table) {
      entry.symbol_id = map_symbol(state, entry.symbol_id);
    }
    shift_code_pcs(&code, pc_offset);
    return code;
  }

  void copy_module(NativeGraphModuleState &state) {
    out_.required_features.insert(out_.required_features.end(),
                                  state.input->module.required_features.begin(),
                                  state.input->module.required_features.end());
    out_.optional_features.insert(out_.optional_features.end(),
                                  state.input->module.optional_features.begin(),
                                  state.input->module.optional_features.end());
    out_.forbidden_features.insert(
        out_.forbidden_features.end(),
        state.input->module.forbidden_features.begin(),
        state.input->module.forbidden_features.end());
    out_.capabilities.insert(out_.capabilities.end(),
                             state.input->module.capabilities.begin(),
                             state.input->module.capabilities.end());
    out_.effects.insert(out_.effects.end(), state.input->module.effects.begin(),
                        state.input->module.effects.end());
    out_.schemas.insert(out_.schemas.end(), state.input->module.schemas.begin(),
                        state.input->module.schemas.end());
    out_.schema_migrations.insert(out_.schema_migrations.end(),
                                  state.input->module.schema_migrations.begin(),
                                  state.input->module.schema_migrations.end());
    out_.table_plans.insert(out_.table_plans.end(),
                            state.input->module.table_plans.begin(),
                            state.input->module.table_plans.end());
    out_.wasm_components.insert(out_.wasm_components.end(),
                                state.input->module.wasm_components.begin(),
                                state.input->module.wasm_components.end());
    out_.accelerator_kernels.insert(
        out_.accelerator_kernels.end(),
        state.input->module.accelerator_kernels.begin(),
        state.input->module.accelerator_kernels.end());
    out_.agent_symbols.insert(out_.agent_symbols.end(),
                              state.input->module.agent_symbols.begin(),
                              state.input->module.agent_symbols.end());
    out_.agent_patches.insert(out_.agent_patches.end(),
                              state.input->module.agent_patches.begin(),
                              state.input->module.agent_patches.end());
    out_.provenance_records.insert(
        out_.provenance_records.end(),
        state.input->module.provenance_records.begin(),
        state.input->module.provenance_records.end());
    out_.contracts.insert(out_.contracts.end(),
                          state.input->module.contracts.begin(),
                          state.input->module.contracts.end());
    out_.properties.insert(out_.properties.end(),
                           state.input->module.properties.begin(),
                           state.input->module.properties.end());
    out_.privacy_labels.insert(out_.privacy_labels.end(),
                               state.input->module.privacy_labels.begin(),
                               state.input->module.privacy_labels.end());
    out_.privacy_policies.insert(out_.privacy_policies.end(),
                                 state.input->module.privacy_policies.begin(),
                                 state.input->module.privacy_policies.end());
    out_.lineage_nodes.insert(out_.lineage_nodes.end(),
                              state.input->module.lineage_nodes.begin(),
                              state.input->module.lineage_nodes.end());
    out_.workflow_steps.insert(out_.workflow_steps.end(),
                               state.input->module.workflow_steps.begin(),
                               state.input->module.workflow_steps.end());
    out_.workflow_history.insert(out_.workflow_history.end(),
                                 state.input->module.workflow_history.begin(),
                                 state.input->module.workflow_history.end());

    for (std::uint32_t i = 0; i < state.input->module.const_pool.size(); ++i) {
      map_constant(state, i);
    }

    std::vector<amber::bytecode::Instruction> import_seeds =
        import_seed_instructions(state);
    for (const amber::bytecode::BcCode &source_code :
         state.input->module.code_objects) {
      const bool is_init =
          state.input->module.init.has_entry_code_id &&
          source_code.code_id == state.input->module.init.entry_code_id;
      state.code_pc_offsets[source_code.code_id] = 0;
      state.code_pc_insert_points[source_code.code_id] = 0;
      amber::bytecode::BcCode code = remap_code(state, source_code, 0);
      if (is_init && !import_seeds.empty()) {
        const std::uint32_t insert_pc = import_seed_insert_pc(code);
        const std::uint32_t pc_offset =
            static_cast<std::uint32_t>(import_seeds.size());
        state.code_pc_offsets[source_code.code_id] = pc_offset;
        state.code_pc_insert_points[source_code.code_id] = insert_pc;
        shift_instruction_targets_from(&code, insert_pc, pc_offset);
        shift_code_pcs(&code, pc_offset, insert_pc);
        code.instructions.insert(code.instructions.begin() + insert_pc,
                                 import_seeds.begin(), import_seeds.end());
      }
      out_.code_objects.push_back(std::move(code));
    }

    for (const amber::bytecode::BcMethod &source :
         state.input->module.methods) {
      amber::bytecode::BcMethod method = source;
      method.selector_sym_id = map_symbol(state, method.selector_sym_id);
      if ((method.flags & (amber::bytecode::kMethodFlagInstance |
                           amber::bytecode::kMethodFlagClass |
                           amber::bytecode::kMethodFlagPropertyGetter |
                           amber::bytecode::kMethodFlagPropertySetter)) != 0U) {
        method.owner_dispatch_ref =
            class_index(state, method.owner_dispatch_ref);
      }
      method.signature_blob_id = map_constant(state, method.signature_blob_id);
      for (amber::bytecode::MethodParamEntry &param : method.params) {
        param.external_name_sym_id =
            map_symbol(state, param.external_name_sym_id);
        param.local_name_str_id = map_string(state, param.local_name_str_id);
      }
      for (std::uint32_t &code_id : method.default_thunk_ids) {
        code_id = map_code(state, code_id);
      }
      for (std::uint32_t &code_id : method.type_hook_ids) {
        code_id = map_code(state, code_id);
      }
      for (amber::bytecode::ClauseEntry &entry : method.clause_table) {
        entry.pattern_program_id =
            pattern_index(state, entry.pattern_program_id);
        entry.pattern_code_id = map_code(state, entry.pattern_code_id);
        entry.guard_code_id = map_code(state, entry.guard_code_id);
        entry.body_code_id = map_code(state, entry.body_code_id);
      }
      for (amber::bytecode::AutoAssignEntry &entry : method.auto_assign_desc) {
        entry.local_name_str_id = map_string(state, entry.local_name_str_id);
        entry.target_name_str_id = map_string(state, entry.target_name_str_id);
      }
      method.entry_code_id = map_code(state, method.entry_code_id);
      out_.methods.push_back(std::move(method));
    }

    for (const amber::bytecode::BcClass &source : state.input->module.classes) {
      amber::bytecode::BcClass klass = source;
      klass.class_name_sym_id = map_symbol(state, klass.class_name_sym_id);
      if (klass.has_superclass_ref) {
        klass.superclass_ref = map_constant(state, klass.superclass_ref);
      }
      klass.ivar_schema_id = map_constant(state, klass.ivar_schema_id);
      klass.method_range_start = method_index(state, klass.method_range_start);
      for (std::uint32_t &ref : klass.direct_include_refs) {
        ref = map_constant(state, ref);
      }
      for (std::uint32_t &ref : klass.direct_extend_refs) {
        ref = map_constant(state, ref);
      }
      if (klass.has_class_init_code_id) {
        klass.class_init_code_id = map_code(state, klass.class_init_code_id);
      }
      out_.classes.push_back(std::move(klass));
    }

    for (const amber::bytecode::PatternProgramEntry &source :
         state.input->module.pattern_programs) {
      amber::bytecode::PatternProgramEntry entry = source;
      out_.pattern_programs.push_back(entry);
    }

    for (const amber::bytecode::ExportEntry &source :
         state.input->module.exports) {
      amber::bytecode::ExportEntry entry = source;
      entry.symbol_id = map_symbol(state, entry.symbol_id);
      entry.target_kind_str_id = map_string(state, entry.target_kind_str_id);
      entry.reexport_module_name_str_id =
          map_string(state, entry.reexport_module_name_str_id);
      const std::string kind =
          bc_string_or_empty(state.input->module, source.target_kind_str_id);
      NativeGraphExportRef ref;
      ref.kind = kind;
      if (kind == "method") {
        entry.target_index = method_index(state, source.target_index);
        const amber::bytecode::BcMethod &method =
            state.input->module.methods[source.target_index];
        ref.method_index = entry.target_index;
        ref.code_id = map_code(state, method.entry_code_id);
      } else if (kind == "code") {
        entry.target_index = map_code(state, source.target_index);
        ref.code_id = entry.target_index;
      } else if (kind == "class") {
        entry.target_index = class_index(state, source.target_index);
        ref.class_index = entry.target_index;
      } else {
        entry.target_index = source.target_index;
      }
      out_.exports.push_back(entry);
      state.exports[bc_symbol_or_empty(state.input->module, source.symbol_id)] =
          ref;
    }

    for (const amber::bytecode::LineEntry &source :
         state.input->module.line_table) {
      amber::bytecode::LineEntry entry = source;
      entry.code_id = map_code(state, source.code_id);
      const auto offset = state.code_pc_offsets.find(source.code_id);
      const auto insert = state.code_pc_insert_points.find(source.code_id);
      const std::uint32_t insert_pc =
          insert == state.code_pc_insert_points.end() ? 0U : insert->second;
      if (offset != state.code_pc_offsets.end() && entry.pc >= insert_pc) {
        entry.pc += offset->second;
      }
      out_.line_table.push_back(entry);
    }
    for (const amber::bytecode::LocalDebugEntry &source :
         state.input->module.local_debug) {
      amber::bytecode::LocalDebugEntry entry = source;
      entry.code_id = map_code(state, source.code_id);
      entry.name_str_id = map_string(state, source.name_str_id);
      const auto offset = state.code_pc_offsets.find(source.code_id);
      const auto insert = state.code_pc_insert_points.find(source.code_id);
      const std::uint32_t insert_pc =
          insert == state.code_pc_insert_points.end() ? 0U : insert->second;
      if (offset != state.code_pc_offsets.end()) {
        if (entry.start_pc >= insert_pc) {
          entry.start_pc += offset->second;
        }
        if (entry.end_pc > insert_pc) {
          entry.end_pc += offset->second;
        }
      }
      out_.local_debug.push_back(entry);
    }

    for (const amber::bytecode::AttrEntry &source : state.input->module.attrs) {
      const std::string raw_key =
          bc_string_or_empty(state.input->module, source.key_str_id);
      const std::string raw_value =
          bc_string_or_empty(state.input->module, source.value_str_id);
      if (raw_key.rfind("amber.import.alias:", 0) == 0) {
        continue;
      }
      std::string key = raw_key;
      if (raw_key.rfind("amber.native.bind:", 0) == 0) {
        std::uint32_t old_code = 0;
        if (parse_u32_text(
                raw_key.substr(std::string("amber.native.bind:").size()),
                &old_code)) {
          key =
              "amber.native.bind:" + std::to_string(map_code(state, old_code));
        }
      }
      out_.attrs.push_back({intern_string(key), intern_string(raw_value)});
    }
    if (state.input->module.init.has_entry_code_id) {
      add_module_attr(&out_,
                      "amber.merged.init:" +
                          std::to_string(map_code(
                              state, state.input->module.init.entry_code_id)),
                      state.input->name);
      init_code_ids_.push_back(
          map_code(state, state.input->module.init.entry_code_id));
    }
  }

  void synthesize_merged_init() {
    const std::uint32_t wrapper_id = next_code_id_++;
    amber::bytecode::BcCode wrapper;
    wrapper.code_id = wrapper_id;
    wrapper.kind = amber::bytecode::CodeKind::Module;
    wrapper.reg_count = 2;
    std::uint32_t pc = 0;
    for (const std::uint32_t code_id : init_code_ids_) {
      wrapper.instructions.push_back(
          {amber::bytecode::Opcode::MakeClosure,
           {{0, false}, {code_id, false}, {0, false}}});
      wrapper.instructions.push_back(
          {amber::bytecode::Opcode::Call,
           {{1, false},
            {0, false},
            {0, false},
            {0, false},
            {-1, true},
            {static_cast<std::int64_t>(wrapper.call_site_table.size()),
             false}}});
      wrapper.call_site_table.push_back(
          {pc + 1U, static_cast<std::uint32_t>(wrapper.call_site_table.size()),
           intern_symbol("<call>"), 0});
      pc += 2U;
    }
    if (init_code_ids_.empty()) {
      wrapper.instructions.push_back(
          {amber::bytecode::Opcode::LoadNull, {{1, false}}});
    }
    wrapper.instructions.push_back(
        {amber::bytecode::Opcode::Return, {{1, false}}});
    out_.code_objects.push_back(std::move(wrapper));
    out_.init = {true, wrapper_id, 0};
    add_module_attr(&out_, "amber.merged.wrapper:" + std::to_string(wrapper_id),
                    "true");
  }

  std::vector<NativeGraphModule> order_;
  amber::bytecode::BcModule out_;
  std::unordered_map<std::string, NativeGraphModuleState> states_;
  std::unordered_map<std::string, std::uint32_t> string_ids_;
  std::unordered_map<std::string, std::uint32_t> symbol_ids_;
  std::vector<std::uint32_t> init_code_ids_;
  std::uint32_t next_code_id_ = 1;
};

std::vector<NativeGraphModule>
decode_native_graph_modules(const amber::build::BuildSummary &summary) {
  std::vector<NativeGraphModule> modules;
  for (const amber::build::BuildArtifactRecord &record : summary.artifacts) {
    NativeGraphModule module;
    module.name = record.name;
    module.path = record.path;
    module.stdlib = record.stdlib;
    module.bytes = read_bytes(record.output_path);
    amber::bytecode::DecodeResult decoded =
        amber::bytecode::deserialize_module(module.bytes);
    if (!decoded.ok()) {
      throw std::runtime_error(
          amber::bytecode::verify_errors_to_json(decoded.errors));
    }
    module.module = std::move(decoded.module);
    modules.push_back(std::move(module));
  }
  return modules;
}

NativeGraphLinkResult
link_native_graph(const std::vector<NativeGraphModule> &modules,
                  const std::string &root_module, EntryExecutionMode entry_mode,
                  const std::vector<amber::capability::CapabilityRequest>
                      &capability_grants) {
  NativeGraphLinkResult result;
  std::map<std::string, const NativeGraphModule *> by_name;
  for (const NativeGraphModule &module : modules) {
    by_name[module.name] = &module;
  }
  if (by_name.find(root_module) == by_name.end()) {
    result.diagnostics.push_back(
        {"BuildError", "root build artifact is missing: " + root_module,
         root_module});
    return result;
  }
  std::set<std::string> visiting;
  std::set<std::string> visited;
  std::vector<NativeGraphModule> order;
  std::function<bool(const std::string &)> visit =
      [&](const std::string &name) {
        if (visited.count(name) != 0U) {
          return true;
        }
        if (!visiting.insert(name).second) {
          result.diagnostics.push_back(
              {"BuildError", "module dependency cycle in native graph: " + name,
               name});
          return false;
        }
        const auto found = by_name.find(name);
        if (found == by_name.end()) {
          result.diagnostics.push_back(
              {"BuildError",
               "module dependency is missing from native graph: " + name,
               name});
          return false;
        }
        const NativeGraphModule &module = *found->second;
        for (const amber::bytecode::DepEntry &dep :
             module.module.dependencies) {
          const std::string dep_name =
              bc_string_or_empty(module.module, dep.module_name_str_id);
          if (!dep_name.empty() && !visit(dep_name)) {
            return false;
          }
        }
        visiting.erase(name);
        visited.insert(name);
        order.push_back(module);
        return true;
      };
  if (!visit(root_module)) {
    return result;
  }
  try {
    return NativeGraphModuleBuilder(std::move(order))
        .build(root_module, entry_mode, capability_grants);
  } catch (const std::exception &error) {
    result.diagnostics.push_back({"BuildError", error.what(), root_module});
    return result;
  }
}

int run_build_command(int argc, char **argv) {
  if (argc < 3 || std::string(argv[1]) != "build") {
    usage(std::cerr);
    return 2;
  }

  const std::string manifest_path = argv[2];
  if (is_amber_source_path(manifest_path)) {
    return run_source_build_command(argc, argv);
  }
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
  summary.target = options.target;
  summary.out_dir = out_dir.string();
  summary.cache_dir = cache_dir.string();

  const amber::build::BuildManifestResult parsed =
      amber::build::parse_build_manifest(read_file(manifest_path),
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
    // Cross-module macro staging (§11): a parse-only pre-pass harvests every
    // module's `export macro` table before anything compiles, so staging is
    // independent of build order (the manifest parser sorts modules by name).
    // Providers do not need to be *built* first — the importer's expansion
    // clones provider macro-def ASTs into its own macro module. Provider
    // source hashes salt every cache key: expansion output, not just
    // linkage, depends on provider sources.
    amber::macros::MacroProviderMap macro_providers;
    std::string macro_material;
    for (const amber::build::BuildModule &module : parsed.manifest.modules) {
      const std::string module_source =
          read_file((manifest_dir / module.path).string());
      const std::string module_source_hash =
          amber::lexer::sha256_hex(module_source);
      // Prefer the persisted artifact macro section of a fresh build output
      // (§11); fall back to the parse-only source harvest.
      std::vector<amber::macros::MacroExport> exports;
      std::optional<std::vector<amber::macros::MacroExport>> from_artifact =
          macro_exports_from_artifact(
              out_dir / (safe_artifact_name(module.name) + ".amberbc"),
              module_source_hash, module.path);
      if (from_artifact.has_value()) {
        exports = std::move(*from_artifact);
      } else {
        exports = harvest_macro_exports(module_source, module.path);
      }
      if (!exports.empty()) {
        macro_material +=
            "macro-provider\n" + module.name + "\n" + module_source_hash + "\n";
        macro_providers[module.name] = std::move(exports);
      }
    }
    for (const amber::build::BuildModule &module : parsed.manifest.modules) {
      summary.artifacts.push_back(build_one_module(
          module, manifest_dir, out_dir, cache_dir, parsed.manifest.profiles,
          stdlib_abis, options.cache_enabled, &macro_providers,
          macro_material));
    }
    if (options.target == "native" || options.target == "both") {
      amber::build::BuildArtifactRecord *root_record = nullptr;
      for (amber::build::BuildArtifactRecord &artifact : summary.artifacts) {
        if (!artifact.stdlib && artifact.name == parsed.manifest.root_module) {
          root_record = &artifact;
          break;
        }
      }
      if (root_record == nullptr) {
        throw std::runtime_error("root build artifact is missing: " +
                                 parsed.manifest.root_module);
      }
      const std::vector<std::uint8_t> root_bytes =
          read_bytes(root_record->output_path);
      amber::bytecode::DecodeResult decoded =
          amber::bytecode::deserialize_module(root_bytes);
      if (!decoded.ok()) {
        throw std::runtime_error(
            amber::bytecode::verify_errors_to_json(decoded.errors));
      }
      const EntryExecutionMode entry_mode =
          default_entry_mode_for(true, decoded.module);
      const NativeGraphLinkResult linked_graph =
          link_native_graph(decode_native_graph_modules(summary),
                            parsed.manifest.root_module, entry_mode, {});
      if (!linked_graph.ok) {
        for (const amber::build::BuildDiagnostic &diagnostic :
             linked_graph.diagnostics) {
          summary.diagnostics.push_back(diagnostic);
        }
        throw std::runtime_error(
            linked_graph.diagnostics.empty()
                ? "native graph link failed"
                : linked_graph.diagnostics.front().message);
      }
      const std::filesystem::path native_output_path =
          out_dir / safe_artifact_name(parsed.manifest.root_module);
      const std::filesystem::path native_source_path =
          out_dir /
          (safe_artifact_name(parsed.manifest.root_module) + ".native.cpp");
      const std::vector<amber::pkg::PackageNativeExtension> native_extensions =
          augment_native_extensions_from_source(
              parsed.manifest.native_extensions, linked_graph.artifact.module);
      const std::vector<amber::pkg::PackageNativeBlob> native_blobs =
          collect_native_blobs(native_extensions, manifest_dir.string());
      const NativeExecutableBuildResult native_result = build_native_executable(
          argv[0], linked_graph.artifact, native_output_path,
          native_source_path, native_extensions, manifest_dir);
      summary.native_output_path = native_result.output_path;
      summary.native_backend = native_result.backend;
      summary.native_hash = native_result.hash;
      summary.native_launcher_source = native_result.source_path;
      summary.native_cxx = native_result.cxx;
      summary.native_bytecode_trampoline = native_result.uses_bytecode_fallback;
      summary.native_graph_module_count =
          linked_graph.artifact.graph_module_count;
      summary.native_graph_code_count = native_result.total_code_count;
      summary.native_graph_native_code_count = native_result.native_code_count;
      summary.native_graph_vm_fallback_code_count =
          native_result.vm_callable_code_count;
      summary.native_graph_fallback_code_count =
          native_result.fallback_code_count;
      summary.native_graph_full_coverage = native_result.full_native_coverage;
      summary.native_extensions = amber::pkg::native_extension_metadata(
          native_extensions, native_blobs, native_target_triple());
      root_record->native_output_path = native_result.output_path;
      root_record->native_hash = native_result.hash;
      root_record->native_backend = native_result.backend;
      root_record->native_eligible = native_result.entry_native;
      root_record->native_fallback_reason = native_result.fallback_reason;
      root_record->native_byte_size =
          std::filesystem::file_size(native_output_path);
      if (options.require_full_native) {
        const std::string diagnostic =
            full_native_requirement_diagnostic(native_result);
        if (!diagnostic.empty()) {
          summary.ok = false;
          summary.diagnostics.push_back(
              {"NativeCoverageError", diagnostic, manifest_path});
          std::cout << amber::build::summary_to_json(summary);
          return 1;
        }
      }
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
    amber::pkg::PackageBuildOptions package_options =
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
    package_options.target_triple = native_target_triple();
    package_options.native_blobs =
        collect_native_blobs(manifest.manifest.native_extensions, root_dir);
    // Cross-module macro staging (§11): same parse-only provider pre-pass as
    // the build/package paths, so image builds expand imported macros too.
    amber::macros::MacroProviderMap macro_providers;
    for (const amber::pkg::PackageModule &module : manifest.manifest.modules) {
      std::vector<amber::macros::MacroExport> exports = harvest_macro_exports(
          read_file(join_path(root_dir, module.path)), module.path);
      if (!exports.empty()) {
        macro_providers[module.name] = std::move(exports);
      }
    }
    for (const amber::pkg::PackageModule &module : manifest.manifest.modules) {
      CompiledModuleArtifact compiled = compile_source_to_module_artifact(
          join_path(root_dir, module.path), module.name,
          manifest.manifest.capabilities, &macro_providers);
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

int run_bytecode_artifact_command(const std::string &command,
                                  const std::string &path,
                                  bool structured_decode_errors) {
  const std::string binary = read_file(path);
  const std::vector<std::uint8_t> bytes(binary.begin(), binary.end());
  amber::bytecode::DecodeResult decode_result =
      amber::bytecode::deserialize_module(bytes);
  if (!decode_result.ok()) {
    const std::string errors =
        amber::bytecode::verify_errors_to_json(decode_result.errors);
    if (structured_decode_errors) {
      std::cout << errors;
    } else {
      std::cerr << errors;
    }
    return 1;
  }

  if (command == "verify" || command == "amberbc-verify") {
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

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--version") {
      std::cout << "amberc 0.1.0-dev\n";
      return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "run-embedded") {
      return run_embedded_command(argc, argv);
    }
    if (argc == 2 && is_amber_source_path(argv[1])) {
      return run_source_file_command(argv[1]);
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
    if (argc >= 2 && (std::string(argv[1]) == "metadata" ||
                      std::string(argv[1]) == "verify")) {
      if (argc != 4 || std::string(argv[3]) != "--json") {
        usage(std::cerr);
        return 2;
      }
      return run_bytecode_artifact_command(argv[1], argv[2], true);
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
      return run_bytecode_artifact_command(command, path, false);
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
              RunnableModuleArtifact cpp_artifact;
              cpp_artifact.module_name =
                  parse_result.module_name.empty()
                      ? synthetic_module_name_for_path(path)
                      : parse_result.module_name;
              cpp_artifact.entry_mode = default_entry_mode_for(
                  !parse_result.module_name.empty(), decode_result.module);
              cpp_artifact.bytes = bytes;
              cpp_artifact.module = decode_result.module;
              if (cpp_artifact.module.init.has_entry_code_id) {
                cpp_artifact.has_entry_init_code_id = true;
                cpp_artifact.entry_init_code_id =
                    cpp_artifact.module.init.entry_code_id;
              }
              if (const amber::bytecode::BcMethod *main_method =
                      zero_arg_method_by_name(cpp_artifact.module, "main")) {
                cpp_artifact.has_entry_main_code_id = true;
                cpp_artifact.entry_main_code_id = main_method->entry_code_id;
              }
              const NativeCppBuildPlan cpp_plan =
                  build_native_cpp_plan(cpp_artifact, decode_result.module);
              std::cout << amber::native::module_to_dump(native_module,
                                                         source_hash)
                        << native_cpp_coverage_to_dump(cpp_plan);
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
