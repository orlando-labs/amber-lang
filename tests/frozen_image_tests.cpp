#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "frozen/image.h"
#include "optimizer/mir.h"
#include "optimizer/native.h"
#include "package/package.h"
#include "runtime/frozen_image.h"
#include "runtime/native_bridge.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct ModuleArtifacts {
  amber::pkg::PackageModuleBlob blob;
  amber::bytecode::BcModule bytecode_module;
  amber::native::NativeModule native_module;
};

struct ImageFixture {
  amber::pkg::PackageBuildResult package;
  amber::frozen::FrozenImageBuildResult image;
  amber::bytecode::BcModule root_module;
};

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "frozen image test failed: " << message << "\n";
    std::exit(1);
  }
}

ModuleArtifacts compile_module(const std::string &module_name,
                               const std::string &path,
                               const std::string &source) {
  amber::lexer::Lexer lexer(source, path);
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
  expect(parse_result.module_name == module_name,
         "compiled source module name should match fixture manifest");

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    std::exit(1);
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::mir::Module mir_module =
      amber::mir::lower_program(program, parse_result.module_name);
  amber::mir::ValidationResult mir_validation =
      amber::mir::validate_module(mir_module);
  if (!mir_validation.ok()) {
    std::cerr << amber::mir::validation_errors_to_json(mir_validation.errors);
    std::exit(1);
  }

  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(emit_result.diagnostics);
    std::exit(1);
  }
  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(emit_result.module);
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(bytes);
  if (!decoded.ok()) {
    std::cerr << amber::bytecode::verify_errors_to_json(decoded.errors);
    std::exit(1);
  }

  amber::native::NativeModule native_module =
      amber::native::compile_native_module(decoded.module, mir_module);
  const amber::native::NativeValidationResult native_validation =
      amber::native::validate_native_module(native_module, &decoded.module);
  if (!native_validation.ok()) {
    std::cerr << amber::native::diagnostics_to_json(
        native_validation.diagnostics);
    std::exit(1);
  }

  amber::pkg::PackageModuleBlob blob;
  blob.name = module_name;
  blob.path = path;
  blob.bytes = bytes;
  return {std::move(blob), decoded.module, std::move(native_module)};
}

amber::pkg::PackageManifest fixture_manifest() {
  const std::string manifest_source = "[package]\n"
                                      "name = \"frozen.demo\"\n"
                                      "version = \"1.0.0\"\n"
                                      "root = \"frozen.core\"\n"
                                      "\n"
                                      "[[modules]]\n"
                                      "name = \"frozen.extra\"\n"
                                      "path = \"src/extra.am\"\n"
                                      "\n"
                                      "[[modules]]\n"
                                      "name = \"frozen.core\"\n"
                                      "path = \"src/core.am\"\n"
                                      "\n"
                                      "[[native]]\n"
                                      "name = \"frozen_ext\"\n"
                                      "language = \"c\"\n"
                                      "sources = [\"native/ext.c\"]\n"
                                      "headers = [\"native/ext.h\"]\n"
                                      "\n"
                                      "[native.symbols]\n"
                                      "\"frozen.native\" = \"amber_frozen_native\"\n"
                                      "\n"
                                      "[[native.types]]\n"
                                      "amber = \"frozen.NativeBox\"\n"
                                      "tag = \"frozen.NativeBox\"\n"
                                      "ownership = \"borrowed\"\n"
                                      "\n"
                                      "[[native.errors]]\n"
                                      "name = \"frozen.NativeError\"\n"
                                      "parent = \"NativeError\"\n"
                                      "default_message = \"native failed\"\n";
  amber::pkg::PackageManifestResult parsed =
      amber::pkg::parse_manifest_toml(manifest_source, "amber.toml");
  expect(parsed.ok(), "fixture manifest should parse");
  return parsed.manifest;
}

