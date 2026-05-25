#include "profile/modern.h"

#include "profile/effects.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace amber::modern {

namespace {

std::string trim(const std::string &value) {
  std::size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return value;
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

std::map<std::string, std::string> parse_lines(const std::string &source) {
  std::map<std::string, std::string> values;
  std::istringstream in(source);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (first) {
      values["schema"] = trim(line);
      first = false;
      continue;
    }
    const std::string text = trim(line);
    if (text.empty() || text[0] == '#') {
      continue;
    }
    const std::size_t equals = text.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    values[trim(text.substr(0, equals))] =
        line_unescape(trim(text.substr(equals + 1U)));
  }
  return values;
}

std::string value_or_empty(const std::map<std::string, std::string> &values,
                           const std::string &key) {
  const auto found = values.find(key);
  return found == values.end() ? std::string{} : found->second;
}

bool parse_u32(const std::string &value, std::uint32_t *out) {
  try {
    const std::string text = trim(value);
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(text, &consumed, 10);
    if (consumed != text.size() || parsed > 0xffffffffUL) {
      return false;
    }
    if (out != nullptr) {
      *out = static_cast<std::uint32_t>(parsed);
    }
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool parse_u64(const std::string &value, std::uint64_t *out) {
  try {
    const std::string text = trim(value);
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(text, &consumed, 10);
    if (consumed != text.size()) {
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

std::vector<std::string> split_csv(const std::string &value) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::string item = trim(value.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!item.empty()) {
      out.push_back(item);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1U;
  }
  return out;
}

std::vector<std::string> sorted_unique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

bool valid_name_char(char c) {
  const unsigned char ch = static_cast<unsigned char>(c);
  return ch >= 128U || std::isalnum(ch) != 0 || c == '_' || c == '-' ||
         c == '.' || c == ':' || c == '#' || c == '$' || c == '@' || c == '/' ||
         c == '!' || c == '?' || c == '=';
}

bool valid_identifierish(const std::string &name) {
  if (name.empty() || name.front() == '.' || name.back() == '.') {
    return false;
  }
  for (const char c : name) {
    if (!valid_name_char(c)) {
      return false;
    }
  }
  return true;
}

const std::set<std::string> &symbol_kind_set() {
  static const std::set<std::string> names = {
      "class",      "constant", "field",  "function", "import_alias", "ivar",
      "last_value", "local",    "method", "mixin",    "module",       "param",
      "policy",     "property", "schema", "step",     "workflow"};
  return names;
}

const std::set<std::string> &visibility_set() {
  static const std::set<std::string> names = {"internal", "local", "private",
                                              "public"};
  return names;
}

const std::set<std::string> &patch_operation_set() {
  static const std::set<std::string> names = {"annotate", "delete", "insert",
                                              "rename", "replace"};
  return names;
}

const std::set<std::string> &patch_capability_set() {
  static const std::set<std::string> names = {"explain",    "format",   "patch",
                                              "provenance", "refactor", "test"};
  return names;
}

const std::set<std::string> &contract_kind_set() {
  static const std::set<std::string> names = {"ensure", "invariant", "require"};
  return names;
}

const std::set<std::string> &lineage_kind_set() {
  static const std::set<std::string> names = {
      "aggregate", "export", "join", "notebook_cell", "source", "transform"};
  return names;
}

const std::set<std::string> &policy_action_set() {
  static const std::set<std::string> names = {"allow", "deny", "redact"};
  return names;
}

const std::set<std::string> &workflow_event_set() {
  static const std::set<std::string> names = {"commit", "compensation",
                                              "failure", "retry", "start"};
  return names;
}

ModernDiagnostic diagnostic(std::string error_name, std::string message,
                            std::string subject = {}, std::string field = {}) {
  ModernDiagnostic out;
  out.error_name = std::move(error_name);
  out.message = std::move(message);
  out.subject = std::move(subject);
  out.field = std::move(field);
  return out;
}

bool read_count(const std::map<std::string, std::string> &values,
                const std::string &key, std::uint32_t *count,
                std::vector<ModernDiagnostic> *diagnostics,
                bool required = true) {
  const std::string raw = value_or_empty(values, key);
  if (raw.empty()) {
    if (required && diagnostics != nullptr) {
      diagnostics->push_back(
          diagnostic("ModernProfileParseError", "missing count: " + key));
    }
    if (count != nullptr) {
      *count = 0;
    }
    return !required;
  }
  if (!parse_u32(raw, count)) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(
          diagnostic("ModernProfileParseError", "invalid count: " + key));
    }
    return false;
  }
  return true;
}

std::string prefixed_key(const std::string &prefix, const std::string &name) {
  return prefix + "." + name;
}

SourceLocation parse_source_location(const std::string &value) {
  SourceLocation source;
  const std::string text = trim(value);
  if (text.empty()) {
    return source;
  }
  const std::size_t last_colon = text.find_last_of(':');
  if (last_colon == std::string::npos) {
    source.file = text;
    return source;
  }
  const std::size_t prev_colon = last_colon == 0U
                                     ? std::string::npos
                                     : text.find_last_of(':', last_colon - 1U);
  if (prev_colon == std::string::npos) {
    source.file = text;
    return source;
  }
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  if (!parse_u32(text.substr(prev_colon + 1U, last_colon - prev_colon - 1U),
                 &line) ||
      !parse_u32(text.substr(last_colon + 1U), &column)) {
    source.file = text;
    return source;
  }
  source.file = text.substr(0, prev_colon);
  source.line = line;
  source.column = column;
  return source;
}

std::vector<SourceLocation> parse_source_locations(const std::string &value) {
  std::vector<SourceLocation> out;
  for (const std::string &item : split_csv(value)) {
    out.push_back(parse_source_location(item));
  }
  return out;
}

void parse_effects_value(const std::map<std::string, std::string> &values,
                         const std::string &key,
                         std::vector<std::string> *effects,
                         std::vector<ModernDiagnostic> *diagnostics,
                         const std::string &subject) {
  const std::string raw = value_or_empty(values, key);
  if (raw.empty()) {
    if (effects != nullptr) {
      *effects = {};
    }
    return;
  }
  std::vector<effect::EffectDiagnostic> effect_diagnostics;
  std::vector<std::string> parsed;
  if (!effect::parse_effect_row(raw, &parsed, &effect_diagnostics, subject)) {
    if (diagnostics != nullptr) {
      for (const effect::EffectDiagnostic &item : effect_diagnostics) {
        diagnostics->push_back(
            diagnostic("ModernProfileParseError", item.message, subject, key));
      }
    }
  }
  if (effects != nullptr) {
    *effects = std::move(parsed);
  }
}

AgentSymbol parse_symbol(const std::map<std::string, std::string> &values,
                         const std::string &prefix) {
  AgentSymbol symbol;
  symbol.symbol_id = value_or_empty(values, prefixed_key(prefix, "id"));
  symbol.name = value_or_empty(values, prefixed_key(prefix, "name"));
  symbol.kind = value_or_empty(values, prefixed_key(prefix, "kind"));
  symbol.module = value_or_empty(values, prefixed_key(prefix, "module"));
  symbol.visibility =
      value_or_empty(values, prefixed_key(prefix, "visibility"));
  symbol.source = parse_source_location(
      value_or_empty(values, prefixed_key(prefix, "source")));
  symbol.defined_in =
      value_or_empty(values, prefixed_key(prefix, "defined_in"));
  symbol.references = parse_source_locations(
      value_or_empty(values, prefixed_key(prefix, "references")));
  symbol.type_summary = value_or_empty(values, prefixed_key(prefix, "type"));
  symbol.effect_summary =
      value_or_empty(values, prefixed_key(prefix, "effects"));
  symbol.schema_summary =
      value_or_empty(values, prefixed_key(prefix, "schema"));
  symbol.doc_summary = value_or_empty(values, prefixed_key(prefix, "doc"));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "flags")),
            &symbol.flags);
  return normalize_agent_symbol(std::move(symbol));
}

