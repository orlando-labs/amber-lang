#include "profile/replay.h"

#include "frontend/lexer/token.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace amber::replay {

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

std::string line_escape(const std::string &value) {
  std::ostringstream out;
  for (const char c : value) {
    switch (c) {
    case '\\':
      out << "\\\\";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '=':
      out << "\\=";
      break;
    default:
      out << c;
      break;
    }
  }
  return out.str();
}

std::string line_unescape(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  bool escaping = false;
  for (const char c : value) {
    if (escaping) {
      if (c == 'n') {
        out.push_back('\n');
      } else if (c == 'r') {
        out.push_back('\r');
      } else {
        out.push_back(c);
      }
      escaping = false;
      continue;
    }
    if (c == '\\') {
      escaping = true;
      continue;
    }
    out.push_back(c);
  }
  if (escaping) {
    out.push_back('\\');
  }
  return out;
}

const std::set<std::string> &canonical_event_set() {
  static const std::set<std::string> names = {"atomic.cas",
                                              "capability.check",
                                              "capability.denied",
                                              "channel.close",
                                              "channel.recv",
                                              "channel.send",
                                              "effect.boundary",
                                              "ffi.enter",
                                              "ffi.exit",
                                              "gc.cycle.end",
                                              "gc.cycle.start",
                                              "gc.pause.end",
                                              "gc.pause.start",
                                              "loader.module.init",
                                              "loader.module.load",
                                              "mutex.lock",
                                              "mutex.unlock",
                                              "mutex.wait",
                                              "schema.decode",
                                              "schema.encode",
                                              "strand.enqueued",
                                              "strand.migrated",
                                              "task.blocked",
                                              "task.cancelled",
                                              "task.completed",
                                              "task.failed",
                                              "task.resumed",
                                              "task.started",
                                              "watch.invalidate",
                                              "watch.read",
                                              "watch.write",
                                              "workflow.step.commit",
                                              "workflow.step.compensate",
                                              "workflow.step.retry",
                                              "workflow.step.start",
                                              "world.freeze",
                                              "world.mutation"};
  return names;
}

const std::set<std::string> &deterministic_source_set() {
  static const std::set<std::string> names = {
      "channel_order", "external_input", "random",
      "scheduler",     "stderr",         "stdout",
      "time",          "watch_revision", "workflow_history"};
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
  for (const char c : name) {
    if (!valid_name_char(c)) {
      return false;
    }
    if (c == '.') {
      ++components;
    }
  }
  return components >= 3U;
}

