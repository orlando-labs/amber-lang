#include "profile/modern.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "modern profile test failed: " << message << "\n";
    std::exit(1);
  }
}

void test_agent_patch_and_provenance_document() {
  const std::string source = "schema: amber.modern_profiles.v1\n"
                             "symbol.count=1\n"
                             "symbol.0.id=main::customer_id\n"
                             "symbol.0.name=customer_id\n"
                             "symbol.0.kind=local\n"
                             "symbol.0.module=main\n"
                             "symbol.0.visibility=internal\n"
                             "symbol.0.source=customer.am:1:5\n"
                             "patch.count=1\n"
                             "patch.0.id=patch.rename_customer\n"
                             "patch.0.intent=rename_symbol\n"
                             "patch.0.tool=agent-name\n"
                             "patch.0.request_digest=abc123\n"
                             "patch.0.capabilities=refactor,format\n"
                             "patch.0.operation.count=1\n"
                             "patch.0.operation.0.op=rename\n"
                             "patch.0.operation.0.symbol_id=main::customer_id\n"
                             "patch.0.operation.0.new_name=account_id\n"
                             "provenance.count=1\n"
                             "provenance.0.patch_id=patch.rename_customer\n"
                             "provenance.0.tool=agent-name\n"
                             "provenance.0.request_digest=abc123\n"
                             "provenance.0.files_changed=customer.am\n"
                             "provenance.0.symbols_changed=main::customer_id\n"
                             "provenance.0.checks_run=bind,typed\n";
  const amber::modern::ModernDocumentParseResult parsed =
      amber::modern::parse_modern_document(source);
  expect(parsed.ok(), "agent document should parse");
  const amber::modern::AgentValidationResult validated =
      amber::modern::validate_agent_metadata(parsed.document.symbols,
                                             parsed.document.patches,
                                             parsed.document.provenance);
  expect(validated.ok, "agent patch/provenance should validate");
  expect(validated.symbols[0].references.empty(),
         "symbol references default to empty");
  const std::string json = amber::modern::agent_validation_to_json(validated);
  expect(json.find("\"schema\": \"amber.agent_tooling.v1\"") !=
             std::string::npos,
         "agent JSON schema should be exposed");
}

void test_agent_patch_rejects_stale_symbol() {
  const std::string source = "schema: amber.modern_profiles.v1\n"
                             "symbol.count=1\n"
                             "symbol.0.id=main::live\n"
                             "symbol.0.name=live\n"
                             "symbol.0.kind=local\n"
                             "symbol.0.module=main\n"
                             "patch.count=1\n"
                             "patch.0.id=patch.bad\n"
                             "patch.0.intent=rename_symbol\n"
                             "patch.0.tool=agent-name\n"
                             "patch.0.request_digest=abc123\n"
                             "patch.0.operation.count=1\n"
                             "patch.0.operation.0.op=rename\n"
                             "patch.0.operation.0.symbol_id=main::stale\n"
                             "patch.0.operation.0.new_name=next\n";
  const amber::modern::ModernDocumentParseResult parsed =
      amber::modern::parse_modern_document(source);
  const amber::modern::AgentValidationResult validated =
      amber::modern::validate_agent_metadata(parsed.document.symbols,
                                             parsed.document.patches);
  expect(!validated.ok, "stale symbol patch should be rejected");
  expect(!validated.diagnostics.empty() &&
             validated.diagnostics[0].error_name == "AgentPatchError",
         "stale symbol rejection should be an AgentPatchError");
}