AgentPatchOperation
parse_patch_operation(const std::map<std::string, std::string> &values,
                      const std::string &prefix) {
  AgentPatchOperation operation;
  operation.op = value_or_empty(values, prefixed_key(prefix, "op"));
  operation.symbol_id =
      value_or_empty(values, prefixed_key(prefix, "symbol_id"));
  operation.new_name = value_or_empty(values, prefixed_key(prefix, "new_name"));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "flags")),
            &operation.flags);
  operation.op = lower_ascii(trim(operation.op));
  operation.symbol_id = trim(operation.symbol_id);
  operation.new_name = trim(operation.new_name);
  return operation;
}

AgentPatch parse_patch(const std::map<std::string, std::string> &values,
                       const std::string &prefix,
                       std::vector<ModernDiagnostic> *diagnostics) {
  AgentPatch patch;
  patch.patch_id = value_or_empty(values, prefixed_key(prefix, "id"));
  patch.intent = value_or_empty(values, prefixed_key(prefix, "intent"));
  patch.tool = value_or_empty(values, prefixed_key(prefix, "tool"));
  patch.request_digest =
      value_or_empty(values, prefixed_key(prefix, "request_digest"));
  patch.capabilities =
      split_csv(value_or_empty(values, prefixed_key(prefix, "capabilities")));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "flags")),
            &patch.flags);
  std::uint32_t operation_count = 0;
  read_count(values, prefixed_key(prefix, "operation.count"), &operation_count,
             diagnostics, false);
  for (std::uint32_t i = 0; i < operation_count; ++i) {
    patch.operations.push_back(parse_patch_operation(
        values, prefixed_key(prefix, "operation." + std::to_string(i))));
  }
  return normalize_agent_patch(std::move(patch));
}

ProvenanceRecord
parse_provenance(const std::map<std::string, std::string> &values,
                 const std::string &prefix) {
  ProvenanceRecord record;
  record.patch_id = value_or_empty(values, prefixed_key(prefix, "patch_id"));
  record.tool = value_or_empty(values, prefixed_key(prefix, "tool"));
  record.request_digest =
      value_or_empty(values, prefixed_key(prefix, "request_digest"));
  record.files_changed =
      split_csv(value_or_empty(values, prefixed_key(prefix, "files_changed")));
  record.symbols_changed = split_csv(
      value_or_empty(values, prefixed_key(prefix, "symbols_changed")));
  record.diagnostics_before = split_csv(
      value_or_empty(values, prefixed_key(prefix, "diagnostics_before")));
  record.diagnostics_after = split_csv(
      value_or_empty(values, prefixed_key(prefix, "diagnostics_after")));
  record.checks_run =
      split_csv(value_or_empty(values, prefixed_key(prefix, "checks_run")));
  record.artifact_digests = split_csv(
      value_or_empty(values, prefixed_key(prefix, "artifact_digests")));
  record.human_approval =
      value_or_empty(values, prefixed_key(prefix, "human_approval"));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "flags")),
            &record.flags);
  return normalize_provenance_record(std::move(record));
}

ContractSpec parse_contract(const std::map<std::string, std::string> &values,
                            const std::string &prefix,
                            std::vector<ModernDiagnostic> *diagnostics) {
  ContractSpec contract;
  contract.owner = value_or_empty(values, prefixed_key(prefix, "owner"));
  contract.kind = value_or_empty(values, prefixed_key(prefix, "kind"));
  contract.expression = value_or_empty(values, prefixed_key(prefix, "expr"));
  parse_effects_value(values, prefixed_key(prefix, "effects"),
                      &contract.effect_row, diagnostics, contract.owner);
  contract.source = parse_source_location(
      value_or_empty(values, prefixed_key(prefix, "source")));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "flags")),
            &contract.flags);
  return normalize_contract_spec(std::move(contract));
}

PropertySpec parse_property(const std::map<std::string, std::string> &values,
                            const std::string &prefix) {
  PropertySpec property;
  property.name = value_or_empty(values, prefixed_key(prefix, "name"));
  property.owner = value_or_empty(values, prefixed_key(prefix, "owner"));
  parse_u64(value_or_empty(values, prefixed_key(prefix, "seed")),
            &property.seed);
  property.generator =
      value_or_empty(values, prefixed_key(prefix, "generator"));
  property.shrinker_path =
      value_or_empty(values, prefixed_key(prefix, "shrinker_path"));
  property.counterexample =
      value_or_empty(values, prefixed_key(prefix, "counterexample"));
  property.profile_set =
      split_csv(value_or_empty(values, prefixed_key(prefix, "profiles")));
  property.dependency_fingerprints = split_csv(
      value_or_empty(values, prefixed_key(prefix, "dependency_fingerprints")));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "flags")),
            &property.flags);
  return normalize_property_spec(std::move(property));
}

PrivacyLabel parse_label(const std::map<std::string, std::string> &values,
                         const std::string &prefix) {
  PrivacyLabel label;
  label.name = value_or_empty(values, prefixed_key(prefix, "name"));
  label.kind = value_or_empty(values, prefixed_key(prefix, "kind"));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "flags")),
            &label.flags);
  return normalize_privacy_label(std::move(label));
}