template <typename T> std::vector<T> sorted_unique(std::vector<T> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::vector<TraceAttribute>
sorted_attributes(std::vector<TraceAttribute> attributes) {
  std::sort(attributes.begin(), attributes.end(),
            [](const TraceAttribute &left, const TraceAttribute &right) {
              if (left.key != right.key) {
                return left.key < right.key;
              }
              return left.value < right.value;
            });
  attributes.erase(
      std::unique(attributes.begin(), attributes.end(),
                  [](const TraceAttribute &left, const TraceAttribute &right) {
                    return left.key == right.key && left.value == right.value;
                  }),
      attributes.end());
  return attributes;
}

std::vector<capability::CapabilityRequest>
sorted_capabilities(std::vector<capability::CapabilityRequest> grants) {
  std::sort(grants.begin(), grants.end(),
            [](const capability::CapabilityRequest &left,
               const capability::CapabilityRequest &right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              if (left.target != right.target) {
                return left.target < right.target;
              }
              if (left.reason != right.reason) {
                return left.reason < right.reason;
              }
              return left.flags < right.flags;
            });
  return grants;
}

ReplayDiagnostic diagnostic(std::string error_name, std::string message,
                            std::uint64_t expected_event_id = 0,
                            std::uint64_t actual_event_id = 0,
                            std::string event_name = {}) {
  ReplayDiagnostic out;
  out.error_name = std::move(error_name);
  out.message = std::move(message);
  out.expected_event_id = expected_event_id;
  out.actual_event_id = actual_event_id;
  out.event_name = std::move(event_name);
  return out;
}

bool parse_u64(const std::string &value, std::uint64_t *out) {
  try {
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed, 10);
    if (consumed != value.size()) {
      return false;
    }
    if (out != nullptr) {
      *out = static_cast<std::uint64_t>(parsed);
    }
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool parse_u32(const std::string &value, std::uint32_t *out) {
  std::uint64_t parsed = 0;
  if (!parse_u64(value, &parsed) || parsed > 0xffffffffULL) {
    return false;
  }
  if (out != nullptr) {
    *out = static_cast<std::uint32_t>(parsed);
  }
  return true;
}

std::map<std::string, std::string> parse_lines(const std::string &serialized) {
  std::map<std::string, std::string> values;
  std::istringstream in(serialized);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (first) {
      values["schema"] = line;
      first = false;
      continue;
    }
    if (line.empty()) {
      continue;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    values[line.substr(0, equals)] = line_unescape(line.substr(equals + 1U));
  }
  return values;
}

std::string value_or_empty(const std::map<std::string, std::string> &values,
                           const std::string &key) {
  const auto found = values.find(key);
  return found == values.end() ? std::string{} : found->second;
}

bool read_count(const std::map<std::string, std::string> &values,
                const std::string &key, std::uint32_t *count,
                std::vector<ReplayDiagnostic> *diagnostics) {
  const std::string raw = value_or_empty(values, key);
  if (raw.empty() || !parse_u32(raw, count)) {
    diagnostics->push_back(diagnostic("ReplayTraceParseError",
                                      "invalid or missing count: " + key));
    return false;
  }
  return true;
}

std::string trace_body(const ReplayTrace &trace) {
  const ReplayTrace normalized = normalize_trace(trace);
  std::ostringstream out;
  out << "package.lock=" << line_escape(normalized.package_lock_digest) << "\n";
  out << "artifact.count=" << normalized.artifact_digests.size() << "\n";
  for (std::size_t i = 0; i < normalized.artifact_digests.size(); ++i) {
    out << "artifact." << i
        << ".sha256=" << line_escape(normalized.artifact_digests[i]) << "\n";
  }
  out << "grant.count=" << normalized.capability_grants.size() << "\n";
  for (std::size_t i = 0; i < normalized.capability_grants.size(); ++i) {
    const capability::CapabilityRequest &grant =
        normalized.capability_grants[i];
    out << "grant." << i << ".name=" << line_escape(grant.name) << "\n";
    out << "grant." << i << ".target=" << line_escape(grant.target) << "\n";
    out << "grant." << i << ".reason=" << line_escape(grant.reason) << "\n";
    out << "grant." << i << ".flags=" << grant.flags << "\n";
  }
  out << "schema.count=" << normalized.schema_versions.size() << "\n";
  for (std::size_t i = 0; i < normalized.schema_versions.size(); ++i) {
    out << "schema." << i
        << ".name=" << line_escape(normalized.schema_versions[i]) << "\n";
  }
  out << "event.count=" << normalized.events.size() << "\n";
  for (std::size_t i = 0; i < normalized.events.size(); ++i) {
    const TraceEvent event = normalize_event(normalized.events[i]);
    const std::string prefix = "event." + std::to_string(i) + ".";
    out << prefix << "id=" << event.event_id << "\n";
    out << prefix << "name=" << line_escape(event.name) << "\n";
    out << prefix << "time=" << event.timestamp_or_virtual_time << "\n";
    out << prefix << "trace_id=" << line_escape(event.trace_id) << "\n";
    out << prefix << "span_id=" << line_escape(event.span_id) << "\n";
    out << prefix << "parent_span_id=" << line_escape(event.parent_span_id)
        << "\n";
    out << prefix << "task_id=" << line_escape(event.task_id) << "\n";
    out << prefix << "strand_id=" << line_escape(event.strand_id) << "\n";
    out << prefix << "worker_id=" << line_escape(event.worker_id) << "\n";
    out << prefix << "module_id=" << line_escape(event.module_id) << "\n";
    out << prefix << "method_id=" << line_escape(event.method_id) << "\n";
    out << prefix << "source.file=" << line_escape(event.source.file) << "\n";
    out << prefix << "source.line=" << event.source.line << "\n";
    out << prefix << "source.column=" << event.source.column << "\n";
    out << prefix << "world_epoch=" << event.world_epoch << "\n";
    out << prefix << "watch_epoch=" << event.watch_epoch << "\n";
    out << prefix << "severity=" << line_escape(event.severity) << "\n";
    out << prefix << "flags=" << event.flags << "\n";
    out << prefix << "attr.count=" << event.attributes.size() << "\n";
    for (std::size_t attr_i = 0; attr_i < event.attributes.size(); ++attr_i) {
      out << prefix << "attr." << attr_i
          << ".key=" << line_escape(event.attributes[attr_i].key) << "\n";
      out << prefix << "attr." << attr_i
          << ".value=" << line_escape(event.attributes[attr_i].value) << "\n";
    }
    out << prefix << "edge.count=" << event.causality_edges.size() << "\n";
    for (std::size_t edge_i = 0; edge_i < event.causality_edges.size();
         ++edge_i) {
      out << prefix << "edge." << edge_i
          << ".id=" << event.causality_edges[edge_i] << "\n";
    }
  }
  return out.str();
}

void emit_string_array(std::ostringstream &out, const char *name,
                       const std::vector<std::string> &values,
                       bool trailing_comma) {
  out << "  \"" << name << "\": [";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\"" << json_escape(values[i]) << "\"";
  }
  out << "]";
  if (trailing_comma) {
    out << ",";
  }
  out << "\n";
}

} // namespace

