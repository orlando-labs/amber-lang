#include "package/package.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "package test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::pkg::PackageManifest sample_manifest() {
  const std::string manifest_source = "[package]\n"
                                      "name = \"demo.pkg\"\n"
                                      "version = \"0.1.0\"\n"
                                      "root = \"demo.core\"\n"
                                      "\n"
                                      "[[modules]]\n"
                                      "name = \"demo.extra\"\n"
                                      "path = \"src/extra.am\"\n"
                                      "\n"
                                      "[[modules]]\n"
                                      "name = \"demo.core\"\n"
                                      "path = \"src/core.am\"\n"
                                      "\n"
                                      "[dependencies]\n"
                                      "zeta.lib = \"2.0.0\"\n"
                                      "alpha.lib = \"1.0.0\"\n";

  amber::pkg::PackageManifestResult parsed =
      amber::pkg::parse_manifest_toml(manifest_source, "amber.toml");
  expect(parsed.ok(), "sample manifest should parse");
  return parsed.manifest;
}

std::vector<amber::pkg::PackageModuleBlob> sample_modules_reversed() {
  amber::pkg::PackageModuleBlob extra;
  extra.name = "demo.extra";
  extra.path = "src/extra.am";
  extra.bytes = {0xAA, 0xBB, 0xCC};

  amber::pkg::PackageModuleBlob core;
  core.name = "demo.core";
  core.path = "src/core.am";
  core.bytes = {0x01, 0x02, 0x03, 0x04};
  return {extra, core};
}

std::vector<amber::pkg::PackageModuleBlob> sample_modules_sorted() {
  std::vector<amber::pkg::PackageModuleBlob> modules =
      sample_modules_reversed();
  std::swap(modules[0], modules[1]);
  return modules;
}

void test_manifest_and_lock_are_deterministic() {
  const amber::pkg::PackageManifest manifest = sample_manifest();
  expect(manifest.modules.size() == 2, "manifest should expose two modules");
  expect(manifest.modules[0].name == "demo.core",
         "manifest modules should be sorted by module name");
  expect(manifest.dependencies.size() == 2,
         "manifest should expose two dependencies");
  expect(manifest.dependencies[0].name == "alpha.lib",
         "dependencies should be sorted by name");

  const std::string lockfile = amber::pkg::render_lockfile(manifest);
  const std::size_t alpha = lockfile.find("alpha.lib");
  const std::size_t zeta = lockfile.find("zeta.lib");
  expect(alpha != std::string::npos && zeta != std::string::npos &&
             alpha < zeta,
         "lockfile dependencies should be deterministic");
  expect(lockfile.find("checksum = \"sha256:") != std::string::npos,
         "lockfile should include dependency checksums");
}

void test_capability_manifest_and_policy_resolution() {
  const std::string manifest_source = "[package]\n"
                                      "name = \"caps.pkg\"\n"
                                      "version = \"0.1.0\"\n"
                                      "root = \"caps.core\"\n"
                                      "\n"
                                      "[[modules]]\n"
                                      "name = \"caps.core\"\n"
                                      "path = \"src/core.am\"\n"
                                      "\n"
                                      "[capabilities]\n"
                                      "fs.read = [\"./data\"]\n"
                                      "net.connect = [\"api.example:443\"]\n"
                                      "time = true\n"
                                      "ffi = false\n";
  const amber::pkg::PackageManifestResult parsed =
      amber::pkg::parse_manifest_toml(manifest_source, "amber.toml");
  expect(parsed.ok(), "capability manifest should parse");
  expect(parsed.manifest.capabilities.size() == 4,
         "capability manifest should canonicalize aliases");

  std::vector<amber::capability::CapabilityRequest> grants;
  grants.push_back(amber::capability::make_capability("fs.read", "./data"));
  grants.push_back(
      amber::capability::make_capability("net.connect", "api.example:443"));
  grants.push_back(amber::capability::make_capability(
      "time.now", "*", "host policy",
      amber::capability::kCapabilityFlagWildcardTarget));
  grants.push_back(amber::capability::make_capability(
      "time.sleep", "*", "host policy",
      amber::capability::kCapabilityFlagWildcardTarget));
  const amber::capability::CapabilityResolutionResult allowed =
      amber::capability::resolve_capabilities(parsed.manifest.capabilities,
                                              grants);
  expect(allowed.ok, "matching grants should satisfy requested capabilities");
  expect(amber::capability::capability_set_allows(allowed.effective, "fs.read",
                                                  "./data/orders.csv"),
         "fs.read grant should allow paths under target");
  expect(!amber::capability::capability_set_allows(allowed.effective, "fs.read",
                                                   "./private/orders.csv"),
         "fs.read grant should deny paths outside target");

  const amber::capability::CapabilityResolutionResult denied =
      amber::capability::resolve_capabilities(parsed.manifest.capabilities, {});
  expect(!denied.ok, "deny-by-default policy should reject missing grants");
  expect(denied.denied.size() == parsed.manifest.capabilities.size(),
         "all requested capabilities should be denied without grants");
}