PrivacyPolicyRule parse_policy(const std::map<std::string, std::string> &values,
                               const std::string &prefix) {
  PrivacyPolicyRule rule;
  rule.policy = value_or_empty(values, prefixed_key(prefix, "policy"));
  rule.action = value_or_empty(values, prefixed_key(prefix, "action"));
  rule.label = value_or_empty(values, prefixed_key(prefix, "label"));
  rule.aggregate = value_or_empty(values, prefixed_key(prefix, "aggregate"));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "min_group")),
            &rule.min_group);
  parse_u32(value_or_empty(values, prefixed_key(prefix, "flags")), &rule.flags);
  return normalize_privacy_policy_rule(std::move(rule));
}

LineageNode parse_lineage(const std::map<std::string, std::string> &values,
                          const std::string &prefix) {
  LineageNode node;
  node.node_id = value_or_empty(values, prefixed_key(prefix, "id"));
  node.kind = value_or_empty(values, prefixed_key(prefix, "kind"));
  node.inputs =
      split_csv(value_or_empty(values, prefixed_key(prefix, "inputs")));
  node.output = value_or_empty(values, prefixed_key(prefix, "output"));
  node.schema_fingerprint =
      value_or_empty(values, prefixed_key(prefix, "schema_fingerprint"));
  node.labels =
      split_csv(value_or_empty(values, prefixed_key(prefix, "labels")));
  node.source = parse_source_location(
      value_or_empty(values, prefixed_key(prefix, "source")));
  node.trace_span = value_or_empty(values, prefixed_key(prefix, "trace_span"));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "flags")), &node.flags);
  return normalize_lineage_node(std::move(node));
}

WorkflowStep
parse_workflow_step(const std::map<std::string, std::string> &values,
                    const std::string &prefix,
                    std::vector<ModernDiagnostic> *diagnostics) {
  WorkflowStep step;
  step.workflow = value_or_empty(values, prefixed_key(prefix, "workflow"));
  step.name = value_or_empty(values, prefixed_key(prefix, "name"));
  parse_effects_value(values, prefixed_key(prefix, "effects"), &step.effect_row,
                      diagnostics, step.name);
  step.depends_on =
      split_csv(value_or_empty(values, prefixed_key(prefix, "depends_on")));
  step.retry_policy = value_or_empty(values, prefixed_key(prefix, "retry"));
  step.idempotency_key =
      value_or_empty(values, prefixed_key(prefix, "idempotency_key"));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "timeout_ms")),
            &step.timeout_ms);
  parse_u32(value_or_empty(values, prefixed_key(prefix, "flags")), &step.flags);
  return normalize_workflow_step(std::move(step));
}

WorkflowHistoryEvent
parse_workflow_history(const std::map<std::string, std::string> &values,
                       const std::string &prefix) {
  WorkflowHistoryEvent event;
  event.workflow_id =
      value_or_empty(values, prefixed_key(prefix, "workflow_id"));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "version")),
            &event.workflow_version);
  event.step = value_or_empty(values, prefixed_key(prefix, "step"));
  event.event = value_or_empty(values, prefixed_key(prefix, "event"));
  event.input_digest =
      value_or_empty(values, prefixed_key(prefix, "input_digest"));
  event.output_digest =
      value_or_empty(values, prefixed_key(prefix, "output_digest"));
  event.schema_version =
      value_or_empty(values, prefixed_key(prefix, "schema_version"));
  event.effect_grants =
      split_csv(value_or_empty(values, prefixed_key(prefix, "effect_grants")));
  event.idempotency_key =
      value_or_empty(values, prefixed_key(prefix, "idempotency_key"));
  event.trace_id = value_or_empty(values, prefixed_key(prefix, "trace_id"));
  parse_u32(value_or_empty(values, prefixed_key(prefix, "flags")),
            &event.flags);
  return normalize_workflow_history_event(std::move(event));
}

void emit_string_array(std::ostringstream &out,
                       const std::vector<std::string> &values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\"" << json_escape(values[i]) << "\"";
  }
  out << "]";
}

void emit_source_location(std::ostringstream &out,
                          const SourceLocation &source) {
  out << "{\"file\":\"" << json_escape(source.file)
      << "\",\"line\":" << source.line << ",\"column\":" << source.column
      << "}";
}

void emit_source_array(std::ostringstream &out,
                       const std::vector<SourceLocation> &values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    emit_source_location(out, values[i]);
  }
  out << "]";
}

bool has_label_rule(const std::vector<PrivacyPolicyRule> &policies,
                    const std::string &label, const std::string &action) {
  for (const PrivacyPolicyRule &rule : policies) {
    if (rule.label == label && (action.empty() || rule.action == action)) {
      return true;
    }
  }
  return false;
}

} // namespace

bool valid_symbol_kind(const std::string &kind) {
  return symbol_kind_set().find(lower_ascii(trim(kind))) !=
         symbol_kind_set().end();
}

bool valid_visibility(const std::string &visibility) {
  return visibility_set().find(lower_ascii(trim(visibility))) !=
         visibility_set().end();
}

bool valid_patch_operation(const std::string &op) {
  return patch_operation_set().find(lower_ascii(trim(op))) !=
         patch_operation_set().end();
}

bool valid_contract_kind(const std::string &kind) {
  return contract_kind_set().find(lower_ascii(trim(kind))) !=
         contract_kind_set().end();
}

bool valid_lineage_kind(const std::string &kind) {
  return lineage_kind_set().find(lower_ascii(trim(kind))) !=
         lineage_kind_set().end();
}

bool valid_policy_action(const std::string &action) {
  return policy_action_set().find(lower_ascii(trim(action))) !=
         policy_action_set().end();
}

bool valid_workflow_event(const std::string &event) {
  return workflow_event_set().find(lower_ascii(trim(event))) !=
         workflow_event_set().end();
}

SourceLocation source_from_span(const lexer::Span &span) {
  SourceLocation source;
  source.file = span.file;
  source.line = static_cast<std::uint32_t>(span.start.line);
  source.column = static_cast<std::uint32_t>(span.start.col);
  return source;
}

std::string source_location_to_text(const SourceLocation &source) {
  if (source.file.empty()) {
    return {};
  }
  std::ostringstream out;
  out << source.file << ":" << source.line << ":" << source.column;
  return out.str();
}

AgentSymbol normalize_agent_symbol(AgentSymbol symbol) {
  symbol.symbol_id = trim(symbol.symbol_id);
  symbol.name = trim(symbol.name);
  symbol.kind = lower_ascii(trim(symbol.kind));
  symbol.module = trim(symbol.module);
  symbol.visibility = lower_ascii(trim(symbol.visibility));
  if (symbol.visibility.empty()) {
    symbol.visibility = "internal";
  }
  symbol.defined_in = trim(symbol.defined_in);
  std::sort(symbol.references.begin(), symbol.references.end(),
            [](const SourceLocation &left, const SourceLocation &right) {
              if (left.file != right.file) {
                return left.file < right.file;
              }
              if (left.line != right.line) {
                return left.line < right.line;
              }
              return left.column < right.column;
            });
  return symbol;
}