std::vector<std::string> canonical_event_names() {
  return std::vector<std::string>(canonical_event_set().begin(),
                                  canonical_event_set().end());
}

std::vector<std::string> canonical_deterministic_sources() {
  return std::vector<std::string>(deterministic_source_set().begin(),
                                  deterministic_source_set().end());
}

bool valid_event_name(const std::string &name) {
  if (canonical_event_set().find(name) != canonical_event_set().end()) {
    return true;
  }
  return valid_vendor_name(name);
}

bool valid_deterministic_source(const std::string &name) {
  return deterministic_source_set().find(name) !=
         deterministic_source_set().end();
}

TraceEvent make_event(std::string name,
                      std::vector<TraceAttribute> attributes) {
  TraceEvent event;
  event.name = std::move(name);
  event.attributes = sorted_attributes(std::move(attributes));
  return event;
}

TraceEvent normalize_event(TraceEvent event) {
  event.attributes = sorted_attributes(std::move(event.attributes));
  event.causality_edges = sorted_unique(std::move(event.causality_edges));
  return event;
}

ObservabilitySite normalize_site(ObservabilitySite site) { return site; }

ReplayMetadata normalize_metadata(ReplayMetadata metadata) {
  metadata.required_event_names =
      sorted_unique(std::move(metadata.required_event_names));
  metadata.deterministic_sources =
      sorted_unique(std::move(metadata.deterministic_sources));
  return metadata;
}

ReplayTrace normalize_trace(ReplayTrace trace) {
  if (trace.schema.empty()) {
    trace.schema = "amber.replay.v1";
  }
  trace.artifact_digests = sorted_unique(std::move(trace.artifact_digests));
  trace.schema_versions = sorted_unique(std::move(trace.schema_versions));
  trace.capability_grants =
      sorted_capabilities(std::move(trace.capability_grants));
  for (TraceEvent &event : trace.events) {
    event = normalize_event(std::move(event));
  }
  return trace;
}

ReplayValidationResult
validate_metadata(const ReplayMetadata &metadata,
                  const std::vector<ObservabilitySite> &sites) {
  ReplayValidationResult result;
  const ReplayMetadata normalized = normalize_metadata(metadata);
  for (const std::string &event_name : normalized.required_event_names) {
    if (!valid_event_name(event_name)) {
      result.diagnostics.push_back(diagnostic(
          "ReplayMetadataError", "invalid replay event name: " + event_name, 0,
          0, event_name));
    }
  }
  for (const std::string &source : normalized.deterministic_sources) {
    if (!valid_deterministic_source(source)) {
      result.diagnostics.push_back(diagnostic(
          "ReplayMetadataError", "invalid deterministic source: " + source));
    }
  }
  for (const ObservabilitySite &site : sites) {
    if (site.event_name.empty() || !valid_event_name(site.event_name)) {
      result.diagnostics.push_back(diagnostic(
          "ReplayMetadataError",
          "invalid observability site event name: " + site.event_name, 0, 0,
          site.event_name));
    }
    if (site.kind.empty()) {
      result.diagnostics.push_back(
          diagnostic("ReplayMetadataError", "observability site kind is empty",
                     0, 0, site.event_name));
    }
  }
  result.ok = result.diagnostics.empty();
  return result;
}

