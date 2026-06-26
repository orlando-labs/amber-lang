#pragma once

#include "bytecode/format.h"
#include "package/package.h"
#include "profile/capabilities.h"
#include "runtime/heap.h"
#include "runtime/value.h"
#include "runtime/watch.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace amber::runtime {

enum class MethodTableSide { Instance, Class };
enum class RuntimeWorldState { Open, Frozen };
enum class RuntimeOwnerKind { Class, Mixin };

struct RuntimeWorldTransaction {
  RuntimeOwnerKind target_kind = RuntimeOwnerKind::Class;
  std::uint32_t target_index = 0;
  bool has_superclass_ref = false;
  std::uint32_t superclass_ref = 0;
  std::vector<bytecode::BcMethod> instance_methods;
  std::vector<bytecode::BcMethod> class_methods;
  std::vector<std::uint32_t> include_indices;
  std::vector<std::uint32_t> extend_indices;
};

struct RuntimeMirrorSourceLocation {
  bool present = false;
  std::uint32_t code_id = 0;
  std::uint32_t pc = 0;
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

struct RuntimeMethodMirror {
  bool read_only = true;
  std::uint32_t selector_symbol_id = 0;
  std::string selector;
  std::uint32_t owner_index = 0;
  std::string owner_name;
  RuntimeOwnerKind owner_kind = RuntimeOwnerKind::Class;
  MethodTableSide side = MethodTableSide::Instance;
  std::uint32_t signature_blob_id = 0;
  std::uint32_t entry_code_id = 0;
  std::uint32_t flags = 0;
  std::size_t parameter_count = 0;
  std::size_t default_count = 0;
  std::size_t type_hook_count = 0;
  std::size_t clause_count = 0;
  RuntimeMirrorSourceLocation source_location;
};

struct RuntimeDirectMixinMirror {
  std::uint32_t index = 0;
  std::string name;
  bool dynamic = false;
};

struct RuntimePackageDependencyMirror {
  std::string module_name;
  bytecode::Version required_format;
  bytecode::Version min_language_version;
  bool has_max_language_version = false;
  bytecode::Version max_language_version;
  bool has_abi_requirement = false;
};

struct RuntimePackageExportMirror {
  std::string public_name;
  std::string target_kind;
  std::uint32_t target_index = 0;
  std::uint32_t visibility_flags = 0;
  bool has_reexport = false;
  std::string reexport_module_name;
};

struct RuntimePackageAttrMirror {
  std::string key;
  std::string value;
};

using RuntimeCapabilityGrant = capability::CapabilityRequest;
using RuntimeCapabilityResolution = capability::CapabilityResolutionResult;
using RuntimeEffectSummary = effect::EffectSummary;
using RuntimeEffectValidation = effect::EffectValidationResult;
using RuntimeObservabilitySite = replay::ObservabilitySite;
using RuntimeReplayMetadata = replay::ReplayMetadata;
using RuntimeTraceEvent = replay::TraceEvent;
using RuntimeReplayTrace = replay::ReplayTrace;
using RuntimeReplayValidation = replay::ReplayValidationResult;
using RuntimeSchemaDefinition = data::SchemaDefinition;
using RuntimeSchemaMigration = data::SchemaMigration;
using RuntimeTablePlan = data::TablePlan;
using RuntimeSchemaValidation = data::SchemaValidationResult;
using RuntimeTablePlanValidation = data::TablePlanValidationResult;
using RuntimeWasmComponent = wasm_accel::WasmComponent;
using RuntimeAcceleratorKernel = wasm_accel::AcceleratorKernel;
using RuntimeWasmValidation = wasm_accel::WasmComponentValidationResult;
using RuntimeAcceleratorValidation = wasm_accel::AcceleratorValidationResult;
using RuntimeAgentSymbol = modern::AgentSymbol;
using RuntimeAgentValidation = modern::AgentValidationResult;
using RuntimeContractSpec = modern::ContractSpec;
using RuntimeContractValidation = modern::ContractValidationResult;
using RuntimePrivacyLabel = modern::PrivacyLabel;
using RuntimePrivacyValidation = modern::PrivacyValidationResult;
using RuntimeWorkflowStep = modern::WorkflowStep;
using RuntimeWorkflowValidation = modern::WorkflowValidationResult;

struct RuntimePackageMirror {
  bool read_only = true;
  std::string name;
  bytecode::Version format_version;
  bytecode::Version language_version;
  std::uint32_t profile_flags = 0;
  std::uint32_t file_flags = 0;
  bool has_init = false;
  std::uint32_t init_code_id = 0;
  std::vector<RuntimePackageDependencyMirror> dependencies;
  std::vector<RuntimePackageExportMirror> exports;
  std::vector<RuntimePackageAttrMirror> attrs;
  std::vector<RuntimeCapabilityGrant> capabilities;
  std::vector<RuntimeEffectSummary> effects;
  std::vector<RuntimeObservabilitySite> observability_sites;
  RuntimeReplayMetadata replay_metadata;
  std::vector<RuntimeSchemaDefinition> schemas;
  std::vector<RuntimeSchemaMigration> schema_migrations;
  std::vector<RuntimeTablePlan> table_plans;
  std::vector<RuntimeWasmComponent> wasm_components;
  std::vector<RuntimeAcceleratorKernel> accelerator_kernels;
  std::vector<RuntimeAgentSymbol> agent_symbols;
  std::vector<RuntimeContractSpec> contracts;
  std::vector<RuntimePrivacyLabel> privacy_labels;
  std::vector<RuntimeWorkflowStep> workflow_steps;
};

struct RuntimeOwnerMirror {
  bool read_only = true;
  std::uint32_t index = 0;
  std::string name;
  RuntimeOwnerKind kind = RuntimeOwnerKind::Class;
  std::uint32_t owner_flags = 0;
  std::uint32_t ivar_schema_id = 0;
  bool has_superclass = false;
  std::uint32_t superclass_index = 0;
  std::string superclass_name;
  std::uint64_t method_version = 0;
  std::uint64_t world_epoch = 0;
  RuntimeMirrorSourceLocation source_location;
  std::vector<RuntimeDirectMixinMirror> direct_includes;
  std::vector<RuntimeDirectMixinMirror> direct_extends;
  std::vector<RuntimeMethodMirror> instance_methods;
  std::vector<RuntimeMethodMirror> class_methods;
};

struct RuntimeWorldMirror {
  bool read_only = true;
  RuntimeWorldState state = RuntimeWorldState::Open;
  std::uint64_t world_epoch = 0;
  std::uint64_t watch_epoch = 0;
  RuntimePackageMirror package;
  std::vector<RuntimeOwnerMirror> owners;
};

struct RuntimeDispatchCacheStats {
  std::uint64_t call_cache_entries = 0;
  std::uint64_t call_cache_hits = 0;
  std::uint64_t call_cache_misses = 0;
  std::uint64_t call_cache_updates = 0;
};

struct RuntimePackageReloadDiagnostic {
  std::string error_name;
  std::string message;
  std::string module_name;
};

struct RuntimePackageReloadResult {
  bool ok = false;
  bool swapped = false;
  std::string package_name;
  std::string previous_version;
  std::string new_version;
  std::string root_module;
  std::uint64_t previous_world_epoch = 0;
  std::uint64_t new_world_epoch = 0;
  std::vector<RuntimePackageReloadDiagnostic> diagnostics;
};

struct RuntimeCapabilityCheckResult {
  bool ok = false;
  std::string error_name;
  std::string message;
  std::string capability;
  std::string target;
};

struct RuntimeEffectCheckResult {
  bool ok = false;
  std::string error_name;
  std::string message;
  std::vector<std::string> effects;
};

struct RuntimeIoProviderStatus {
  bool handled = false;
  bool ok = false;
  bool boolean = false;
  std::uint64_t size = 0;
  bool file = false;
  bool directory = false;
  bool symlink = false;
  std::size_t count = 0;
  std::string bytes;
  std::string error_name;
  std::string message;
};

class RuntimeIoProvider {
public:
  virtual ~RuntimeIoProvider() = default;

