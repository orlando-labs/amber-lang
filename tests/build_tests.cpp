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
}

} // namespace

int main() {
  test_manifest_parses_and_normalizes();
  test_manifest_rejects_profile_conflict();
  test_runtime_feature_support_surface();
  return 0;
}