ReplayValidationResult validate_trace(const ReplayTrace &trace) {
  ReplayValidationResult result;
  const ReplayTrace normalized = normalize_trace(trace);
  if (normalized.schema != "amber.replay.v1") {
    result.diagnostics.push_back(
        diagnostic("ReplayTraceError",
                   "unsupported replay trace schema: " + normalized.schema));
  }
  std::uint64_t previous_event_id = 0;
  for (const TraceEvent &event : normalized.events) {
    if (event.event_id == 0 || event.event_id <= previous_event_id) {
      result.diagnostics.push_back(diagnostic(
          "ReplayTraceError", "trace event ids must be strictly increasing",
          previous_event_id + 1U, event.event_id, event.name));
    }
    previous_event_id = event.event_id;
    if (!valid_event_name(event.name)) {
      result.diagnostics.push_back(diagnostic(
          "ReplayTraceError", "invalid replay event name: " + event.name, 0,
          event.event_id, event.name));
    }
    for (const TraceAttribute &attribute : event.attributes) {
      if (attribute.key.empty()) {
        result.diagnostics.push_back(
            diagnostic("ReplayTraceError", "trace event attribute key is empty",
                       0, event.event_id, event.name));
      }
    }
  }
  result.consumed_events = normalized.events.size();
  result.ok = result.diagnostics.empty();
  return result;
}

ReplayValidationResult compare_traces(const ReplayTrace &expected,
                                      const ReplayTrace &actual) {
  ReplayValidationResult result;
  const ReplayTrace expected_trace = normalize_trace(expected);
  const ReplayTrace actual_trace = normalize_trace(actual);
  const std::size_t count =
      std::min(expected_trace.events.size(), actual_trace.events.size());
  for (std::size_t i = 0; i < count; ++i) {
    const TraceEvent expected_event = normalize_event(expected_trace.events[i]);
    const TraceEvent actual_event = normalize_event(actual_trace.events[i]);
    if (event_signature(expected_event) != event_signature(actual_event)) {
      result.diagnostics.push_back(diagnostic(
          "ReplayDivergenceError",
          "replay event diverged at index " + std::to_string(i),
          expected_event.event_id, actual_event.event_id, actual_event.name));
      result.consumed_events = i;
      result.ok = false;
      return result;
    }
  }
  result.consumed_events = count;
  if (expected_trace.events.size() != actual_trace.events.size()) {
    result.diagnostics.push_back(
        diagnostic("ReplayDivergenceError",
                   "replay event count diverged: expected " +
                       std::to_string(expected_trace.events.size()) + ", got " +
                       std::to_string(actual_trace.events.size())));
  }
  result.ok = result.diagnostics.empty();
  return result;
}

std::string event_signature(const TraceEvent &raw_event) {
  const TraceEvent event = normalize_event(raw_event);
  std::ostringstream out;
  out << event.event_id << "|" << event.name << "|"
      << event.timestamp_or_virtual_time << "|" << event.trace_id << "|"
      << event.span_id << "|" << event.parent_span_id << "|" << event.task_id
      << "|" << event.strand_id << "|" << event.worker_id << "|"
      << event.module_id << "|" << event.method_id << "|" << event.source.file
      << "|" << event.source.line << "|" << event.source.column << "|"
      << event.world_epoch << "|" << event.watch_epoch << "|" << event.severity
      << "|" << event.flags;
  for (const TraceAttribute &attribute : event.attributes) {
    out << "|a:" << attribute.key << "=" << attribute.value;
  }
  for (const std::uint64_t edge : event.causality_edges) {
    out << "|e:" << edge;
  }
  return out.str();
}

std::string trace_digest(const ReplayTrace &trace) {
  const ReplayTrace normalized = normalize_trace(trace);
  return "sha256:" +
         lexer::sha256_hex(normalized.schema + "\n" + trace_body(normalized));
}

