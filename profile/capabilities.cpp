#include "profile/capabilities.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace amber::capability {

namespace {

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

const std::set<std::string> &canonical_name_set() {
  static const std::set<std::string> names = {"fs.read",
                                              "fs.write",
                                              "fs.metadata",
                                              "net.connect",
                                              "net.listen",
                                              "env.read",
                                              "env.write",
                                              "process.spawn",
                                              "process.signal",
                                              "time.now",
                                              "time.sleep",
                                              "random.secure",
                                              "random.pseudo",
                                              "ffi.call",
                                              "ffi.load",
                                              "db.connect",
                                              "secrets.read",
                                              "device.gpu",
                                              "device.accelerator",
                                              "notebook.watch",
                                              "trace.emit",
                                              "workflow.persist"};
  return names;
}

bool valid_name_char(char c) {
  const unsigned char ch = static_cast<unsigned char>(c);
  return std::isalnum(ch) != 0 || c == '.' || c == '_' || c == '-';
}

bool valid_vendor_name(const std::string &name) {
  if (name.size() < 5U || name.find('.') == std::string::npos) {
    return false;
  }
  if (name.front() == '.' || name.back() == '.') {
    return false;
  }
  std::size_t components = 1;
  for (char c : name) {
    if (!valid_name_char(c)) {
      return false;
    }
    if (c == '.') {
      ++components;
    }
  }
  return components >= 3U;
}

std::vector<CapabilityRequest>
sorted_requests(std::vector<CapabilityRequest> requests) {
  std::sort(requests.begin(), requests.end(),
            [](const CapabilityRequest &left, const CapabilityRequest &right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              if (left.target != right.target) {
                return left.target < right.target;
              }
              return left.reason < right.reason;
            });
  return requests;
}

std::string normalized_path_target(const std::string &target) {
  if (target.size() > 2U && target.substr(0, 2) == "./") {
    return target.substr(2);
  }
  return target;
}

bool target_allows(const std::string &grant_target, std::uint32_t grant_flags,
                   const std::string &requested_target) {
  if ((grant_flags & kCapabilityFlagWildcardTarget) != 0U ||
      grant_target == "*") {
    return true;
  }
  if (grant_target.empty()) {
    return requested_target.empty();
  }
  if (requested_target.empty()) {
    return false;
  }
  if (grant_target == requested_target) {
    return true;
  }
  const std::string grant = normalized_path_target(grant_target);
  const std::string requested = normalized_path_target(requested_target);
  if (grant == requested) {
    return true;
  }
  if (requested.size() > grant.size() &&
      requested.compare(0, grant.size(), grant) == 0 &&
      requested[grant.size()] == '/') {
    return true;
  }
  return false;
}

CapabilityDiagnostic diagnostic(std::string error_name, std::string message,
                                std::string capability = {},
                                std::string target = {}) {
  CapabilityDiagnostic out;
  out.error_name = std::move(error_name);
  out.message = std::move(message);
  out.capability = std::move(capability);
  out.target = std::move(target);
  return out;
}

void emit_request_array(std::ostringstream &out, const char *name,
                        const std::vector<CapabilityRequest> &requests,
                        bool trailing_comma) {
  out << "  \"" << name << "\": [";
  for (std::size_t i = 0; i < requests.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(requests[i].name)
        << "\",\"target\":\"" << json_escape(requests[i].target)
        << "\",\"reason\":\"" << json_escape(requests[i].reason)
        << "\",\"flags\":" << requests[i].flags << "}";
  }
  out << "\n  ]";
  if (trailing_comma) {
    out << ",";
  }
  out << "\n";
}

} // namespace

std::vector<std::string> canonical_capability_names() {
  return std::vector<std::string>(canonical_name_set().begin(),
                                  canonical_name_set().end());
}

std::vector<std::string>
canonical_names_for_manifest_key(const std::string &key) {
  static const std::map<std::string, std::vector<std::string>> aliases = {
      {"time", {"time.now", "time.sleep"}},
      {"random", {"random.secure", "random.pseudo"}},
      {"ffi", {"ffi.call", "ffi.load"}},
      {"gpu", {"device.gpu"}},
      {"accelerator", {"device.accelerator"}},
      {"process", {"process.spawn", "process.signal"}},
      {"secrets", {"secrets.read"}}};
  const auto alias = aliases.find(key);
  if (alias != aliases.end()) {
    return alias->second;
  }
  if (valid_capability_name(key)) {
    return {key};
  }
  return {};
}