void test_contract_property_document() {
  const std::string source = "schema: amber.modern_profiles.v1\n"
                             "contract.count=2\n"
                             "contract.0.owner=Account.withdraw\n"
                             "contract.0.kind=require\n"
                             "contract.0.expr=amount > 0\n"
                             "contract.1.owner=Account.withdraw\n"
                             "contract.1.kind=ensure\n"
                             "contract.1.expr=result.balance >= 0\n"
                             "contract.1.effects=!{mut}\n"
                             "property.count=1\n"
                             "property.0.name=reverse_twice\n"
                             "property.0.owner=Array\n"
                             "property.0.seed=42\n"
                             "property.0.generator=Array[Int]\n"
                             "property.0.profiles=contracts,replay\n"
                             "property.0.dependency_fingerprints=abc\n";
  const amber::modern::ModernDocumentParseResult parsed =
      amber::modern::parse_modern_document(source);
  const amber::modern::ContractValidationResult validated =
      amber::modern::validate_contract_metadata(parsed.document.contracts,
                                                parsed.document.properties);
  expect(validated.ok, "contract/property metadata should validate");
  expect(validated.properties[0].seed == 42,
         "property seed should be preserved");
}

void test_privacy_export_policy_rejection() {
  const std::string source = "schema: amber.modern_profiles.v1\n"
                             "label.count=1\n"
                             "label.0.name=pii\n"
                             "label.0.kind=pii\n"
                             "policy.count=1\n"
                             "policy.0.policy=PublicExport\n"
                             "policy.0.action=deny\n"
                             "policy.0.label=pii\n"
                             "lineage.count=1\n"
                             "lineage.0.id=export.users\n"
                             "lineage.0.kind=export\n"
                             "lineage.0.output=public.csv\n"
                             "lineage.0.labels=pii\n";
  const amber::modern::ModernDocumentParseResult parsed =
      amber::modern::parse_modern_document(source);
  const amber::modern::PrivacyValidationResult validated =
      amber::modern::validate_privacy_metadata(parsed.document.privacy_labels,
                                               parsed.document.privacy_policies,
                                               parsed.document.lineage);
  expect(!validated.ok, "PII export with deny policy should be rejected");
  expect(!validated.diagnostics.empty() &&
             validated.diagnostics[0].error_name == "PolicyViolationError",
         "privacy rejection should be a PolicyViolationError");
}

void test_workflow_idempotency_conflict() {
  const std::string source = "schema: amber.modern_profiles.v1\n"
                             "workflow.step.count=1\n"
                             "workflow.step.0.workflow=ImportOrders\n"
                             "workflow.step.0.name=commit\n"
                             "workflow.step.0.effects=!{db}\n"
                             "workflow.step.0.idempotency_key=batch-1\n"
                             "workflow.history.count=2\n"
                             "workflow.history.0.workflow_id=ImportOrders/1\n"
                             "workflow.history.0.version=1\n"
                             "workflow.history.0.step=commit\n"
                             "workflow.history.0.event=commit\n"
                             "workflow.history.0.input_digest=in-a\n"
                             "workflow.history.0.output_digest=out-a\n"
                             "workflow.history.0.idempotency_key=batch-1\n"
                             "workflow.history.1.workflow_id=ImportOrders/1\n"
                             "workflow.history.1.version=1\n"
                             "workflow.history.1.step=commit\n"
                             "workflow.history.1.event=commit\n"
                             "workflow.history.1.input_digest=in-b\n"
                             "workflow.history.1.output_digest=out-b\n"
                             "workflow.history.1.idempotency_key=batch-1\n";
  const amber::modern::ModernDocumentParseResult parsed =
      amber::modern::parse_modern_document(source);
  const amber::modern::WorkflowValidationResult validated =
      amber::modern::validate_workflow_metadata(
          parsed.document.workflow_steps, parsed.document.workflow_history);
  expect(!validated.ok, "conflicting committed workflow key should fail");
  expect(!validated.diagnostics.empty() &&
             validated.diagnostics[0].error_name == "WorkflowError",
         "workflow conflict should be a WorkflowError");
}

} // namespace

int main() {
  test_agent_patch_and_provenance_document();
  test_agent_patch_rejects_stale_symbol();
  test_contract_property_document();
  test_privacy_export_policy_rejection();
  test_workflow_idempotency_conflict();
  std::cout << "modern_profile_tests: ok\n";
  return 0;
}