  virtual RuntimeIoProviderStatus fs_exists(const std::string &path);
  virtual RuntimeIoProviderStatus fs_file(const std::string &path);
  virtual RuntimeIoProviderStatus fs_dir(const std::string &path);
  virtual RuntimeIoProviderStatus fs_metadata(const std::string &path);
  virtual RuntimeIoProviderStatus
  fs_read_bytes(const std::string &path, std::optional<std::size_t> limit);
  virtual RuntimeIoProviderStatus fs_write_bytes(const std::string &path,
                                                 const std::string &bytes,
                                                 bool create, bool truncate,
                                                 bool append = false);
  virtual RuntimeIoProviderStatus fs_mkdir(const std::string &path);
  virtual RuntimeIoProviderStatus fs_mkdir_p(const std::string &path);
  virtual RuntimeIoProviderStatus fs_remove(const std::string &path);
  virtual RuntimeIoProviderStatus fs_rename(const std::string &from,
                                            const std::string &to);
  virtual RuntimeIoProviderStatus fs_copy(const std::string &from,
                                          const std::string &to);
};

struct RuntimeWorldOptions {
  std::vector<RuntimeCapabilityGrant> capability_grants;
  std::vector<std::string> allowed_effects;
  bool enforce_effects = false;
  bool record_replay_trace = false;
  bool enforce_replay = false;
  std::string trace_id;
  std::uint64_t virtual_time_start = 1;
  std::uint64_t virtual_time_step = 1;
  RuntimeReplayTrace expected_replay;
  std::shared_ptr<RuntimeIoProvider> io_provider;
};

struct TraceFrame {
  std::string module_id;
  std::uint32_t code_id = 0;
  std::uint32_t pc = 0;
  std::string file;
  std::uint32_t byte_start = 0;
  std::uint32_t byte_end = 0;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::uint32_t line_end = 0;
  std::uint32_t column_end = 0;
  std::string generated_kind;
};

struct Fault {
  Fault() = default;
  Fault(std::string error_name, std::string message, std::uint32_t code_id,
        std::uint32_t pc)
      : error_name(std::move(error_name)), message(std::move(message)),
        code_id(code_id), pc(pc) {}