void test_package_artifact_is_reproducible_and_signed() {
  const amber::pkg::PackageManifest manifest = sample_manifest();
  amber::pkg::PackageBuildOptions options;
  options.key_id = "ci";
  options.signing_key = "secret";

  const amber::pkg::PackageBuildResult first =
      amber::pkg::build_package_artifact(manifest, sample_modules_reversed(),
                                         options);
  const amber::pkg::PackageBuildResult second =
      amber::pkg::build_package_artifact(manifest, sample_modules_sorted(),
                                         options);
  expect(first.ok, "first package build should succeed");
  expect(second.ok, "second package build should succeed");
  expect(first.serialized == second.serialized,
         "package artifact should be reproducible independent of input order");

  const amber::pkg::PackageVerifyResult verified =
      amber::pkg::verify_package_artifact(first.serialized, "secret");
  expect(verified.ok, "signed package should verify with the right key");
  expect(verified.signature_present, "package signature should be present");
  expect(verified.signature_checked, "package signature should be checked");
  expect(verified.signature_valid, "package signature should be valid");

  const amber::pkg::PackageVerifyResult wrong_key =
      amber::pkg::verify_package_artifact(first.serialized, "wrong");
  expect(!wrong_key.ok, "signed package should reject the wrong key");
}

void test_registry_publish_and_install_smoke() {
  const amber::pkg::PackageManifest manifest = sample_manifest();
  amber::pkg::PackageBuildOptions options;
  options.key_id = "ci";
  options.signing_key = "secret";
  const amber::pkg::PackageBuildResult built =
      amber::pkg::build_package_artifact(manifest, sample_modules_reversed(),
                                         options);
  expect(built.ok, "package build should succeed for registry smoke");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("amber_pkg_tests_" + std::to_string(getpid()));
  std::error_code error;
  std::filesystem::remove_all(root, error);

  const amber::pkg::PackageRegistryResult published =
      amber::pkg::publish_package_artifact(built.serialized, root.string(),
                                           "secret");
  expect(published.ok, "first publish should succeed");
  expect(std::filesystem::exists(published.installed_path),
         "publish should write the package artifact");

  const amber::pkg::PackageRegistryResult duplicate =
      amber::pkg::publish_package_artifact(built.serialized, root.string(),
                                           "secret");
  expect(!duplicate.ok, "duplicate publish should fail");

  const amber::pkg::PackageRegistryResult installed =
      amber::pkg::install_package_artifact(built.serialized, root.string(),
                                           "secret");
  expect(installed.ok, "install should accept the already cached artifact");

  std::filesystem::remove_all(root, error);
}

} // namespace