AgentPatch normalize_agent_patch(AgentPatch patch) {
  patch.patch_id = trim(patch.patch_id);
  patch.intent = lower_ascii(trim(patch.intent));
  patch.tool = trim(patch.tool);
  patch.request_digest = trim(patch.request_digest);
  for (std::string &capability : patch.capabilities) {
    capability = lower_ascii(trim(capability));
  }
  patch.capabilities = sorted_unique(std::move(patch.capabilities));
  for (AgentPatchOperation &operation : patch.operations) {
    operation.op = lower_ascii(trim(operation.op));
    operation.symbol_id = trim(operation.symbol_id);
    operation.new_name = trim(operation.new_name);
  }
  std::sort(
      patch.operations.begin(), patch.operations.end(),
      [](const AgentPatchOperation &left, const AgentPatchOperation &right) {
        if (left.symbol_id != right.symbol_id) {
          return left.symbol_id < right.symbol_id;
        }
        return left.op < right.op;
      });
  return patch;
}

ProvenanceRecord normalize_provenance_record(ProvenanceRecord record) {
  record.patch_id = trim(record.patch_id);
  record.tool = trim(record.tool);
  record.request_digest = trim(record.request_digest);
  record.files_changed = sorted_unique(std::move(record.files_changed));
  record.symbols_changed = sorted_unique(std::move(record.symbols_changed));
  record.checks_run = sorted_unique(std::move(record.checks_run));
  record.artifact_digests = sorted_unique(std::move(record.artifact_digests));
  record.human_approval = trim(record.human_approval);
  return record;
}

ContractSpec normalize_contract_spec(ContractSpec contract) {
  contract.owner = trim(contract.owner);
  contract.kind = lower_ascii(trim(contract.kind));
  contract.expression = trim(contract.expression);
  contract.effect_row =
      effect::normalize_effects(std::move(contract.effect_row));
  return contract;
}

PropertySpec normalize_property_spec(PropertySpec property) {
  property.name = trim(property.name);
  property.owner = trim(property.owner);
  property.generator = trim(property.generator);
  property.shrinker_path = trim(property.shrinker_path);
  property.profile_set = sorted_unique(std::move(property.profile_set));
  property.dependency_fingerprints =
      sorted_unique(std::move(property.dependency_fingerprints));
  return property;
}

PrivacyLabel normalize_privacy_label(PrivacyLabel label) {
  label.name = lower_ascii(trim(label.name));
  label.kind = lower_ascii(trim(label.kind));
  if (label.kind.empty()) {
    label.kind = label.name;
  }
  if (label.kind == "pii" || label.kind == "secret") {
    label.flags |= kPrivacyLabelFlagSensitive;
  }
  return label;
}

PrivacyPolicyRule normalize_privacy_policy_rule(PrivacyPolicyRule rule) {
  rule.policy = trim(rule.policy);
  rule.action = lower_ascii(trim(rule.action));
  rule.label = lower_ascii(trim(rule.label));
  rule.aggregate = trim(rule.aggregate);
  return rule;
}

LineageNode normalize_lineage_node(LineageNode node) {
  node.node_id = trim(node.node_id);
  node.kind = lower_ascii(trim(node.kind));
  node.output = trim(node.output);
  node.inputs = sorted_unique(std::move(node.inputs));
  for (std::string &label : node.labels) {
    label = lower_ascii(trim(label));
  }
  node.labels = sorted_unique(std::move(node.labels));
  return node;
}

WorkflowStep normalize_workflow_step(WorkflowStep step) {
  step.workflow = trim(step.workflow);
  step.name = trim(step.name);
  step.effect_row = effect::normalize_effects(std::move(step.effect_row));
  step.depends_on = sorted_unique(std::move(step.depends_on));
  step.retry_policy = trim(step.retry_policy);
  step.idempotency_key = trim(step.idempotency_key);
  if (!step.idempotency_key.empty()) {
    step.flags |= kWorkflowStepFlagIdempotent;
  }
  return step;
}

WorkflowHistoryEvent
normalize_workflow_history_event(WorkflowHistoryEvent event) {
  event.workflow_id = trim(event.workflow_id);
  event.step = trim(event.step);
  event.event = lower_ascii(trim(event.event));
  event.effect_grants =
      effect::normalize_effects(std::move(event.effect_grants));
  event.idempotency_key = trim(event.idempotency_key);
  event.trace_id = trim(event.trace_id);
  return event;
}

