#pragma once

#include "frontend/lexer/token.h"

#include <cstdint>
#include <string>
#include <vector>

namespace amber::modern {

inline constexpr std::uint32_t kAgentPatchFlagChecked = 0x1U;
inline constexpr std::uint32_t kAgentPatchFlagApplied = 0x2U;
inline constexpr std::uint32_t kContractFlagStrict = 0x1U;
inline constexpr std::uint32_t kPropertyFlagReplayable = 0x1U;
inline constexpr std::uint32_t kPrivacyLabelFlagSensitive = 0x1U;
inline constexpr std::uint32_t kWorkflowStepFlagIdempotent = 0x1U;

struct SourceLocation {
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

struct AgentSymbol {
  std::string symbol_id;
  std::string name;
  std::string kind;
  std::string module;
  std::string visibility;
  SourceLocation source;
  std::string defined_in;
  std::vector<SourceLocation> references;
  std::string type_summary;
  std::string effect_summary;
  std::string schema_summary;
  std::string doc_summary;
  std::uint32_t flags = 0;
};

struct AgentPatchOperation {
  std::string op;
  std::string symbol_id;
  std::string new_name;
  std::uint32_t flags = 0;
};

struct AgentPatch {
  std::string patch_id;
  std::string intent;
  std::string tool;
  std::string request_digest;
  std::vector<std::string> capabilities;
  std::vector<AgentPatchOperation> operations;
  std::uint32_t flags = 0;
};

struct ProvenanceRecord {
  std::string patch_id;
  std::string tool;
  std::string request_digest;
  std::vector<std::string> files_changed;
  std::vector<std::string> symbols_changed;
  std::vector<std::string> diagnostics_before;
  std::vector<std::string> diagnostics_after;
  std::vector<std::string> checks_run;
  std::vector<std::string> artifact_digests;
  std::string human_approval;
  std::uint32_t flags = 0;
};

struct ContractSpec {
  std::string owner;
  std::string kind;
  std::string expression;
  std::vector<std::string> effect_row;
  SourceLocation source;
  std::uint32_t flags = 0;
};

struct PropertySpec {
  std::string name;
  std::string owner;
  std::uint64_t seed = 0;
  std::string generator;
  std::string shrinker_path;
  std::string counterexample;
  std::vector<std::string> profile_set;
  std::vector<std::string> dependency_fingerprints;
  std::uint32_t flags = 0;
};

struct PrivacyLabel {
  std::string name;
  std::string kind;
  std::uint32_t flags = 0;
};

struct PrivacyPolicyRule {
  std::string policy;
  std::string action;
  std::string label;
  std::string aggregate;
  std::uint32_t min_group = 0;
  std::uint32_t flags = 0;
};

struct LineageNode {
  std::string node_id;
  std::string kind;
  std::vector<std::string> inputs;
  std::string output;
  std::string schema_fingerprint;
  std::vector<std::string> labels;
  SourceLocation source;
  std::string trace_span;
  std::uint32_t flags = 0;
};

struct WorkflowStep {
  std::string workflow;
  std::string name;
  std::vector<std::string> effect_row;
  std::vector<std::string> depends_on;
  std::string retry_policy;
  std::string idempotency_key;
  std::uint32_t timeout_ms = 0;
  std::uint32_t flags = 0;
};

struct WorkflowHistoryEvent {
  std::string workflow_id;
  std::uint32_t workflow_version = 0;
  std::string step;
  std::string event;
  std::string input_digest;
  std::string output_digest;
  std::string schema_version;
  std::vector<std::string> effect_grants;
  std::string idempotency_key;
  std::string trace_id;
  std::uint32_t flags = 0;
};

struct ModernDiagnostic {
  std::string error_name;
  std::string message;
  std::string subject;
  std::string field;
};

struct ModernDocument {
  std::vector<AgentSymbol> symbols;
  std::vector<AgentPatch> patches;
  std::vector<ProvenanceRecord> provenance;
  std::vector<ContractSpec> contracts;
  std::vector<PropertySpec> properties;
  std::vector<PrivacyLabel> privacy_labels;
  std::vector<PrivacyPolicyRule> privacy_policies;
  std::vector<LineageNode> lineage;
  std::vector<WorkflowStep> workflow_steps;
  std::vector<WorkflowHistoryEvent> workflow_history;
};

struct ModernDocumentParseResult {
  ModernDocument document;
  std::vector<ModernDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

struct AgentValidationResult {
  bool ok = false;
  std::vector<AgentSymbol> symbols;
  std::vector<AgentPatch> patches;
  std::vector<ProvenanceRecord> provenance;
  std::vector<ModernDiagnostic> diagnostics;
};

struct ContractValidationResult {
  bool ok = false;
  std::vector<ContractSpec> contracts;
  std::vector<PropertySpec> properties;
  std::vector<ModernDiagnostic> diagnostics;
};

struct PrivacyValidationResult {
  bool ok = false;
  std::vector<PrivacyLabel> labels;
  std::vector<PrivacyPolicyRule> policies;
  std::vector<LineageNode> lineage;
  std::vector<ModernDiagnostic> diagnostics;
};

struct WorkflowValidationResult {
  bool ok = false;
  std::vector<WorkflowStep> steps;
  std::vector<WorkflowHistoryEvent> history;
  std::vector<ModernDiagnostic> diagnostics;
};

bool valid_symbol_kind(const std::string &kind);
bool valid_visibility(const std::string &visibility);
bool valid_patch_operation(const std::string &op);
bool valid_contract_kind(const std::string &kind);
bool valid_lineage_kind(const std::string &kind);
bool valid_policy_action(const std::string &action);
bool valid_workflow_event(const std::string &event);

SourceLocation source_from_span(const lexer::Span &span);
std::string source_location_to_text(const SourceLocation &source);

AgentSymbol normalize_agent_symbol(AgentSymbol symbol);
AgentPatch normalize_agent_patch(AgentPatch patch);
ProvenanceRecord normalize_provenance_record(ProvenanceRecord record);
ContractSpec normalize_contract_spec(ContractSpec contract);
PropertySpec normalize_property_spec(PropertySpec property);
PrivacyLabel normalize_privacy_label(PrivacyLabel label);
PrivacyPolicyRule normalize_privacy_policy_rule(PrivacyPolicyRule rule);
LineageNode normalize_lineage_node(LineageNode node);
WorkflowStep normalize_workflow_step(WorkflowStep step);
WorkflowHistoryEvent
normalize_workflow_history_event(WorkflowHistoryEvent event);

AgentValidationResult
validate_agent_metadata(const std::vector<AgentSymbol> &symbols,
                        const std::vector<AgentPatch> &patches = {},
                        const std::vector<ProvenanceRecord> &provenance = {});
ContractValidationResult
validate_contract_metadata(const std::vector<ContractSpec> &contracts,
                           const std::vector<PropertySpec> &properties = {});
PrivacyValidationResult
validate_privacy_metadata(const std::vector<PrivacyLabel> &labels,
                          const std::vector<PrivacyPolicyRule> &policies = {},
                          const std::vector<LineageNode> &lineage = {});
WorkflowValidationResult validate_workflow_metadata(
    const std::vector<WorkflowStep> &steps,
    const std::vector<WorkflowHistoryEvent> &history = {});

ModernDocumentParseResult parse_modern_document(const std::string &source);

std::string agent_validation_to_json(const AgentValidationResult &result);
std::string contract_validation_to_json(const ContractValidationResult &result);
std::string privacy_validation_to_json(const PrivacyValidationResult &result);
std::string workflow_validation_to_json(const WorkflowValidationResult &result);
std::string explain_result_to_json(const AgentValidationResult &symbols,
                                   const SourceLocation &source);

} // namespace amber::modern