ImageFixture build_fixture(bool reverse_inputs = false) {
  ModuleArtifacts core = compile_module("frozen.core", "src/core.am",
                                        "package frozen.core\n"
                                        "export answer\n"
                                        "\n"
                                        "def answer():\n"
                                        "  40 + 2\n");
  ModuleArtifacts extra = compile_module("frozen.extra", "src/extra.am",
                                         "package frozen.extra\n"
                                         "export id\n"
                                         "\n"
                                         "def id(x):\n"
                                         "  x\n");

  std::vector<amber::pkg::PackageModuleBlob> blobs = {core.blob, extra.blob};
  std::vector<amber::native::NativeModule> native_modules = {
      core.native_module, extra.native_module};
  if (reverse_inputs) {
    std::swap(blobs[0], blobs[1]);
    std::swap(native_modules[0], native_modules[1]);
  }

  amber::pkg::PackageBuildOptions options;
  options.key_id = "ci";
  options.signing_key = "secret";
  options.target_triple = "test-triple";
  options.native_blobs = {
      {"frozen_ext", "source", "native/ext.c", {}, {0x63, 0x31}},
      {"frozen_ext", "header", "native/ext.h", {}, {0x68, 0x31}},
  };
  amber::pkg::PackageBuildResult package =
      amber::pkg::build_package_artifact(fixture_manifest(), blobs, options);
  expect(package.ok, "package artifact should build");

  amber::frozen::FrozenImageBuildResult image =
      amber::frozen::build_frozen_image_artifact(
          package.artifact, package.serialized, native_modules);
  expect(image.ok, "frozen image should build");
  return {std::move(package), std::move(image), core.bytecode_module};
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

const amber::native::NativeCodeObject *
native_code_for_bc(const amber::native::NativeModule &module,
                   std::uint32_t code_id) {
  for (const amber::native::NativeCodeObject &code : module.code_objects) {
    if (code.source_bc_code_id == code_id) {
      return &code;
    }
  }
  return nullptr;
}

std::string replace_first(std::string value, const std::string &from,
                          const std::string &to) {
  const std::size_t found = value.find(from);
  expect(found != std::string::npos, "replacement target should exist");
  value.replace(found, from.size(), to);
  return value;
}

std::string replace_all(std::string value, const std::string &from,
                        const std::string &to) {
  std::size_t found = value.find(from);
  expect(found != std::string::npos, "replacement target should exist");
  while (found != std::string::npos) {
    value.replace(found, from.size(), to);
    found = value.find(from, found + to.size());
  }
  return value;
}

std::string hex_text(const std::string &value) {
  std::string out;
  const char *hex = "0123456789abcdef";
  for (const unsigned char ch : value) {
    out.push_back(hex[(ch >> 4U) & 0x0FU]);
    out.push_back(hex[ch & 0x0FU]);
  }
  return out;
}

void test_frozen_image_build_is_reproducible_and_verifies() {
  const ImageFixture first = build_fixture(false);
  const ImageFixture second = build_fixture(true);
  expect(first.image.serialized == second.image.serialized,
         "frozen image artifact should be reproducible independent of input "
         "order");

  const amber::frozen::FrozenImageVerifyResult verified =
      amber::frozen::verify_frozen_image_artifact(first.image.serialized,
                                                  "secret");
  expect(verified.ok, "signed frozen image should verify with package key");
  expect(verified.loadable, "verified frozen image should be loadable");

  const std::string inspect =
      amber::frozen::artifact_to_json(first.image.artifact);
  expect(inspect.find("amber.image.inspect.v1") != std::string::npos,
         "inspect JSON should identify frozen image schema");
  expect(inspect.find("native_modules") != std::string::npos,
         "inspect JSON should include native metadata summaries");
}

void test_frozen_image_load_freezes_world_and_executes_bound_native() {
  const ImageFixture fixture = build_fixture(false);
  amber::runtime::RuntimeFrozenImageLoadResult loaded =
      amber::runtime::load_frozen_image(fixture.image.artifact);
  expect(loaded.ok, "frozen image should load into runtime");
  expect(loaded.world != nullptr, "loaded image should expose runtime world");
  expect(loaded.world->is_world_frozen(),
         "loaded image should install frozen world barrier");
  expect(!loaded.bound_native_modules.empty(),
         "runtime loader should bind native modules when metadata is present");

  const amber::bytecode::BcMethod *method =
      method_by_name(fixture.root_module, "answer");
  expect(method != nullptr, "answer method should exist");

  const amber::native::NativeModule *root_native = nullptr;
  for (const amber::native::NativeModule &module :
       loaded.bound_native_modules) {
    if (module.module_name == "frozen.core") {
      root_native = &module;
      break;
    }
  }
  expect(root_native != nullptr, "root native module should be bound");
  const amber::native::NativeCodeObject *code =
      native_code_for_bc(*root_native, method->entry_code_id);
  expect(code != nullptr, "answer native code object should be present");

  const amber::runtime::ExecutionResult result =
      amber::runtime::execute_native_code(*loaded.world, *root_native,
                                          code->native_id);
  expect(result.ok(), "bound native trampoline should execute");
  expect(result.value.is_integer() && result.value.as_integer() == 42,
         "bound native trampoline should preserve bytecode result");

  const amber::runtime::RuntimePackageReloadResult reload =
      loaded.world->reload_package_artifact(fixture.package.artifact);
  expect(!reload.ok, "package reload should be rejected after image load");
  expect(!reload.diagnostics.empty() &&
             reload.diagnostics[0].error_name == "WorldFrozenError",
         "reload rejection should use the frozen-world barrier");
}

void test_frozen_image_verify_rejects_non_frozen_native_summary() {
  const ImageFixture fixture = build_fixture(false);
  const std::string tampered = replace_first(
      fixture.image.serialized, "native.requires_frozen_world=true",
      "native.requires_frozen_world=false");
  const amber::frozen::FrozenImageVerifyResult verified =
      amber::frozen::verify_frozen_image_artifact(tampered, "secret");
  expect(!verified.ok,
         "frozen image verify should reject non-frozen native summaries");
  bool saw_error = false;
  for (const amber::frozen::FrozenImageDiagnostic &diagnostic :
       verified.diagnostics) {
    if (diagnostic.message.find("does not require frozen world") !=
        std::string::npos) {
      saw_error = true;
    }
  }
  expect(saw_error, "verify diagnostics should explain frozen guard failure");
}

void test_frozen_image_verify_rejects_missing_native_readiness_guard() {
  const ImageFixture fixture = build_fixture(false);
  const std::string tampered =
      replace_all(fixture.image.serialized, hex_text("slowpath_table"),
                  hex_text("slowpath-table"));
  const amber::frozen::FrozenImageVerifyResult verified =
      amber::frozen::verify_frozen_image_artifact(tampered, "secret");
  expect(!verified.ok,
         "frozen image verify should reject native metadata missing W15 "
         "readiness fields");
  bool saw_error = false;
  for (const amber::frozen::FrozenImageDiagnostic &diagnostic :
       verified.diagnostics) {
    if (diagnostic.message.find("readiness guards") != std::string::npos) {
      saw_error = true;
    }
  }
  expect(saw_error,
         "verify diagnostics should explain readiness guard failure");
}

void test_frozen_image_verify_rejects_native_extension_mismatches() {
  const ImageFixture fixture = build_fixture(false);
  const auto rejects = [](const std::string &serialized,
                          const std::string &message) {
    const amber::frozen::FrozenImageVerifyResult verified =
        amber::frozen::verify_frozen_image_artifact(serialized, "secret");
    expect(!verified.ok, message);
  };

  rejects(replace_first(fixture.image.serialized,
                        "native_extension.0.amber_ext_abi_version=1",
                        "native_extension.0.amber_ext_abi_version=2"),
          "frozen image verify should reject changed amber_ext ABI version");
  rejects(replace_first(fixture.image.serialized,
                        "native_extension.0.native_source_sha256=sha256:",
                        "native_extension.0.native_source_sha256=sha256:0"),
          "frozen image verify should reject changed native source digest");
  rejects(replace_first(fixture.image.serialized,
                        "native_extension.0.exported_symbol_sha256=sha256:",
                        "native_extension.0.exported_symbol_sha256=sha256:0"),
          "frozen image verify should reject changed exported-symbol digest");
  rejects(replace_first(fixture.image.serialized,
                        "native_extension.0.type.0.ownership=borrowed",
                        "native_extension.0.type.0.ownership=owned"),
          "frozen image verify should reject changed ownership metadata");
  rejects(replace_first(fixture.image.serialized,
                        "native_extension.0.error.0.default_message=native "
                        "failed",
                        "native_extension.0.error.0.default_message=changed"),
          "frozen image verify should reject changed native error metadata");
}

} // namespace

int main() {
  test_frozen_image_build_is_reproducible_and_verifies();
  test_frozen_image_load_freezes_world_and_executes_bound_native();
  test_frozen_image_verify_rejects_non_frozen_native_summary();
  test_frozen_image_verify_rejects_missing_native_readiness_guard();
  test_frozen_image_verify_rejects_native_extension_mismatches();
  return 0;
}