  std::string error_name;
  std::string message;
  std::uint32_t code_id = 0;
  std::uint32_t pc = 0;
  std::vector<TraceFrame> trace;
  std::string trace_text;
};

struct ExecutionLocal {
  std::uint32_t slot = 0;
  std::string name;
  std::string role;
  std::string binding_kind;
  bool initialized = false;
  bool watched = false;
  std::uint64_t watch_cell_id = 0;
  std::uint64_t watch_revision = 0;
  Value value = Value::null();
};

struct ExecutionResult {
  ExecutionResult() = default;
  ExecutionResult(Value result_value, std::optional<Fault> result_fault,
                  std::vector<ExecutionLocal> result_locals = {},
                  std::vector<RuntimeWatchEvent> result_watch_events = {},
                  std::uint64_t result_watch_epoch = 0,
                  std::vector<std::string> result_runtime_strings = {},
                  std::vector<std::string> result_runtime_symbols = {})
      : value(std::move(result_value)), fault(std::move(result_fault)),
        locals(std::move(result_locals)),
        watch_events(std::move(result_watch_events)),
        watch_epoch(result_watch_epoch),
        runtime_strings(std::move(result_runtime_strings)),
        runtime_symbols(std::move(result_runtime_symbols)) {}

  Value value = Value::null();
  std::optional<Fault> fault;
  std::vector<ExecutionLocal> locals;
  std::vector<RuntimeWatchEvent> watch_events;
  std::uint64_t watch_epoch = 0;
  std::vector<std::string> runtime_strings;
  std::vector<std::string> runtime_symbols;

  bool ok() const { return !fault.has_value(); }
};

class RuntimeWorld {
public:
  explicit RuntimeWorld(const bytecode::BcModule &module);
  RuntimeWorld(const bytecode::BcModule &module, RuntimeWorldOptions options);
  explicit RuntimeWorld(const pkg::PackageArtifact &artifact);
  RuntimeWorld(const pkg::PackageArtifact &artifact,
               RuntimeWorldOptions options);
  ~RuntimeWorld();

  ExecutionResult execute(std::uint32_t code_id,
                          const std::vector<Value> &args = {},
                          Value self = Value::null(),
                          Value block = Value::null());