AgentValidationResult
validate_agent_metadata(const std::vector<AgentSymbol> &symbols,
                        const std::vector<AgentPatch> &patches,
                        const std::vector<ProvenanceRecord> &provenance) {
  AgentValidationResult result;
  std::set<std::string> symbol_ids;
  std::set<std::string> patch_ids;
  for (AgentSymbol symbol : symbols) {
    symbol = normalize_agent_symbol(std::move(symbol));
    result.symbols.push_back(symbol);
    if (!valid_identifierish(symbol.symbol_id)) {
      result.diagnostics.push_back(diagnostic(
          "AgentToolingError", "invalid symbol id", symbol.symbol_id, "id"));
    }
    if (!valid_identifierish(symbol.name)) {
      result.diagnostics.push_back(diagnostic("AgentToolingError",
                                              "invalid symbol name",
                                              symbol.symbol_id, "name"));
    }
    if (!valid_symbol_kind(symbol.kind)) {
      result.diagnostics.push_back(diagnostic("AgentToolingError",
                                              "invalid symbol kind",
                                              symbol.symbol_id, "kind"));
    }
    if (!valid_identifierish(symbol.module)) {
      result.diagnostics.push_back(diagnostic("AgentToolingError",
                                              "invalid symbol module",
                                              symbol.symbol_id, "module"));
    }
    if (!valid_visibility(symbol.visibility)) {
      result.diagnostics.push_back(diagnostic("AgentToolingError",
                                              "invalid visibility",
                                              symbol.symbol_id, "visibility"));
    }
    if (!symbol_ids.insert(symbol.symbol_id).second) {
      result.diagnostics.push_back(diagnostic(
          "AgentToolingError", "duplicate symbol id", symbol.symbol_id, "id"));
    }
  }
  for (AgentPatch patch : patches) {
    patch = normalize_agent_patch(std::move(patch));
    result.patches.push_back(patch);
    if (!valid_identifierish(patch.patch_id)) {
      result.diagnostics.push_back(diagnostic(
          "AgentPatchError", "invalid patch id", patch.patch_id, "id"));
    }
    if (!patch_ids.insert(patch.patch_id).second) {
      result.diagnostics.push_back(diagnostic(
          "AgentPatchError", "duplicate patch id", patch.patch_id, "id"));
    }
    if (patch.intent.empty()) {
      result.diagnostics.push_back(diagnostic("AgentPatchError",
                                              "patch intent is required",
                                              patch.patch_id, "intent"));
    }
    if (patch.tool.empty() || patch.request_digest.empty()) {
      result.diagnostics.push_back(diagnostic(
          "AgentPatchError", "patch provenance tool and digest are required",
          patch.patch_id, "provenance"));
    }
    for (const std::string &capability : patch.capabilities) {
      if (patch_capability_set().find(capability) ==
          patch_capability_set().end()) {
        result.diagnostics.push_back(diagnostic("AgentPatchError",
                                                "unknown patch capability",
                                                patch.patch_id, capability));
      }
    }
    if (patch.operations.empty()) {
      result.diagnostics.push_back(diagnostic(
          "AgentPatchError", "patch must contain at least one operation",
          patch.patch_id, "operations"));
    }
    for (const AgentPatchOperation &operation : patch.operations) {
      if (!valid_patch_operation(operation.op)) {
        result.diagnostics.push_back(diagnostic("AgentPatchError",
                                                "invalid patch operation",
                                                patch.patch_id, operation.op));
      }
      if (symbol_ids.find(operation.symbol_id) == symbol_ids.end()) {
        result.diagnostics.push_back(
            diagnostic("AgentPatchError", "patch references stale symbol id",
                       patch.patch_id, operation.symbol_id));
      }
      if (operation.op == "rename" &&
          !valid_identifierish(operation.new_name)) {
        result.diagnostics.push_back(diagnostic(
            "AgentPatchError", "rename operation requires a valid new name",
            patch.patch_id, operation.symbol_id));
      }
    }
  }
  for (ProvenanceRecord record : provenance) {
    record = normalize_provenance_record(std::move(record));
    result.provenance.push_back(record);
    if (!patch_ids.empty() &&
        patch_ids.find(record.patch_id) == patch_ids.end()) {
      result.diagnostics.push_back(diagnostic(
          "AgentProvenanceError", "provenance references unknown patch id",
          record.patch_id, "patch_id"));
    }
    if (record.tool.empty() || record.request_digest.empty()) {
      result.diagnostics.push_back(diagnostic(
          "AgentProvenanceError", "provenance tool and digest are required",
          record.patch_id, "provenance"));
    }
    if (record.checks_run.empty()) {
      result.diagnostics.push_back(diagnostic(
          "AgentProvenanceError", "provenance must record checks run",
          record.patch_id, "checks"));
    }
  }
  std::sort(result.symbols.begin(), result.symbols.end(),
            [](const AgentSymbol &left, const AgentSymbol &right) {
              return left.symbol_id < right.symbol_id;
            });
  std::sort(result.patches.begin(), result.patches.end(),
            [](const AgentPatch &left, const AgentPatch &right) {
              return left.patch_id < right.patch_id;
            });
  result.ok = result.diagnostics.empty();
  return result;
}

ContractValidationResult
validate_contract_metadata(const std::vector<ContractSpec> &contracts,
                           const std::vector<PropertySpec> &properties) {
  ContractValidationResult result;
  for (ContractSpec contract : contracts) {
    contract = normalize_contract_spec(std::move(contract));
    result.contracts.push_back(contract);
    if (!valid_identifierish(contract.owner)) {
      result.diagnostics.push_back(diagnostic("ContractViolationError",
                                              "invalid contract owner",
                                              contract.owner, "owner"));
    }
    if (!valid_contract_kind(contract.kind)) {
      result.diagnostics.push_back(diagnostic("ContractViolationError",
                                              "invalid contract kind",
                                              contract.owner, contract.kind));
    }
    if (contract.expression.empty()) {
      result.diagnostics.push_back(diagnostic("ContractViolationError",
                                              "contract expression is required",
                                              contract.owner, contract.kind));
    }
    const effect::EffectValidationResult effects =
        effect::validate_effect_summaries({effect::make_effect_summary(
            contract.owner, "contract", contract.effect_row,
            contract.effect_row, true)});
    for (const effect::EffectDiagnostic &diag : effects.diagnostics) {
      result.diagnostics.push_back(diagnostic(
          "ContractViolationError", diag.message, contract.owner, "effects"));
    }
  }
  for (PropertySpec property : properties) {
    property = normalize_property_spec(std::move(property));
    result.properties.push_back(property);
    if (property.name.empty()) {
      result.diagnostics.push_back(diagnostic(
          "ContractViolationError", "property name is required", {}, "name"));
    }
    if (property.seed == 0U) {
      result.diagnostics.push_back(diagnostic(
          "ContractViolationError", "property test must record a seed",
          property.name, "seed"));
    }
    if (property.generator.empty()) {
      result.diagnostics.push_back(diagnostic(
          "ContractViolationError", "property test generator is required",
          property.name, "generator"));
    }
    if (property.profile_set.empty()) {
      result.diagnostics.push_back(diagnostic(
          "ContractViolationError", "property test must record profile set",
          property.name, "profile_set"));
    }
  }
  std::sort(result.contracts.begin(), result.contracts.end(),
            [](const ContractSpec &left, const ContractSpec &right) {
              if (left.owner != right.owner) {
                return left.owner < right.owner;
              }
              return left.kind < right.kind;
            });
  std::sort(result.properties.begin(), result.properties.end(),
            [](const PropertySpec &left, const PropertySpec &right) {
              return left.name < right.name;
            });
  result.ok = result.diagnostics.empty();
  return result;
}