std::string serialize_trace(const ReplayTrace &trace) {
  const ReplayTrace normalized = normalize_trace(trace);
  std::ostringstream out;
  out << normalized.schema << "\n";
  out << "trace.sha256=" << trace_digest(normalized) << "\n";
  out << trace_body(normalized);
  return out.str();
}

ReplayTraceParseResult parse_trace(const std::string &serialized) {
  ReplayTraceParseResult result;
  const std::map<std::string, std::string> values = parse_lines(serialized);
  result.trace.schema = value_or_empty(values, "schema");
  if (result.trace.schema != "amber.replay.v1") {
    result.diagnostics.push_back(
        diagnostic("ReplayTraceParseError",
                   "unsupported replay trace schema: " + result.trace.schema));
    return result;
  }
  result.trace.package_lock_digest = value_or_empty(values, "package.lock");

  std::uint32_t count = 0;
  if (!read_count(values, "artifact.count", &count, &result.diagnostics)) {
    return result;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    result.trace.artifact_digests.push_back(
        value_or_empty(values, "artifact." + std::to_string(i) + ".sha256"));
  }

  if (!read_count(values, "grant.count", &count, &result.diagnostics)) {
    return result;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::string prefix = "grant." + std::to_string(i) + ".";
    capability::CapabilityRequest grant;
    grant.name = value_or_empty(values, prefix + "name");
    grant.target = value_or_empty(values, prefix + "target");
    grant.reason = value_or_empty(values, prefix + "reason");
    if (!parse_u32(value_or_empty(values, prefix + "flags"), &grant.flags)) {
      result.diagnostics.push_back(diagnostic(
          "ReplayTraceParseError", "invalid capability grant flags"));
    }
    result.trace.capability_grants.push_back(std::move(grant));
  }

  if (!read_count(values, "schema.count", &count, &result.diagnostics)) {
    return result;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    result.trace.schema_versions.push_back(
        value_or_empty(values, "schema." + std::to_string(i) + ".name"));
  }

  if (!read_count(values, "event.count", &count, &result.diagnostics)) {
    return result;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::string prefix = "event." + std::to_string(i) + ".";
    TraceEvent event;
    parse_u64(value_or_empty(values, prefix + "id"), &event.event_id);
    event.name = value_or_empty(values, prefix + "name");
    parse_u64(value_or_empty(values, prefix + "time"),
              &event.timestamp_or_virtual_time);
    event.trace_id = value_or_empty(values, prefix + "trace_id");
    event.span_id = value_or_empty(values, prefix + "span_id");
    event.parent_span_id = value_or_empty(values, prefix + "parent_span_id");
    event.task_id = value_or_empty(values, prefix + "task_id");
    event.strand_id = value_or_empty(values, prefix + "strand_id");
    event.worker_id = value_or_empty(values, prefix + "worker_id");
    event.module_id = value_or_empty(values, prefix + "module_id");
    event.method_id = value_or_empty(values, prefix + "method_id");
    event.source.file = value_or_empty(values, prefix + "source.file");
    parse_u32(value_or_empty(values, prefix + "source.line"),
              &event.source.line);
    parse_u32(value_or_empty(values, prefix + "source.column"),
              &event.source.column);
    parse_u64(value_or_empty(values, prefix + "world_epoch"),
              &event.world_epoch);
    parse_u64(value_or_empty(values, prefix + "watch_epoch"),
              &event.watch_epoch);
    event.severity = value_or_empty(values, prefix + "severity");
    parse_u32(value_or_empty(values, prefix + "flags"), &event.flags);

    std::uint32_t attr_count = 0;
    if (parse_u32(value_or_empty(values, prefix + "attr.count"), &attr_count)) {
      for (std::uint32_t attr_i = 0; attr_i < attr_count; ++attr_i) {
        const std::string attr_prefix =
            prefix + "attr." + std::to_string(attr_i) + ".";
        event.attributes.push_back(
            {value_or_empty(values, attr_prefix + "key"),
             value_or_empty(values, attr_prefix + "value")});
      }
    }

    std::uint32_t edge_count = 0;
    if (parse_u32(value_or_empty(values, prefix + "edge.count"), &edge_count)) {
      for (std::uint32_t edge_i = 0; edge_i < edge_count; ++edge_i) {
        std::uint64_t edge = 0;
        parse_u64(value_or_empty(values, prefix + "edge." +
                                             std::to_string(edge_i) + ".id"),
                  &edge);
        event.causality_edges.push_back(edge);
      }
    }
    result.trace.events.push_back(normalize_event(std::move(event)));
  }

  if (result.diagnostics.empty()) {
    const ReplayValidationResult validation = validate_trace(result.trace);
    result.diagnostics = validation.diagnostics;
  }
  return result;
}