  ExecutionResult define_instance_method(std::uint32_t class_index,
                                         bytecode::BcMethod method);
  ExecutionResult define_class_method(std::uint32_t class_index,
                                      bytecode::BcMethod method);
  ExecutionResult include_mixin(std::uint32_t class_index,
                                std::uint32_t mixin_index);
  ExecutionResult extend_mixin(std::uint32_t class_index,
                               std::uint32_t mixin_index);
  ExecutionResult commit_transaction(const RuntimeWorldTransaction &tx);
  ExecutionResult freeze_world();
  RuntimePackageReloadResult
  reload_package_artifact(const pkg::PackageArtifact &artifact);
  RuntimeCapabilityCheckResult
  check_capability(const std::string &capability,
                   const std::string &target = {}) const;
  RuntimeCapabilityResolution capability_resolution() const;
  RuntimeEffectCheckResult
  check_effects(const std::vector<std::string> &effects) const;
  RuntimeEffectValidation effect_validation() const;
  RuntimeSchemaValidation schema_validation() const;
  RuntimeTablePlanValidation table_plan_validation() const;
  RuntimeWasmValidation wasm_validation() const;
  RuntimeAcceleratorValidation accelerator_validation() const;
  RuntimeAgentValidation agent_validation() const;
  RuntimeContractValidation contract_validation() const;
  RuntimePrivacyValidation privacy_validation() const;
  RuntimeWorkflowValidation workflow_validation() const;
  RuntimeTraceEvent record_trace_event(RuntimeTraceEvent event);
  RuntimeReplayTrace replay_trace() const;
  RuntimeReplayValidation replay_validation() const;

  std::uint64_t world_epoch() const;
  std::uint64_t watch_epoch() const;
  std::vector<RuntimeWatchEvent> watch_events() const;
  void begin_dependency_capture(std::uint64_t notebook_cell_id);
  RuntimeDependencySet end_dependency_capture();
  RuntimeDependencySet dependency_capture_snapshot() const;
  RuntimeWorldState world_state() const;
  bool is_world_frozen() const;
  std::uint64_t method_version(std::uint32_t class_index) const;
  std::size_t method_table_size(std::uint32_t class_index,
                                MethodTableSide side) const;
  RuntimePackageMirror package_mirror() const;
  std::optional<RuntimeOwnerMirror>
  owner_mirror(std::uint32_t owner_index) const;
  std::optional<RuntimeOwnerMirror>
  class_mirror(std::uint32_t class_index) const;
  std::optional<RuntimeOwnerMirror>
  mixin_mirror(std::uint32_t mixin_index) const;
  RuntimeWorldMirror world_mirror() const;
  RuntimeDispatchCacheStats dispatch_cache_stats() const;
  RuntimeHeapStats heap_stats() const;
  std::uint64_t drain_remote_frees();
  std::uint64_t drain_remote_frees(std::uint64_t worker_id);
  RuntimeWriteBarrierResult write_barrier(const Value &owner,
                                          const Value &value);
  RuntimeGcResult collect_garbage(const std::vector<Value> &roots = {},
                                  RuntimeGcCycle cycle = RuntimeGcCycle::Full);
  void request_garbage_collection(RuntimeGcCycle cycle = RuntimeGcCycle::Full);
  RuntimePinResult
  pin(const Value &value,
      RuntimePinViewKind view_kind = RuntimePinViewKind::Opaque,
      RuntimePinPermission permissions = RuntimePinPermission::ReadOnly);
  RuntimeUnpinResult unpin(RuntimePinToken *token);
  std::uint64_t pin_count(const Value &value) const;
  bool is_pinned(const Value &value) const;
  RuntimeOpaqueHandleResult opaque_handle_for(const RuntimePinToken &token);
  RuntimeOpaqueHandleResult release_opaque_handle(RuntimeOpaqueHandle *handle);
  RuntimeOpaqueHandleResult
  resolve_opaque_handle(const RuntimeOpaqueHandle &handle) const;
  RuntimeValueBufferViewResult value_buffer_view(const RuntimePinToken &token);
  RuntimeNativeWaitResult register_native_wait(const RuntimePinToken &token);
  RuntimeNativeWaitResult cancel_native_wait(RuntimeNativeWaitHandle *handle);
  RuntimeNativeWaitResult
  poll_native_wait(const RuntimeNativeWaitHandle &handle) const;
  RuntimeNativeWaitResult finish_native_wait(RuntimeNativeWaitHandle *handle);

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace amber::runtime