PrivacyValidationResult
validate_privacy_metadata(const std::vector<PrivacyLabel> &labels,
                          const std::vector<PrivacyPolicyRule> &policies,
                          const std::vector<LineageNode> &lineage) {
  PrivacyValidationResult result;
  std::set<std::string> label_names;
  for (PrivacyLabel label : labels) {
    label = normalize_privacy_label(std::move(label));
    result.labels.push_back(label);
    if (!valid_identifierish(label.name)) {
      result.diagnostics.push_back(diagnostic("PolicyViolationError",
                                              "invalid privacy label",
                                              label.name, "label"));
    }
    if (!label_names.insert(label.name).second) {
      result.diagnostics.push_back(diagnostic("PolicyViolationError",
                                              "duplicate privacy label",
                                              label.name, "label"));
    }
  }
  for (PrivacyPolicyRule rule : policies) {
    rule = normalize_privacy_policy_rule(std::move(rule));
    result.policies.push_back(rule);
    if (!valid_identifierish(rule.policy)) {
      result.diagnostics.push_back(diagnostic("PolicyViolationError",
                                              "invalid policy name",
                                              rule.policy, "policy"));
    }
    if (!valid_policy_action(rule.action)) {
      result.diagnostics.push_back(diagnostic("PolicyViolationError",
                                              "invalid policy action",
                                              rule.policy, rule.action));
    }
    if (rule.label.empty() && rule.aggregate.empty()) {
      result.diagnostics.push_back(diagnostic(
          "PolicyViolationError", "policy rule must name a label or aggregate",
          rule.policy, "rule"));
    }
    if (!rule.label.empty() &&
        label_names.find(rule.label) == label_names.end()) {
      result.diagnostics.push_back(diagnostic("PolicyViolationError",
                                              "policy references unknown label",
                                              rule.policy, rule.label));
    }
    if (!rule.aggregate.empty() && rule.min_group == 0U) {
      result.diagnostics.push_back(diagnostic(
          "PolicyViolationError", "aggregate policy requires min_group",
          rule.policy, rule.aggregate));
    }
  }
  for (LineageNode node : lineage) {
    node = normalize_lineage_node(std::move(node));
    result.lineage.push_back(node);
    if (!valid_identifierish(node.node_id)) {
      result.diagnostics.push_back(diagnostic("PolicyViolationError",
                                              "invalid lineage node id",
                                              node.node_id, "id"));
    }
    if (!valid_lineage_kind(node.kind)) {
      result.diagnostics.push_back(diagnostic("PolicyViolationError",
                                              "invalid lineage node kind",
                                              node.node_id, "kind"));
    }
    if (node.output.empty()) {
      result.diagnostics.push_back(diagnostic("PolicyViolationError",
                                              "lineage output is required",
                                              node.node_id, "output"));
    }
    for (const std::string &label : node.labels) {
      if (label_names.find(label) == label_names.end()) {
        result.diagnostics.push_back(diagnostic(
            "PolicyViolationError", "lineage references unknown label",
            node.node_id, label));
        continue;
      }
      if (node.kind == "export") {
        if (has_label_rule(result.policies, label, "deny")) {
          result.diagnostics.push_back(diagnostic(
              "PolicyViolationError", "sensitive export blocked by policy",
              node.node_id, label));
        } else if (!has_label_rule(result.policies, label, "redact") &&
                   !has_label_rule(result.policies, label, "allow")) {
          result.diagnostics.push_back(diagnostic(
              "PolicyViolationError",
              "sensitive export requires explicit policy or redaction",
              node.node_id, label));
        }
      }
    }
  }
  std::sort(result.labels.begin(), result.labels.end(),
            [](const PrivacyLabel &left, const PrivacyLabel &right) {
              return left.name < right.name;
            });
  std::sort(result.policies.begin(), result.policies.end(),
            [](const PrivacyPolicyRule &left, const PrivacyPolicyRule &right) {
              if (left.policy != right.policy) {
                return left.policy < right.policy;
              }
              return left.label < right.label;
            });
  std::sort(result.lineage.begin(), result.lineage.end(),
            [](const LineageNode &left, const LineageNode &right) {
              return left.node_id < right.node_id;
            });
  result.ok = result.diagnostics.empty();
  return result;
}

WorkflowValidationResult
validate_workflow_metadata(const std::vector<WorkflowStep> &steps,
                           const std::vector<WorkflowHistoryEvent> &history) {
  WorkflowValidationResult result;
  std::set<std::string> step_ids;
  for (WorkflowStep step : steps) {
    step = normalize_workflow_step(std::move(step));
    result.steps.push_back(step);
    const std::string step_id = step.workflow + "." + step.name;
    if (!valid_identifierish(step.workflow) ||
        !valid_identifierish(step.name)) {
      result.diagnostics.push_back(diagnostic(
          "WorkflowError", "invalid workflow step identity", step_id, "step"));
    }
    if (!step_ids.insert(step_id).second) {
      result.diagnostics.push_back(diagnostic(
          "WorkflowError", "duplicate workflow step", step_id, "step"));
    }
    const effect::EffectValidationResult effects =
        effect::validate_effect_summaries({effect::make_effect_summary(
            step_id, "workflow_step", step.effect_row, step.effect_row, true)});
    for (const effect::EffectDiagnostic &diag : effects.diagnostics) {
      result.diagnostics.push_back(
          diagnostic("WorkflowError", diag.message, step_id, "effects"));
    }
  }
  std::map<std::string, WorkflowHistoryEvent> committed_by_key;
  for (WorkflowHistoryEvent event : history) {
    event = normalize_workflow_history_event(std::move(event));
    result.history.push_back(event);
    if (event.workflow_id.empty() || event.workflow_version == 0U) {
      result.diagnostics.push_back(diagnostic(
          "WorkflowError", "workflow history id and version are required",
          event.workflow_id, "history"));
    }
    if (!valid_workflow_event(event.event)) {
      result.diagnostics.push_back(diagnostic("WorkflowError",
                                              "invalid workflow history event",
                                              event.workflow_id, event.event));
    }
    bool known_step = false;
    for (const WorkflowStep &step : result.steps) {
      if (step.name == event.step ||
          step.workflow + "." + step.name == event.step) {
        known_step = true;
        break;
      }
    }
    if (!known_step) {
      result.diagnostics.push_back(diagnostic(
          "WorkflowError", "history references unknown workflow step",
          event.workflow_id, event.step));
    }
    if (event.event == "commit") {
      if (event.idempotency_key.empty()) {
        result.diagnostics.push_back(diagnostic(
            "WorkflowError", "committed workflow step requires idempotency key",
            event.workflow_id, event.step));
      } else {
        const auto found = committed_by_key.find(event.idempotency_key);
        if (found != committed_by_key.end() &&
            (found->second.output_digest != event.output_digest ||
             found->second.input_digest != event.input_digest)) {
          result.diagnostics.push_back(diagnostic(
              "WorkflowError",
              "workflow step re-execution conflicts with committed idempotency "
              "key",
              event.workflow_id, event.idempotency_key));
        } else {
          committed_by_key.emplace(event.idempotency_key, event);
        }
      }
    }
  }
  std::sort(result.steps.begin(), result.steps.end(),
            [](const WorkflowStep &left, const WorkflowStep &right) {
              if (left.workflow != right.workflow) {
                return left.workflow < right.workflow;
              }
              return left.name < right.name;
            });
  std::sort(
      result.history.begin(), result.history.end(),
      [](const WorkflowHistoryEvent &left, const WorkflowHistoryEvent &right) {
        if (left.workflow_id != right.workflow_id) {
          return left.workflow_id < right.workflow_id;
        }
        if (left.workflow_version != right.workflow_version) {
          return left.workflow_version < right.workflow_version;
        }
        return left.step < right.step;
      });
  result.ok = result.diagnostics.empty();
  return result;
}

