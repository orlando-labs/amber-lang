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

int main() {
  test_manifest_and_lock_are_deterministic();
  test_capability_manifest_and_policy_resolution();
  test_package_artifact_is_reproducible_and_signed();
  test_registry_publish_and_install_smoke();
  return 0;
}
