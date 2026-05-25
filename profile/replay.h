#pragma once

#include "profile/capabilities.h"

#include <cstdint>
#include <string>
#include <vector>

namespace amber::replay {

inline constexpr std::uint32_t kObservabilitySiteFlagRequired = 0x1U;
inline constexpr std::uint32_t kReplayMetadataFlagDeterministic = 0x1U;

struct TraceAttribute {
  std::string key;
  std::string value;
};

struct SourceLocation {
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

struct ObservabilitySite {
  std::uint32_t site_id = 0;
  std::string event_name;
  std::string kind;
  std::string owner;
  SourceLocation source;
  std::uint32_t flags = 0;
};

struct ReplayMetadata {
  std::vector<std::string> required_event_names;
  std::vector<std::string> deterministic_sources;
  std::uint32_t flags = 0;
};

struct TraceEvent {
  std::uint64_t event_id = 0;
  std::string name;
  std::uint64_t timestamp_or_virtual_time = 0;
  std::string trace_id;
  std::string span_id;
  std::string parent_span_id;
  std::string task_id;
  std::string strand_id;
  std::string worker_id;
  std::string module_id;
  std::string method_id;
  SourceLocation source;
  std::uint64_t world_epoch = 0;
  std::uint64_t watch_epoch = 0;
  std::vector<TraceAttribute> attributes;
  std::string severity;
  std::vector<std::uint64_t> causality_edges;
  std::uint32_t flags = 0;
};

struct ReplayTrace {
  std::string schema = "amber.replay.v1";
  std::string package_lock_digest;
  std::vector<std::string> artifact_digests;
  std::vector<capability::CapabilityRequest> capability_grants;
  std::vector<std::string> schema_versions;
  std::vector<TraceEvent> events;
};

struct ReplayDiagnostic {
  std::string error_name;
  std::string message;
  std::uint64_t expected_event_id = 0;
  std::uint64_t actual_event_id = 0;
  std::string event_name;
};

struct ReplayValidationResult {
  bool ok = false;
  std::size_t consumed_events = 0;
  std::vector<ReplayDiagnostic> diagnostics;
};

struct ReplayTraceParseResult {
  ReplayTrace trace;
  std::vector<ReplayDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

std::vector<std::string> canonical_event_names();
std::vector<std::string> canonical_deterministic_sources();
bool valid_event_name(const std::string &name);
bool valid_deterministic_source(const std::string &name);

TraceEvent make_event(std::string name,
                      std::vector<TraceAttribute> attributes = {});
TraceEvent normalize_event(TraceEvent event);
ObservabilitySite normalize_site(ObservabilitySite site);
ReplayMetadata normalize_metadata(ReplayMetadata metadata);
ReplayTrace normalize_trace(ReplayTrace trace);

ReplayValidationResult
validate_metadata(const ReplayMetadata &metadata,
                  const std::vector<ObservabilitySite> &sites = {});
ReplayValidationResult validate_trace(const ReplayTrace &trace);
ReplayValidationResult compare_traces(const ReplayTrace &expected,
                                      const ReplayTrace &actual);

std::string event_signature(const TraceEvent &event);
std::string trace_digest(const ReplayTrace &trace);
std::string serialize_trace(const ReplayTrace &trace);
ReplayTraceParseResult parse_trace(const std::string &serialized);

std::string trace_to_json(const ReplayTrace &trace);
std::string validation_to_json(const ReplayValidationResult &result);

} // namespace amber::replay