ModernDocumentParseResult parse_modern_document(const std::string &source) {
  ModernDocumentParseResult result;
  const std::map<std::string, std::string> values = parse_lines(source);

  std::uint32_t count = 0;
  read_count(values, "symbol.count", &count, &result.diagnostics, false);
  for (std::uint32_t i = 0; i < count; ++i) {
    result.document.symbols.push_back(
        parse_symbol(values, "symbol." + std::to_string(i)));
  }

  count = 0;
  read_count(values, "patch.count", &count, &result.diagnostics, false);
  for (std::uint32_t i = 0; i < count; ++i) {
    result.document.patches.push_back(
        parse_patch(values, "patch." + std::to_string(i), &result.diagnostics));
  }

  count = 0;
  read_count(values, "provenance.count", &count, &result.diagnostics, false);
  for (std::uint32_t i = 0; i < count; ++i) {
    result.document.provenance.push_back(
        parse_provenance(values, "provenance." + std::to_string(i)));
  }

  count = 0;
  read_count(values, "contract.count", &count, &result.diagnostics, false);
  for (std::uint32_t i = 0; i < count; ++i) {
    result.document.contracts.push_back(parse_contract(
        values, "contract." + std::to_string(i), &result.diagnostics));
  }

  count = 0;
  read_count(values, "property.count", &count, &result.diagnostics, false);
  for (std::uint32_t i = 0; i < count; ++i) {
    result.document.properties.push_back(
        parse_property(values, "property." + std::to_string(i)));
  }

  count = 0;
  read_count(values, "label.count", &count, &result.diagnostics, false);
  for (std::uint32_t i = 0; i < count; ++i) {
    result.document.privacy_labels.push_back(
        parse_label(values, "label." + std::to_string(i)));
  }

  count = 0;
  read_count(values, "policy.count", &count, &result.diagnostics, false);
  for (std::uint32_t i = 0; i < count; ++i) {
    result.document.privacy_policies.push_back(
        parse_policy(values, "policy." + std::to_string(i)));
  }

  count = 0;
  read_count(values, "lineage.count", &count, &result.diagnostics, false);
  for (std::uint32_t i = 0; i < count; ++i) {
    result.document.lineage.push_back(
        parse_lineage(values, "lineage." + std::to_string(i)));
  }

  count = 0;
  read_count(values, "workflow.step.count", &count, &result.diagnostics, false);
  for (std::uint32_t i = 0; i < count; ++i) {
    result.document.workflow_steps.push_back(parse_workflow_step(
        values, "workflow.step." + std::to_string(i), &result.diagnostics));
  }

  count = 0;
  read_count(values, "workflow.history.count", &count, &result.diagnostics,
             false);
  for (std::uint32_t i = 0; i < count; ++i) {
    result.document.workflow_history.push_back(parse_workflow_history(
        values, "workflow.history." + std::to_string(i)));
  }

  return result;
}