bool valid_capability_name(const std::string &name) {
  if (canonical_name_set().find(name) != canonical_name_set().end()) {
    return true;
  }
  return valid_vendor_name(name);
}

CapabilityRequest make_capability(std::string name, std::string target,
                                  std::string reason, std::uint32_t flags) {
  CapabilityRequest request;
  request.name = std::move(name);
  request.target = std::move(target);
  request.reason = std::move(reason);
  request.flags = flags;
  return request;
}

bool capability_allows(const CapabilityRequest &grant,
                       const std::string &capability,
                       const std::string &target) {
  return grant.name == capability &&
         target_allows(grant.target, grant.flags, target);
}

bool capability_set_allows(const std::vector<CapabilityRequest> &grants,
                           const std::string &capability,
                           const std::string &target) {
  return std::any_of(grants.begin(), grants.end(),
                     [&](const CapabilityRequest &grant) {
                       return capability_allows(grant, capability, target);
                     });
}

CapabilityResolutionResult
resolve_capabilities(const std::vector<CapabilityRequest> &requested,
                     const std::vector<CapabilityRequest> &grants) {
  CapabilityResolutionResult result;
  result.requested = sorted_requests(requested);
  result.grants = sorted_requests(grants);

  for (const CapabilityRequest &grant : result.grants) {
    if (!valid_capability_name(grant.name)) {
      result.diagnostics.push_back(diagnostic(
          "CapabilityPolicyError", "invalid capability grant: " + grant.name,
          grant.name, grant.target));
    }
  }

  for (const CapabilityRequest &request : result.requested) {
    if (!valid_capability_name(request.name)) {
      result.denied.push_back(request);
      result.diagnostics.push_back(diagnostic(
          "CapabilityManifestError", "invalid capability name: " + request.name,
          request.name, request.target));
      continue;
    }
    if (capability_set_allows(result.grants, request.name, request.target)) {
      result.effective.push_back(request);
      continue;
    }
    result.denied.push_back(request);
    result.diagnostics.push_back(diagnostic("CapabilityError",
                                            "capability grant is missing for " +
                                                request_to_text(request),
                                            request.name, request.target));
  }
  result.effective = sorted_requests(result.effective);
  result.denied = sorted_requests(result.denied);
  result.ok = result.diagnostics.empty();
  return result;
}

bool parse_cli_grant(const std::string &raw, CapabilityRequest *grant,
                     CapabilityDiagnostic *diag) {
  const std::size_t equals = raw.find('=');
  const std::string name =
      equals == std::string::npos ? raw : raw.substr(0, equals);
  const std::string target =
      equals == std::string::npos ? "*" : raw.substr(equals + 1U);
  if (!valid_capability_name(name)) {
    if (diag != nullptr) {
      *diag = diagnostic("CapabilityPolicyError",
                         "invalid capability grant: " + raw, name, target);
    }
    return false;
  }
  if (grant != nullptr) {
    *grant =
        make_capability(name, target, "host policy",
                        target == "*" ? kCapabilityFlagWildcardTarget : 0U);
  }
  return true;
}

std::string request_to_text(const CapabilityRequest &request) {
  std::ostringstream out;
  out << request.name;
  if (!request.target.empty()) {
    out << "=" << request.target;
  }
  return out.str();
}

std::string resolution_to_json(const CapabilityResolutionResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.capabilities.v1\",\n";
  out << "  \"status\": \"" << (result.ok ? "ok" : "error") << "\",\n";
  emit_request_array(out, "requested", result.requested, true);
  emit_request_array(out, "grants", result.grants, true);
  emit_request_array(out, "effective", result.effective, true);
  emit_request_array(out, "denied", result.denied, true);
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error_name\":\""
        << json_escape(result.diagnostics[i].error_name) << "\",\"message\":\""
        << json_escape(result.diagnostics[i].message) << "\",\"capability\":\""
        << json_escape(result.diagnostics[i].capability) << "\",\"target\":\""
        << json_escape(result.diagnostics[i].target) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::capability