void test_native_extension_manifest() {
  const std::string source =
      "[package]\n"
      "name = \"crypto.blake3\"\n"
      "version = \"0.1.0\"\n"
      "root = \"crypto.blake3\"\n"
      "\n"
      "[[modules]]\n"
      "name = \"crypto.blake3\"\n"
      "path = \"src/blake3.am\"\n"
      "\n"
      "[[native]]\n"
      "name = \"blake3\"\n"
      "language = \"c\"\n"
      "sources = [\"native/blake3.c\", \"native/amber_blake3.c\"]\n"
      "include_dirs = [\"native/include\"]\n"
      "cxxflags = [\"-O3\"]\n"
      "\n"
      "[native.symbols]\n"
      "\"blake3.hash\" = \"amber_blake3_hash\"\n"
      "\"blake3.hasher_free\" = \"amber_blake3_hasher_free\"\n"
      "\n"
      "[[native.types]]\n"
      "amber = \"crypto.blake3.Hasher\"\n"
      "tag = \"blake3.Hasher\"\n"
      "ownership = \"owned\"\n"
      "destructor = \"blake3.hasher_free\"\n"
      "\n"
      "[[native.errors]]\n"
      "name = \"crypto.blake3.HashError\"\n"
      "parent = \"NativeError\"\n"
      "default_message = \"hash failed\"\n";

  amber::pkg::PackageManifestResult parsed =
      amber::pkg::parse_manifest_toml(source, "amber.toml");
  expect(parsed.ok(), "native manifest should parse");
  expect(parsed.manifest.native_extensions.size() == 1,
         "one native extension parsed");
  const amber::pkg::PackageNativeExtension &ext =
      parsed.manifest.native_extensions[0];
  expect(ext.name == "blake3" && ext.language == "c", "native ext name/lang");
  expect(ext.sources.size() == 2 && ext.sources[0] == "native/blake3.c",
         "native sources string array");
  expect(ext.include_dirs.size() == 1 && ext.cxxflags.size() == 1 &&
             ext.cxxflags[0] == "-O3",
         "native include_dirs/cxxflags arrays");
  expect(ext.symbols.size() == 2 && ext.symbols[0].logical == "blake3.hash" &&
             ext.symbols[0].symbol == "amber_blake3_hash",
         "symbol map unquotes the logical key");
  expect(ext.types.size() == 1 && ext.types[0].tag == "blake3.Hasher" &&
             ext.types[0].ownership == "owned" &&
             ext.types[0].destructor == "blake3.hasher_free",
         "native type fields");
  expect(ext.errors.size() == 1 &&
             ext.errors[0].name == "crypto.blake3.HashError" &&
             ext.errors[0].parent == "NativeError" &&
             ext.errors[0].default_message == "hash failed",
         "native error fields");

  const std::string json = amber::pkg::manifest_to_json(parsed.manifest);
  expect(json.find("\"native_extensions\"") != std::string::npos &&
             json.find("amber_blake3_hash") != std::string::npos &&
             json.find("\"tag\":\"blake3.Hasher\"") != std::string::npos &&
             json.find("\"name\":\"crypto.blake3.HashError\"") !=
                 std::string::npos,
         "manifest json includes native sections");

  // Native content folds deterministically into the package digest.
  amber::pkg::PackageModuleBlob blob;
  blob.name = "crypto.blake3";
  blob.path = "src/blake3.am";
  blob.bytes = {0x01, 0x02, 0x03};
  amber::pkg::PackageBuildOptions native_options;
  native_options.target_triple = "test-triple";
  native_options.native_blobs = {
      {"blake3", "source", "native/blake3.c", {}, {0x63, 0x31}},
      {"blake3", "source", "native/amber_blake3.c", {}, {0x63, 0x32}},
  };
  const amber::pkg::PackageBuildResult missing_blobs =
      amber::pkg::build_package_artifact(parsed.manifest, {blob});
  expect(!missing_blobs.ok, "declared native sources require package blobs");
  const amber::pkg::PackageBuildResult first =
      amber::pkg::build_package_artifact(parsed.manifest, {blob},
                                         native_options);
  const amber::pkg::PackageBuildResult second =
      amber::pkg::build_package_artifact(parsed.manifest, {blob},
                                         native_options);
  expect(first.ok && second.ok, "native package builds");
  expect(first.artifact.manifest_digest == second.artifact.manifest_digest,
         "native manifest digest is deterministic");
  expect(first.artifact.native_blobs.size() == 2,
         "native source blobs are stored in the package");
  expect(first.artifact.native_extensions.size() == 1 &&
             first.artifact.native_extensions[0].target_triple ==
                 "test-triple",
         "native extension metadata records target triple");
  const amber::pkg::PackageVerifyResult verified =
      amber::pkg::verify_package_artifact(first.serialized);
  expect(verified.ok, "native package verifies");
  const std::string inspect = amber::pkg::artifact_to_json(first.artifact);
  expect(inspect.find("\"native_blobs\"") != std::string::npos &&
             inspect.find("\"native_source_sha256\"") != std::string::npos,
         "package json exposes native blobs and source digest");
  std::string tampered = first.serialized;
  const std::size_t native_bytes = tampered.find("bytes=6331");
  expect(native_bytes != std::string::npos,
         "serialized package should contain native source bytes");
  tampered.replace(native_bytes + std::string("bytes=").size(), 4, "6330");
  const amber::pkg::PackageVerifyResult tampered_verify =
      amber::pkg::verify_package_artifact(tampered);
  expect(!tampered_verify.ok,
         "native blob digest mismatch should be rejected");

  const std::string base =
      "[package]\nname = \"p\"\nversion = \"0.1.0\"\nroot = \"p\"\n"
      "[[modules]]\nname = \"p\"\npath = \"p.am\"\n";
  amber::pkg::PackageManifestResult bad_owner = amber::pkg::parse_manifest_toml(
      base + "[[native]]\nname = \"x\"\n[[native.types]]\namber = \"P.T\"\n"
             "tag = \"x.T\"\nownership = \"weird\"\n",
      "amber.toml");
  expect(!bad_owner.ok(), "invalid native ownership is rejected");
  amber::pkg::PackageManifestResult no_dtor = amber::pkg::parse_manifest_toml(
      base + "[[native]]\nname = \"x\"\n[[native.types]]\namber = \"P.T\"\n"
             "tag = \"x.T\"\nownership = \"owned\"\n",
      "amber.toml");
  expect(!no_dtor.ok(), "owned native type without destructor is rejected");
  amber::pkg::PackageManifestResult orphan = amber::pkg::parse_manifest_toml(
      base + "[native.symbols]\n\"a\" = \"b\"\n", "amber.toml");
  expect(!orphan.ok(), "native.symbols before [[native]] is rejected");
  amber::pkg::PackageManifestResult native_caps =
      amber::pkg::parse_manifest_toml(
          base + "[[native]]\nname = \"x\"\ncapabilities = [\"ffi\"]\n",
          "amber.toml");
  expect(!native_caps.ok(), "native extension capabilities are rejected");
}

int main() {
  test_native_extension_manifest();
  test_manifest_and_lock_are_deterministic();
  test_capability_manifest_and_policy_resolution();
  test_package_artifact_is_reproducible_and_signed();
  test_registry_publish_and_install_smoke();
  return 0;
}
