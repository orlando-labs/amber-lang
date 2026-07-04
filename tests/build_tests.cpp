#include "buildsys/build.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "build test failed: " << message << "\n";
    std::exit(1);
  }
}

void test_manifest_parses_and_normalizes() {
  const std::string source =
      "{\n"
      "  \"schema\": \"amber.build.v1\",\n"
      "  \"name\": \"demo\",\n"
      "  \"root\": \"demo.main\",\n"
      "  \"profiles\": {\n"
      "    \"required\": [\"effects.v1\", \"core.v1\", \"core.v1\"],\n"
      "    \"optional\": [\"typed.v1\"],\n"
      "    \"forbidden\": [\"ffi.v1\"]\n"
      "  },\n"
      "  \"stdlib\": [\n"
      "    {\"name\": \"amber.core\", \"path\": \"stdlib/core.am\"}\n"
      "  ],\n"
      "  \"modules\": [\n"
      "    {\"name\": \"demo.main\", \"path\": \"src/main.am\"},\n"
      "    {\"name\": \"demo.util\", \"path\": \"src/util.am\"}\n"
      "  ]\n"
      "}\n";
  const amber::build::BuildManifestResult parsed =
      amber::build::parse_build_manifest_json(source, "amber.build.json");
  expect(parsed.ok(), amber::build::diagnostics_to_string(parsed.diagnostics));
  expect(parsed.manifest.profiles.required_features.size() == 2,
         "required features should be unique");
  expect(parsed.manifest.profiles.required_features[0] == "core.v1",
         "required features should be sorted");
  expect(parsed.manifest.stdlib_modules.size() == 1,
         "stdlib module should parse");
  expect(parsed.manifest.stdlib_modules[0].bootstrap_layer == "B2",
         "stdlib bootstrap should default to B2");
  expect(parsed.manifest.modules[0].name == "demo.main",
         "modules should sort by name");

  const std::uint32_t flags =
      amber::build::profile_flags_for(parsed.manifest.profiles);
  expect((flags & (1U << 0U)) != 0U, "core profile flag should be set");
  expect((flags & (1U << 1U)) != 0U, "typed profile flag should be set");
  expect((flags & (1U << 3U)) != 0U, "effects profile flag should be set");
}

void test_manifest_rejects_profile_conflict() {
  const std::string source =
      "{\"schema\":\"amber.build.v1\",\"name\":\"bad\",\"root\":\"bad.main\","
      "\"profiles\":{\"required\":[\"core.v1\"],"
      "\"forbidden\":[\"core.v1\"]},"
      "\"modules\":[{\"name\":\"bad.main\",\"path\":\"main.am\"}]}";
  const amber::build::BuildManifestResult parsed =
      amber::build::parse_build_manifest_json(source, "bad.json");
  expect(!parsed.ok(), "conflicting profile manifest should fail");
  expect(parsed.diagnostics[0].message.find("both required and forbidden") !=
             std::string::npos,
         "conflict diagnostic should name profile conflict");
}

void test_runtime_feature_support_surface() {
  expect(amber::build::runtime_supports_feature("core.v1"),
         "runtime should support core.v1");
  expect(!amber::build::runtime_supports_feature("ffi.v1"),
         "runtime should not support ffi.v1 by default");
  // macro.v1 is an opt-in build profile (DESIGN-macro-system §14). Post-
  // expansion bytecode carries no macro nodes, so the loader must accept a
  // module that stamps macro.v1 in its PROF metadata.
  expect(amber::build::runtime_supports_feature("macro.v1"),
         "runtime should support macro.v1");
  const amber::build::BuildProfileSet macro_profiles = {{"macro.v1"}, {}, {}};
  expect((amber::build::profile_flags_for(macro_profiles) & (1U << 16U)) != 0U,
         "macro.v1 profile flag should be set");
}

void test_native_extensions_parse_and_gate() {
  const std::string source =
      "{\"schema\":\"amber.build.v1\",\"name\":\"crypto\","
      "\"root\":\"crypto.blake3\","
      "\"profiles\":{\"required\":[\"core.v1\",\"ffi.v1\"]},"
      "\"modules\":[{\"name\":\"crypto.blake3\",\"path\":\"src/blake3.am\"}],"
      "\"native_extensions\":[{"
      "\"name\":\"blake3\",\"language\":\"c\","
      "\"sources\":[\"native/blake3.c\"],"
      "\"capabilities\":[\"ffi\"],"
      "\"symbols\":[{\"logical\":\"blake3.hash\","
      "\"symbol\":\"amber_blake3_hash\"}],"
      "\"types\":[{\"amber\":\"crypto.blake3.Hasher\",\"tag\":\"blake3.Hasher\","
      "\"ownership\":\"owned\",\"destructor\":\"blake3.hasher_free\"}],"
      "\"errors\":[{\"name\":\"crypto.blake3.HashError\","
      "\"parent\":\"NativeError\",\"default_message\":\"hash failed\"}]"
      "}]}";
  const amber::build::BuildManifestResult parsed =
      amber::build::parse_build_manifest_json(source, "amber.build.json");
  expect(parsed.ok(), amber::build::diagnostics_to_string(parsed.diagnostics));
  expect(parsed.manifest.native_extensions.size() == 1,
         "one native extension should parse");
  const amber::pkg::PackageNativeExtension &extension =
      parsed.manifest.native_extensions[0];
  expect(extension.name == "blake3" && extension.language == "c",
         "native extension name/language");
  expect(extension.sources.size() == 1 &&
             extension.sources[0] == "native/blake3.c",
         "native extension sources");
  expect(extension.symbols.size() == 1 &&
             extension.symbols[0].symbol == "amber_blake3_hash",
         "native extension symbol map");
  expect(extension.types.size() == 1 &&
             extension.types[0].tag == "blake3.Hasher" &&
             extension.types[0].ownership == "owned",
         "native extension type tag/ownership");
  expect(extension.errors.size() == 1 &&
             extension.errors[0].name == "crypto.blake3.HashError" &&
             extension.errors[0].parent == "NativeError",
         "native extension error descriptor");

  // Without ffi.v1 the same manifest is rejected.
  const std::string ungated =
      "{\"schema\":\"amber.build.v1\",\"name\":\"crypto\","
      "\"root\":\"crypto.blake3\","
      "\"profiles\":{\"required\":[\"core.v1\"]},"
      "\"modules\":[{\"name\":\"crypto.blake3\",\"path\":\"src/blake3.am\"}],"
      "\"native_extensions\":[{\"name\":\"blake3\",\"language\":\"c\"}]}";
  const amber::build::BuildManifestResult gated =
      amber::build::parse_build_manifest_json(ungated, "amber.build.json");
  expect(!gated.ok(), "native extensions without ffi.v1 should be rejected");
  bool mentions_ffi = false;
  for (const amber::build::BuildDiagnostic &diagnostic : gated.diagnostics) {
    mentions_ffi =
        mentions_ffi || diagnostic.message.find("ffi.v1") != std::string::npos;
  }
  expect(mentions_ffi, "gating diagnostic should name the ffi.v1 requirement");
}

} // namespace

int main() {
  test_manifest_parses_and_normalizes();
  test_manifest_rejects_profile_conflict();
  test_runtime_feature_support_surface();
  test_native_extensions_parse_and_gate();
  return 0;
}