std::string trace_to_json(const ReplayTrace &raw_trace) {
  const ReplayTrace trace = normalize_trace(raw_trace);
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.replay.v1\",\n";
  out << "  \"digest\": \"" << json_escape(trace_digest(trace)) << "\",\n";
  out << "  \"package_lock_digest\": \""
      << json_escape(trace.package_lock_digest) << "\",\n";
  emit_string_array(out, "artifact_digests", trace.artifact_digests, true);
  emit_string_array(out, "schema_versions", trace.schema_versions, true);
  out << "  \"capability_grants\": [";
  for (std::size_t i = 0; i < trace.capability_grants.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const capability::CapabilityRequest &grant = trace.capability_grants[i];
    out << "\n    {\"name\":\"" << json_escape(grant.name) << "\",\"target\":\""
        << json_escape(grant.target) << "\",\"reason\":\""
        << json_escape(grant.reason) << "\",\"flags\":" << grant.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"events\": [";
  for (std::size_t i = 0; i < trace.events.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const TraceEvent event = normalize_event(trace.events[i]);
    out << "\n    {\"id\":" << event.event_id << ",\"name\":\""
        << json_escape(event.name)
        << "\",\"time\":" << event.timestamp_or_virtual_time
        << ",\"trace_id\":\"" << json_escape(event.trace_id)
        << "\",\"span_id\":\"" << json_escape(event.span_id)
        << "\",\"parent_span_id\":\"" << json_escape(event.parent_span_id)
        << "\",\"task_id\":\"" << json_escape(event.task_id)
        << "\",\"strand_id\":\"" << json_escape(event.strand_id)
        << "\",\"worker_id\":\"" << json_escape(event.worker_id)
        << "\",\"module_id\":\"" << json_escape(event.module_id)
        << "\",\"method_id\":\"" << json_escape(event.method_id)
        << "\",\"source\":{\"file\":\"" << json_escape(event.source.file)
        << "\",\"line\":" << event.source.line
        << ",\"column\":" << event.source.column
        << "},\"world_epoch\":" << event.world_epoch
        << ",\"watch_epoch\":" << event.watch_epoch << ",\"severity\":\""
        << json_escape(event.severity) << "\",\"attributes\":[";
    for (std::size_t attr_i = 0; attr_i < event.attributes.size(); ++attr_i) {
      if (attr_i != 0U) {
        out << ",";
      }
      out << "{\"key\":\"" << json_escape(event.attributes[attr_i].key)
          << "\",\"value\":\"" << json_escape(event.attributes[attr_i].value)
          << "\"}";
    }
    out << "],\"causality_edges\":[";
    for (std::size_t edge_i = 0; edge_i < event.causality_edges.size();
         ++edge_i) {
      if (edge_i != 0U) {
        out << ",";
      }
      out << event.causality_edges[edge_i];
    }
    out << "],\"flags\":" << event.flags << "}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

std::string validation_to_json(const ReplayValidationResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.replay.v1\",\n";
  out << "  \"status\": \"" << (result.ok ? "ok" : "error") << "\",\n";
  out << "  \"consumed_events\": " << result.consumed_events << ",\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const ReplayDiagnostic &entry = result.diagnostics[i];
    out << "\n    {\"error_name\":\"" << json_escape(entry.error_name)
        << "\",\"message\":\"" << json_escape(entry.message)
        << "\",\"expected_event_id\":" << entry.expected_event_id
        << ",\"actual_event_id\":" << entry.actual_event_id
        << ",\"event_name\":\"" << json_escape(entry.event_name) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::replay
