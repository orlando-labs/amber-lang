#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber::capability {

inline constexpr std::uint32_t kCapabilityFlagWildcardTarget = 0x1U;

struct CapabilityRequest {
  std::string name;
  std::string target;
  std::string reason;
  std::uint32_t flags = 0;
};

struct CapabilityDiagnostic {
  std::string error_name;
  std::string message;
  std::string capability;
  std::string target;
};

struct CapabilityResolutionResult {
  bool ok = false;
  std::vector<CapabilityRequest> requested;
  std::vector<CapabilityRequest> grants;
  std::vector<CapabilityRequest> effective;
  std::vector<CapabilityRequest> denied;
  std::vector<CapabilityDiagnostic> diagnostics;
};

std::vector<std::string> canonical_capability_names();
std::vector<std::string>
canonical_names_for_manifest_key(const std::string &key);
bool valid_capability_name(const std::string &name);
CapabilityRequest make_capability(std::string name, std::string target = {},
                                  std::string reason = {},
                                  std::uint32_t flags = 0);

bool capability_allows(const CapabilityRequest &grant,
                       const std::string &capability,
                       const std::string &target = {});
bool capability_set_allows(const std::vector<CapabilityRequest> &grants,
                           const std::string &capability,
                           const std::string &target = {});
CapabilityResolutionResult
resolve_capabilities(const std::vector<CapabilityRequest> &requested,
                     const std::vector<CapabilityRequest> &grants);

bool parse_cli_grant(const std::string &raw, CapabilityRequest *grant,
                     CapabilityDiagnostic *diagnostic = nullptr);
std::string request_to_text(const CapabilityRequest &request);
std::string resolution_to_json(const CapabilityResolutionResult &result);

} // namespace amber::capability