std::string agent_validation_to_json(const AgentValidationResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.agent_tooling.v1\",\n";
  out << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n";
  out << "  \"symbols\": [";
  for (std::size_t i = 0; i < result.symbols.size(); ++i) {
    const AgentSymbol &symbol = result.symbols[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"id\":\"" << json_escape(symbol.symbol_id)
        << "\",\"name\":\"" << json_escape(symbol.name) << "\",\"kind\":\""
        << json_escape(symbol.kind) << "\",\"module\":\""
        << json_escape(symbol.module) << "\",\"visibility\":\""
        << json_escape(symbol.visibility) << "\",\"source\":";
    emit_source_location(out, symbol.source);
    out << ",\"defined_in\":\"" << json_escape(symbol.defined_in)
        << "\",\"references\":";
    emit_source_array(out, symbol.references);
    out << ",\"type_summary\":\"" << json_escape(symbol.type_summary)
        << "\",\"effect_summary\":\"" << json_escape(symbol.effect_summary)
        << "\",\"schema_summary\":\"" << json_escape(symbol.schema_summary)
        << "\",\"doc_summary\":\"" << json_escape(symbol.doc_summary)
        << "\",\"flags\":" << symbol.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"patches\": [";
  for (std::size_t i = 0; i < result.patches.size(); ++i) {
    const AgentPatch &patch = result.patches[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"id\":\"" << json_escape(patch.patch_id)
        << "\",\"intent\":\"" << json_escape(patch.intent) << "\",\"tool\":\""
        << json_escape(patch.tool) << "\",\"request_digest\":\""
        << json_escape(patch.request_digest) << "\",\"capabilities\":";
    emit_string_array(out, patch.capabilities);
    out << ",\"operations\":[";
    for (std::size_t j = 0; j < patch.operations.size(); ++j) {
      const AgentPatchOperation &operation = patch.operations[j];
      if (j != 0U) {
        out << ",";
      }
      out << "{\"op\":\"" << json_escape(operation.op) << "\",\"symbol_id\":\""
          << json_escape(operation.symbol_id) << "\",\"new_name\":\""
          << json_escape(operation.new_name)
          << "\",\"flags\":" << operation.flags << "}";
    }
    out << "],\"flags\":" << patch.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"provenance\": [";
  for (std::size_t i = 0; i < result.provenance.size(); ++i) {
    const ProvenanceRecord &record = result.provenance[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"patch_id\":\"" << json_escape(record.patch_id)
        << "\",\"tool\":\"" << json_escape(record.tool)
        << "\",\"request_digest\":\"" << json_escape(record.request_digest)
        << "\",\"files_changed\":";
    emit_string_array(out, record.files_changed);
    out << ",\"symbols_changed\":";
    emit_string_array(out, record.symbols_changed);
    out << ",\"checks_run\":";
    emit_string_array(out, record.checks_run);
    out << ",\"artifact_digests\":";
    emit_string_array(out, record.artifact_digests);
    out << ",\"human_approval\":\"" << json_escape(record.human_approval)
        << "\",\"flags\":" << record.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    const ModernDiagnostic &diag = result.diagnostics[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error\":\"" << json_escape(diag.error_name)
        << "\",\"message\":\"" << json_escape(diag.message)
        << "\",\"subject\":\"" << json_escape(diag.subject) << "\",\"field\":\""
        << json_escape(diag.field) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

std::string
contract_validation_to_json(const ContractValidationResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.contracts.v1\",\n";
  out << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n";
  out << "  \"contracts\": [";
  for (std::size_t i = 0; i < result.contracts.size(); ++i) {
    const ContractSpec &contract = result.contracts[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"owner\":\"" << json_escape(contract.owner)
        << "\",\"kind\":\"" << json_escape(contract.kind) << "\",\"expr\":\""
        << json_escape(contract.expression) << "\",\"effects\":\""
        << json_escape(effect::effect_row_to_text(contract.effect_row))
        << "\",\"source\":";
    emit_source_location(out, contract.source);
    out << ",\"flags\":" << contract.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"properties\": [";
  for (std::size_t i = 0; i < result.properties.size(); ++i) {
    const PropertySpec &property = result.properties[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(property.name)
        << "\",\"owner\":\"" << json_escape(property.owner)
        << "\",\"seed\":" << property.seed << ",\"generator\":\""
        << json_escape(property.generator) << "\",\"shrinker_path\":\""
        << json_escape(property.shrinker_path) << "\",\"counterexample\":\""
        << json_escape(property.counterexample) << "\",\"profile_set\":";
    emit_string_array(out, property.profile_set);
    out << ",\"dependency_fingerprints\":";
    emit_string_array(out, property.dependency_fingerprints);
    out << ",\"flags\":" << property.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    const ModernDiagnostic &diag = result.diagnostics[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error\":\"" << json_escape(diag.error_name)
        << "\",\"message\":\"" << json_escape(diag.message)
        << "\",\"subject\":\"" << json_escape(diag.subject) << "\",\"field\":\""
        << json_escape(diag.field) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

std::string privacy_validation_to_json(const PrivacyValidationResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.privacy_lineage.v1\",\n";
  out << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n";
  out << "  \"labels\": [";
  for (std::size_t i = 0; i < result.labels.size(); ++i) {
    const PrivacyLabel &label = result.labels[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(label.name) << "\",\"kind\":\""
        << json_escape(label.kind) << "\",\"flags\":" << label.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"policies\": [";
  for (std::size_t i = 0; i < result.policies.size(); ++i) {
    const PrivacyPolicyRule &rule = result.policies[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"policy\":\"" << json_escape(rule.policy)
        << "\",\"action\":\"" << json_escape(rule.action) << "\",\"label\":\""
        << json_escape(rule.label) << "\",\"aggregate\":\""
        << json_escape(rule.aggregate) << "\",\"min_group\":" << rule.min_group
        << ",\"flags\":" << rule.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"lineage\": [";
  for (std::size_t i = 0; i < result.lineage.size(); ++i) {
    const LineageNode &node = result.lineage[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"id\":\"" << json_escape(node.node_id) << "\",\"kind\":\""
        << json_escape(node.kind) << "\",\"inputs\":";
    emit_string_array(out, node.inputs);
    out << ",\"output\":\"" << json_escape(node.output)
        << "\",\"schema_fingerprint\":\""
        << json_escape(node.schema_fingerprint) << "\",\"labels\":";
    emit_string_array(out, node.labels);
    out << ",\"source\":";
    emit_source_location(out, node.source);
    out << ",\"trace_span\":\"" << json_escape(node.trace_span)
        << "\",\"flags\":" << node.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    const ModernDiagnostic &diag = result.diagnostics[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error\":\"" << json_escape(diag.error_name)
        << "\",\"message\":\"" << json_escape(diag.message)
        << "\",\"subject\":\"" << json_escape(diag.subject) << "\",\"field\":\""
        << json_escape(diag.field) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

std::string
workflow_validation_to_json(const WorkflowValidationResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.workflow.v1\",\n";
  out << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n";
  out << "  \"steps\": [";
  for (std::size_t i = 0; i < result.steps.size(); ++i) {
    const WorkflowStep &step = result.steps[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"workflow\":\"" << json_escape(step.workflow)
        << "\",\"name\":\"" << json_escape(step.name) << "\",\"effects\":\""
        << json_escape(effect::effect_row_to_text(step.effect_row))
        << "\",\"depends_on\":";
    emit_string_array(out, step.depends_on);
    out << ",\"retry\":\"" << json_escape(step.retry_policy)
        << "\",\"idempotency_key\":\"" << json_escape(step.idempotency_key)
        << "\",\"timeout_ms\":" << step.timeout_ms
        << ",\"flags\":" << step.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"history\": [";
  for (std::size_t i = 0; i < result.history.size(); ++i) {
    const WorkflowHistoryEvent &event = result.history[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"workflow_id\":\"" << json_escape(event.workflow_id)
        << "\",\"version\":" << event.workflow_version << ",\"step\":\""
        << json_escape(event.step) << "\",\"event\":\""
        << json_escape(event.event) << "\",\"input_digest\":\""
        << json_escape(event.input_digest) << "\",\"output_digest\":\""
        << json_escape(event.output_digest) << "\",\"schema_version\":\""
        << json_escape(event.schema_version) << "\",\"effect_grants\":";
    emit_string_array(out, event.effect_grants);
    out << ",\"idempotency_key\":\"" << json_escape(event.idempotency_key)
        << "\",\"trace_id\":\"" << json_escape(event.trace_id)
        << "\",\"flags\":" << event.flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    const ModernDiagnostic &diag = result.diagnostics[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error\":\"" << json_escape(diag.error_name)
        << "\",\"message\":\"" << json_escape(diag.message)
        << "\",\"subject\":\"" << json_escape(diag.subject) << "\",\"field\":\""
        << json_escape(diag.field) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

std::string explain_result_to_json(const AgentValidationResult &symbols,
                                   const SourceLocation &source) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.explain.v1\",\n";
  out << "  \"query\": ";
  emit_source_location(out, source);
  out << ",\n";
  out << "  \"matches\": [";
  bool first = true;
  for (const AgentSymbol &symbol : symbols.symbols) {
    bool matches = symbol.source.file == source.file &&
                   symbol.source.line == source.line &&
                   symbol.source.column <= source.column;
    for (const SourceLocation &ref : symbol.references) {
      matches =
          matches || (ref.file == source.file && ref.line == source.line &&
                      ref.column == source.column);
    }
    if (!matches) {
      continue;
    }
    if (!first) {
      out << ",";
    }
    first = false;
    out << "\n    {\"symbol_id\":\"" << json_escape(symbol.symbol_id)
        << "\",\"name\":\"" << json_escape(symbol.name) << "\",\"kind\":\""
        << json_escape(symbol.kind) << "\",\"binding_resolution\":\""
        << json_escape(symbol.symbol_id) << "\",\"type_info\":\""
        << json_escape(symbol.type_summary) << "\",\"effect_info\":\""
        << json_escape(symbol.effect_summary) << "\",\"schema_info\":\""
        << json_escape(symbol.schema_summary)
        << "\",\"profile_requirements\":[\"agent_tooling\"],"
        << "\"possible_refactor_actions\":[\"rename\"]}";
  }
  out << "\n  ],\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < symbols.diagnostics.size(); ++i) {
    const ModernDiagnostic &diag = symbols.diagnostics[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error\":\"" << json_escape(diag.error_name)
        << "\",\"message\":\"" << json_escape(diag.message)
        << "\",\"subject\":\"" << json_escape(diag.subject) << "\",\"field\":\""
        << json_escape(diag.field) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::modern
