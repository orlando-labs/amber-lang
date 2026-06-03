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
#include "runtime/vm.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
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
  out << "  amberc <file.am>\n";
  out << "  amberc build <file.am> [-o <path>] [--out-dir <dir>] "
         "[--target native|native-debug|bytecode-wrapper]\n";
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
        method.params.empty() && method.flags == 0) {
      return &method;
    }
  }
  return nullptr;
}

enum class EntryExecutionMode { Init, MainAfterInit };

std::string entry_mode_name(EntryExecutionMode mode) {
  return mode == EntryExecutionMode::MainAfterInit ? "main" : "init";
}

EntryExecutionMode parse_entry_mode(const std::string &mode) {
  if (mode == "init") {
    return EntryExecutionMode::Init;
  }
  if (mode == "main") {
    return EntryExecutionMode::MainAfterInit;
  }
  throw std::runtime_error("unknown embedded executable entry mode: " + mode);
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
};

RunnableModuleArtifact compile_source_to_runnable_module(
    const std::string &path,
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
  const EntryExecutionMode entry_mode =
      default_entry_mode_for(has_package, emit_result.module);
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
  return artifact;
}

amber::runtime::ExecutionResult
execute_runnable_module(const amber::bytecode::BcModule &module,
                        EntryExecutionMode mode) {
  amber::runtime::RuntimeWorld world(module);
  amber::runtime::ExecutionResult init_result;
  if (module.init.has_entry_code_id) {
    init_result = world.execute(module.init.entry_code_id);
    if (!init_result.ok()) {
      return init_result;
    }
  }

  if (mode == EntryExecutionMode::MainAfterInit) {
    const amber::bytecode::BcMethod *main_method =
        zero_arg_method_by_name(module, "main");
    if (main_method == nullptr) {
      return {amber::runtime::Value::null(),
              amber::runtime::Fault{
                  "EntryError",
                  "entry mode requires a zero-argument main() method", 0, 0}};
    }
    return world.execute(main_method->entry_code_id);
  }

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
    std::cout << amber::runtime::value_to_debug_string(result.value,
                                                       &decode_result.module)
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

struct NativeCppBuildPlan {
  std::string source;
  std::set<std::uint32_t> native_code_ids;
  std::string fallback_reason;
  std::string backend = "cpp-bytecode-direct-v1";
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
  std::size_t total_code_count = 0;
  bool entry_native = false;
  bool uses_bytecode_fallback = true;
  std::string fallback_reason;
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

std::string native_cpp_next(std::size_t pc, std::size_t size) {
  if (pc + 1U < size) {
    return "goto pc_" + std::to_string(pc + 1U) + ";";
  }
  return "throw NativeBailout();";
}

bool native_cpp_scalar_selector(const amber::bytecode::BcModule &module,
                                std::uint32_t symbol_id,
                                std::string *selector) {
  if (symbol_id >= module.symbols.size()) {
    return false;
  }
  *selector = module.symbols[symbol_id];
  return *selector == "+" || *selector == "-" || *selector == "*" ||
         *selector == "/" || *selector == "<" || *selector == ">";
}

bool native_cpp_code_supported(const amber::bytecode::BcModule &module,
                               const amber::bytecode::BcCode &code,
                               std::string *reason) {
  if (!code.handler_table.empty() || !code.capture_layout.empty()) {
    *reason = "exceptions and captures still use VM fallback";
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
                                  instruction.opcode == Opcode::ICmpK;
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
          kind != amber::bytecode::ConstantKind::Integer) {
        *reason = "non-scalar constants still use VM fallback";
        return false;
      }
      break;
    }
    case Opcode::LoadNull:
    case Opcode::LoadBool:
    case Opcode::Move:
    case Opcode::GetLast:
    case Opcode::SetLast:
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
    case Opcode::Jump:
    case Opcode::JumpIfTrue:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull:
    case Opcode::Return:
    case Opcode::Safepoint:
    case Opcode::CloseUpvalues:
      break;
    case Opcode::MakeClosure: {
      std::uint32_t ignored_dst = 0;
      std::uint32_t code_id = 0;
      std::uint32_t capture_count = 0;
      if (!operand_u32_value(instruction, 0, &ignored_dst) ||
          !operand_u32_value(instruction, 1, &code_id) ||
          !operand_u32_value(instruction, 2, &capture_count) ||
          capture_count != 0U) {
        *reason = "closures with captures still use VM fallback";
        return false;
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
          !native_cpp_scalar_selector(module, symbol_id, &selector) ||
          !operand_u32_value(instruction, 3, &pos_count) || pos_count != 1U) {
        *reason = "non-integer SEND still uses VM fallback";
        return false;
      }
      std::uint32_t kw_count = 0;
      if (!operand_u32_value(instruction, 5, &kw_count) || kw_count != 0U ||
          !operand_is_no_block(instruction, 6)) {
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
  default:
    return "NativeValue::nullv()";
  }
}

std::string
emit_native_cpp_code_function(const amber::bytecode::BcModule &module,
                              const amber::bytecode::BcCode &code) {
  std::ostringstream out;
  const std::string fn = native_cpp_function_name(code.code_id);
  out << "static NativeValue " << fn
      << "(const std::vector<NativeValue> &args) {\n";
  out << "  std::vector<NativeValue> r(" << code.reg_count << ");\n";
  out << "  NativeValue last = NativeValue::nullv();\n";
  out << "  for (std::size_t i = 0; i < args.size() && i < r.size(); ++i) "
         "{ r[i] = args[i]; }\n";
  if (code.instructions.empty()) {
    out << "  return NativeValue::nullv();\n";
    out << "}\n\n";
    return out.str();
  }
  out << "  goto pc_0;\n";
  for (std::size_t pc = 0; pc < code.instructions.size(); ++pc) {
    const amber::bytecode::Instruction &instruction = code.instructions[pc];
    using amber::bytecode::Opcode;
    out << "pc_" << pc << ":\n";
    switch (instruction.opcode) {
    case Opcode::LoadK: {
      std::uint32_t dst = 0;
      std::uint32_t const_id = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &const_id);
      out << "  r[" << dst
          << "] = " << native_cpp_constant_expr(module.const_pool[const_id])
          << ";\n";
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
      break;
    }
    case Opcode::LoadNull: {
      std::uint32_t dst = 0;
      operand_u32_value(instruction, 0, &dst);
      out << "  r[" << dst << "] = NativeValue::nullv();\n";
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
      break;
    }
    case Opcode::LoadBool: {
      std::uint32_t dst = 0;
      std::uint32_t value = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &value);
      out << "  r[" << dst << "] = NativeValue::boolean("
          << (value != 0U ? "true" : "false") << ");\n";
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
      break;
    }
    case Opcode::Move: {
      std::uint32_t dst = 0;
      std::uint32_t src = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &src);
      out << "  r[" << dst << "] = r[" << src << "];\n";
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
      break;
    }
    case Opcode::GetLast: {
      std::uint32_t dst = 0;
      operand_u32_value(instruction, 0, &dst);
      out << "  r[" << dst << "] = last;\n";
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
      break;
    }
    case Opcode::SetLast: {
      std::uint32_t src = 0;
      operand_u32_value(instruction, 0, &src);
      out << "  last = r[" << src << "];\n";
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
      break;
    }
    case Opcode::MakeClosure: {
      std::uint32_t dst = 0;
      std::uint32_t code_id = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &code_id);
      out << "  r[" << dst << "] = NativeValue::closure(" << code_id << ");\n";
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
      break;
    }
    case Opcode::Call: {
      std::uint32_t dst = 0;
      std::uint32_t callee = 0;
      std::uint32_t pos_count = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &callee);
      operand_u32_value(instruction, 2, &pos_count);
      out << "  r[" << dst << "] = amber_native_call_code(closure_code(r["
          << callee << "]), std::vector<NativeValue>{";
      for (std::uint32_t index = 0; index < pos_count; ++index) {
        std::uint32_t arg_reg = 0;
        operand_u32_value(instruction, 3U + index, &arg_reg);
        if (index != 0U) {
          out << ", ";
        }
        out << "r[" << arg_reg << "]";
      }
      out << "});\n";
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
      break;
    }
    case Opcode::Send: {
      std::uint32_t dst = 0;
      std::uint32_t recv = 0;
      std::uint32_t symbol_id = 0;
      std::uint32_t arg = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &recv);
      operand_u32_value(instruction, 2, &symbol_id);
      operand_u32_value(instruction, 4, &arg);
      const std::string selector = module.symbols[symbol_id];
      if (selector == "+") {
        out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << recv
            << "]) + as_int(r[" << arg << "]));\n";
      } else if (selector == "-") {
        out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << recv
            << "]) - as_int(r[" << arg << "]));\n";
      } else if (selector == "*") {
        out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << recv
            << "]) * as_int(r[" << arg << "]));\n";
      } else if (selector == "/") {
        out << "  if (as_int(r[" << arg << "]) == 0) throw NativeBailout();\n";
        out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << recv
            << "]) / as_int(r[" << arg << "]));\n";
      } else if (selector == "<") {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << recv
            << "]) < as_int(r[" << arg << "]));\n";
      } else if (selector == ">") {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << recv
            << "]) > as_int(r[" << arg << "]));\n";
      }
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
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
    case Opcode::ICmp: {
      std::uint32_t dst = 0;
      std::uint32_t lhs = 0;
      std::uint32_t rhs = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &lhs);
      operand_u32_value(instruction, 2, &rhs);
      if (instruction.opcode == Opcode::IAdd) {
        out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << lhs
            << "]) + as_int(r[" << rhs << "]));\n";
      } else if (instruction.opcode == Opcode::ISub) {
        out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << lhs
            << "]) - as_int(r[" << rhs << "]));\n";
      } else if (instruction.opcode == Opcode::IMul) {
        out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << lhs
            << "]) * as_int(r[" << rhs << "]));\n";
      } else if (instruction.opcode == Opcode::IDiv) {
        out << "  if (as_int(r[" << rhs << "]) == 0) throw NativeBailout();\n";
        out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << lhs
            << "]) / as_int(r[" << rhs << "]));\n";
      } else if (instruction.opcode == Opcode::IMod) {
        out << "  if (as_int(r[" << rhs << "]) == 0) throw NativeBailout();\n";
        out << "  r[" << dst << "] = NativeValue::integer(floor_mod_int64("
            << "as_int(r[" << lhs << "]), as_int(r[" << rhs << "])));\n";
      } else if (instruction.opcode == Opcode::IFloorDiv) {
        out << "  r[" << dst << "] = NativeValue::integer(floor_div_int64("
            << "as_int(r[" << lhs << "]), as_int(r[" << rhs << "])));\n";
      } else if (instruction.opcode == Opcode::ILe) {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) <= as_int(r[" << rhs << "]));\n";
      } else if (instruction.opcode == Opcode::IGe) {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) >= as_int(r[" << rhs << "]));\n";
      } else if (instruction.opcode == Opcode::IEq) {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) == as_int(r[" << rhs << "]));\n";
      } else if (instruction.opcode == Opcode::INe) {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) != as_int(r[" << rhs << "]));\n";
      } else if (instruction.opcode == Opcode::ICmp) {
        out << "  r[" << dst << "] = NativeValue::integer(compare_int64("
            << "as_int(r[" << lhs << "]), as_int(r[" << rhs << "])));\n";
      } else if (instruction.opcode == Opcode::ILt) {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) < as_int(r[" << rhs << "]));\n";
      } else {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) > as_int(r[" << rhs << "]));\n";
      }
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
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
    case Opcode::ICmpK: {
      std::uint32_t dst = 0;
      std::uint32_t lhs = 0;
      std::uint32_t const_id = 0;
      operand_u32_value(instruction, 0, &dst);
      operand_u32_value(instruction, 1, &lhs);
      operand_u32_value(instruction, 2, &const_id);
      const std::int64_t rhs = module.const_pool[const_id].int_value;
      if (instruction.opcode == Opcode::IAddK) {
        out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << lhs
            << "]) + " << cpp_decimal_i64(rhs) << ");\n";
      } else if (instruction.opcode == Opcode::ISubK) {
        out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << lhs
            << "]) - " << cpp_decimal_i64(rhs) << ");\n";
      } else if (instruction.opcode == Opcode::IMulK) {
        out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << lhs
            << "]) * " << cpp_decimal_i64(rhs) << ");\n";
      } else if (instruction.opcode == Opcode::IDivK) {
        if (rhs == 0) {
          out << "  throw NativeBailout();\n";
        } else {
          out << "  r[" << dst << "] = NativeValue::integer(as_int(r[" << lhs
              << "]) / " << cpp_decimal_i64(rhs) << ");\n";
        }
      } else if (instruction.opcode == Opcode::IModK) {
        if (rhs == 0) {
          out << "  throw NativeBailout();\n";
        } else {
          out << "  r[" << dst
              << "] = NativeValue::integer(floor_mod_int64(as_int(r[" << lhs
              << "]), " << cpp_decimal_i64(rhs) << "));\n";
        }
      } else if (instruction.opcode == Opcode::IFloorDivK) {
        out << "  r[" << dst
            << "] = NativeValue::integer(floor_div_int64(as_int(r[" << lhs
            << "]), " << cpp_decimal_i64(rhs) << "));\n";
      } else if (instruction.opcode == Opcode::ILeK) {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) <= " << cpp_decimal_i64(rhs) << ");\n";
      } else if (instruction.opcode == Opcode::IGeK) {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) >= " << cpp_decimal_i64(rhs) << ");\n";
      } else if (instruction.opcode == Opcode::IEqK) {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) == " << cpp_decimal_i64(rhs) << ");\n";
      } else if (instruction.opcode == Opcode::INeK) {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) != " << cpp_decimal_i64(rhs) << ");\n";
      } else if (instruction.opcode == Opcode::ICmpK) {
        out << "  r[" << dst
            << "] = NativeValue::integer(compare_int64(as_int(r[" << lhs
            << "]), " << cpp_decimal_i64(rhs) << "));\n";
      } else if (instruction.opcode == Opcode::ILtK) {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) < " << cpp_decimal_i64(rhs) << ");\n";
      } else {
        out << "  r[" << dst << "] = NativeValue::boolean(as_int(r[" << lhs
            << "]) > " << cpp_decimal_i64(rhs) << ");\n";
      }
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
      break;
    }
    case Opcode::Jump: {
      std::uint32_t target = 0;
      operand_u32_value(instruction, 0, &target);
      out << "  goto pc_" << target << ";\n";
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
        out << "  if (truthy(r[" << cond << "])) goto pc_" << target << ";\n";
      } else if (instruction.opcode == Opcode::JumpIfFalse) {
        out << "  if (!truthy(r[" << cond << "])) goto pc_" << target << ";\n";
      } else {
        out << "  if (r[" << cond << "].tag == NativeValue::Tag::Null) "
            << "goto pc_" << target << ";\n";
      }
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
      break;
    }
    case Opcode::CloseUpvalues:
    case Opcode::Safepoint:
      out << "  " << native_cpp_next(pc, code.instructions.size()) << "\n";
      break;
    case Opcode::Return: {
      std::uint32_t src = 0;
      operand_u32_value(instruction, 0, &src);
      out << "  return r[" << src << "];\n";
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

NativeCppBuildPlan
build_native_cpp_plan(const RunnableModuleArtifact &artifact,
                      const amber::bytecode::BcModule &module) {
  NativeCppBuildPlan plan;
  std::string first_reason;
  for (const amber::bytecode::BcCode &code : module.code_objects) {
    std::string reason;
    if (native_cpp_code_supported(module, code, &reason)) {
      plan.native_code_ids.insert(code.code_id);
    } else if (first_reason.empty()) {
      first_reason = "c" + std::to_string(code.code_id) + ": " + reason;
    }
  }

  const std::uint32_t init_code_id =
      module.init.has_entry_code_id ? module.init.entry_code_id : 0U;
  std::uint32_t main_code_id = 0;
  bool has_main = false;
  if (const amber::bytecode::BcMethod *main_method =
          zero_arg_method_by_name(module, "main")) {
    main_code_id = main_method->entry_code_id;
    has_main = true;
  }
  const bool init_native =
      !module.init.has_entry_code_id ||
      plan.native_code_ids.find(init_code_id) != plan.native_code_ids.end();
  const bool main_native =
      artifact.entry_mode != EntryExecutionMode::MainAfterInit ||
      (has_main &&
       plan.native_code_ids.find(main_code_id) != plan.native_code_ids.end());
  plan.entry_native = init_native && main_native;
  if (!plan.entry_native && first_reason.empty()) {
    first_reason = "entry code is not native eligible";
  }
  plan.fallback_reason = first_reason;

  std::ostringstream out;
  out << "#include \"bytecode/format.h\"\n";
  out << "#include \"runtime/vm.h\"\n\n";
  out << "#include <cstdint>\n";
  out << "#include <exception>\n";
  out << "#include <iostream>\n";
  out << "#include <optional>\n";
  out << "#include <string>\n";
  out << "#include <vector>\n\n";
  out << "namespace {\n\n";
  out << emit_embedded_hex_cpp(artifact.bytes);
  out << "struct NativeBailout : public std::exception {\n";
  out << "  const char *what() const noexcept override { return "
         "\"native bailout\"; }\n";
  out << "};\n\n";
  out << "struct NativeValue {\n";
  out << "  enum class Tag { Null, Bool, Integer, Closure };\n";
  out << "  Tag tag = Tag::Null;\n";
  out << "  bool bool_value = false;\n";
  out << "  std::int64_t int_value = 0;\n";
  out << "  std::uint32_t code_id = 0;\n";
  out << "  static NativeValue nullv() { return {}; }\n";
  out << "  static NativeValue boolean(bool value) { NativeValue out; out.tag "
         "= "
         "Tag::Bool; out.bool_value = value; return out; }\n";
  out << "  static NativeValue integer(std::int64_t value) { NativeValue out; "
         "out.tag = Tag::Integer; out.int_value = value; return out; }\n";
  out << "  static NativeValue closure(std::uint32_t value) { NativeValue out; "
         "out.tag = Tag::Closure; out.code_id = value; return out; }\n";
  out << "};\n\n";
  out << "static std::int64_t as_int(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Integer) { throw "
         "NativeBailout(); }\n";
  out << "  return value.int_value;\n";
  out << "}\n\n";
  out << "static std::uint32_t closure_code(const NativeValue &value) {\n";
  out << "  if (value.tag != NativeValue::Tag::Closure) { throw "
         "NativeBailout(); }\n";
  out << "  return value.code_id;\n";
  out << "}\n\n";
  out << "static bool truthy(const NativeValue &value) {\n";
  out << "  return value.tag != NativeValue::Tag::Null && "
         "!(value.tag == NativeValue::Tag::Bool && !value.bool_value);\n";
  out << "}\n\n";
  out << "static std::int64_t compare_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  if (lhs < rhs) return -1;\n";
  out << "  if (lhs > rhs) return 1;\n";
  out << "  return 0;\n";
  out << "}\n\n";
  out << "static std::int64_t floor_div_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  if (rhs == 0) throw NativeBailout();\n";
  out << "  std::int64_t quotient = lhs / rhs;\n";
  out << "  const std::int64_t remainder = lhs % rhs;\n";
  out << "  if (remainder != 0 && ((remainder < 0) != (rhs < 0))) "
         "--quotient;\n";
  out << "  return quotient;\n";
  out << "}\n\n";
  out << "static std::int64_t floor_mod_int64(std::int64_t lhs, "
         "std::int64_t rhs) {\n";
  out << "  return lhs - floor_div_int64(lhs, rhs) * rhs;\n";
  out << "}\n\n";
  for (std::uint32_t code_id : plan.native_code_ids) {
    out << "static NativeValue " << native_cpp_function_name(code_id)
        << "(const std::vector<NativeValue> &args);\n";
  }
  out << "\nstatic NativeValue amber_native_call_code("
         "std::uint32_t code_id, const std::vector<NativeValue> &args) {\n";
  out << "  switch (code_id) {\n";
  for (std::uint32_t code_id : plan.native_code_ids) {
    out << "  case " << code_id << ": return "
        << native_cpp_function_name(code_id) << "(args);\n";
  }
  out << "  default: throw NativeBailout();\n";
  out << "  }\n";
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
         "method.params.empty() && method.flags == 0) return &method;\n";
  out << "  }\n";
  out << "  return nullptr;\n";
  out << "}\n\n";
  out << "static void print_fault(const amber::runtime::ExecutionResult "
         "&result) {\n";
  out << "  if (!result.fault.has_value()) return;\n";
  out << "  std::cerr << result.fault->error_name << \": \" << "
         "result.fault->message << \"\\n\";\n";
  out << "  if (!result.fault->trace_text.empty()) std::cerr << "
         "result.fault->trace_text;\n";
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
  out << "  amber::runtime::RuntimeWorld world(decoded.module);\n";
  out << "  amber::runtime::ExecutionResult result;\n";
  out << "  if (decoded.module.init.has_entry_code_id) {\n";
  out << "    result = world.execute(decoded.module.init.entry_code_id);\n";
  out << "    if (!result.ok()) { print_fault(result); return 1; }\n";
  out << "  }\n";
  if (artifact.entry_mode == EntryExecutionMode::MainAfterInit) {
    out << "  const amber::bytecode::BcMethod *main_method = "
           "zero_arg_method_by_name(decoded.module, \"main\");\n";
    out << "  if (main_method == nullptr) { std::cerr << "
           "\"EntryError: entry mode requires a zero-argument main() "
           "method\\n\"; return 1; }\n";
    out << "  result = world.execute(main_method->entry_code_id);\n";
  }
  out << "  if (!result.ok()) { print_fault(result); return 1; }\n";
  out << "  if (should_print_vm_value(result.value)) std::cout << "
         "amber::runtime::value_to_debug_string(result.value, "
         "&decoded.module) << \"\\n\";\n";
  out << "  return 0;\n";
  out << "}\n\n";
  out << "static void print_native_value(const NativeValue &value) {\n";
  out << "  switch (value.tag) {\n";
  out << "  case NativeValue::Tag::Null:\n";
  out << "  case NativeValue::Tag::Closure:\n";
  out << "    return;\n";
  out << "  case NativeValue::Tag::Bool:\n";
  out << "    std::cout << (value.bool_value ? \"true\" : \"false\") << "
         "\"\\n\";\n";
  out << "    return;\n";
  out << "  case NativeValue::Tag::Integer:\n";
  out << "    std::cout << value.int_value << \"\\n\";\n";
  out << "    return;\n";
  out << "  }\n";
  out << "}\n\n";
  out << "} // namespace\n\n";
  out << "int main() {\n";
  out << "  try {\n";
  if (module.init.has_entry_code_id) {
    out << "    NativeValue init_result = amber_native_call_code("
        << init_code_id << ", std::vector<NativeValue>{});\n";
    if (artifact.entry_mode == EntryExecutionMode::Init) {
      out << "    print_native_value(init_result);\n";
    } else {
      out << "    (void)init_result;\n";
    }
  }
  if (artifact.entry_mode == EntryExecutionMode::MainAfterInit) {
    out << "    NativeValue result = amber_native_call_code(" << main_code_id
        << ", std::vector<NativeValue>{});\n";
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
    if (std::filesystem::exists(candidate / "runtime" / "vm.cpp") &&
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
      "bytecode/format.cpp",      "frontend/lexer/token.cpp",
      "profile/capabilities.cpp", "profile/effects.cpp",
      "profile/replay.cpp",       "profile/data.cpp",
      "profile/wasm_accel.cpp",   "profile/modern.cpp",
      "package/package.cpp",      "runtime/vm.cpp",
  };
  std::vector<std::string> out;
  out.reserve(relative.size());
  for (const std::string &path : relative) {
    out.push_back((root / path).string());
  }
  return out;
}

NativeExecutableBuildResult
build_native_executable(const std::string &argv0,
                        const RunnableModuleArtifact &artifact,
                        const std::filesystem::path &output_path,
                        const std::filesystem::path &native_source_path) {
  NativeCppBuildPlan plan = build_native_cpp_plan(artifact, artifact.module);
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
  std::vector<std::string> command = {
      cxx,
      "-std=c++17",
      "-O3",
      "-DNDEBUG",
      "-I",
      runtime_root.string(),
      native_source_path.string(),
  };
  const std::vector<std::string> runtime_sources =
      native_runtime_sources(runtime_root);
  command.insert(command.end(), runtime_sources.begin(), runtime_sources.end());
  command.push_back("-pthread");
  command.push_back("-o");
  command.push_back(output_path.string());

  const std::string rendered = shell_command(command);
  const int exit_code = std::system(rendered.c_str());
  if (exit_code != 0) {
    throw std::runtime_error("native C++ build failed: " + rendered);
  }

  NativeExecutableBuildResult result;
  result.output_path = output_path.string();
  result.source_path = native_source_path.string();
  result.hash = amber::lexer::sha256_hex(read_file(output_path.string()));
  result.backend = plan.backend;
  result.cxx = cxx;
  result.native_code_count = plan.native_code_ids.size();
  result.total_code_count = artifact.module.code_objects.size();
  result.entry_native = plan.entry_native;
  result.uses_bytecode_fallback = plan.uses_bytecode_fallback;
  result.fallback_reason = plan.fallback_reason;
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
  out << "  \"native_code_count\": "
      << (native == nullptr ? 0U : native->native_code_count) << ",\n";
  out << "  \"bytecode_code_count\": "
      << (native == nullptr ? 0U : native->total_code_count) << ",\n";
  out << "  \"bytecode_fallback\": "
      << (native != nullptr && native->uses_bytecode_fallback ? "true"
                                                              : "false")
      << ",\n";
  out << "  \"native_fallback_reason\": \""
      << json_escape(native == nullptr ? "" : native->fallback_reason) << "\"";
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
    const RunnableModuleArtifact artifact =
        compile_source_to_runnable_module(source_path);
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
  std::string target = "both";
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
    } else if (arg == "--target" && i + 1 < argc) {
      options.target = argv[++i];
      if (options.target != "bytecode" && options.target != "native" &&
          options.target != "both") {
        throw std::runtime_error("unknown build target: " + options.target);
      }
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
      RunnableModuleArtifact root_artifact;
      root_artifact.module_name = parsed.manifest.root_module;
      root_artifact.entry_mode = default_entry_mode_for(true, decoded.module);
      root_artifact.bytes = root_bytes;
      root_artifact.module = std::move(decoded.module);
      const std::filesystem::path native_output_path =
          out_dir / safe_artifact_name(parsed.manifest.root_module);
      const std::filesystem::path native_source_path =
          out_dir /
          (safe_artifact_name(parsed.manifest.root_module) + ".native.cpp");
      const NativeExecutableBuildResult native_result = build_native_executable(
          argv[0], root_artifact, native_output_path, native_source_path);
      summary.native_output_path = native_result.output_path;
      summary.native_backend = native_result.backend;
      summary.native_hash = native_result.hash;
      summary.native_launcher_source = native_result.source_path;
      summary.native_cxx = native_result.cxx;
      summary.native_bytecode_trampoline = native_result.uses_bytecode_fallback;
      root_record->native_output_path = native_result.output_path;
      root_record->native_hash = native_result.hash;
      root_record->native_backend = native_result.backend;
      root_record->native_eligible = native_result.entry_native;
      root_record->native_fallback_reason = native_result.fallback_reason;
      root_record->native_byte_size =
          std::filesystem::file_size(native_output_path);
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
