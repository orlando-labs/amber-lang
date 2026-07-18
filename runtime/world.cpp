#include "runtime/world.h"
#include "runtime/amber_ext_runtime.h"
#include "runtime/context.h"
#include "runtime/stdlib_registry.h"
#include "runtime/vm_internal.h"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace amber::runtime {

namespace {

RuntimeIoProviderStatus
unsupported_io_provider_operation(const std::string &operation) {
  RuntimeIoProviderStatus status;
  status.handled = false;
  status.ok = false;
  status.error_name = "UnsupportedOperationError";
  status.message = "recorded IO provider does not implement " + operation;
  return status;
}

} // namespace

RuntimeIoProviderStatus RuntimeIoProvider::fs_exists(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.exists?");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_file(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.file?");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_dir(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.dir?");
}

RuntimeIoProviderStatus
RuntimeIoProvider::fs_metadata(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.metadata");
}

RuntimeIoProviderStatus
RuntimeIoProvider::fs_read_bytes(const std::string &path,
                                 std::optional<std::size_t> limit) {
  (void)path;
  (void)limit;
  return unsupported_io_provider_operation("fs.read_bytes");
}

RuntimeIoProviderStatus
RuntimeIoProvider::fs_write_bytes(const std::string &path,
                                  const std::string &bytes, bool create,
                                  bool truncate, bool append) {
  (void)path;
  (void)bytes;
  (void)create;
  (void)truncate;
  (void)append;
  return unsupported_io_provider_operation("fs.write_bytes");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_mkdir(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.mkdir");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_mkdir_p(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.mkdir_p");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_remove(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.remove");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_rename(const std::string &from,
                                                     const std::string &to) {
  (void)from;
  (void)to;
  return unsupported_io_provider_operation("fs.rename");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_copy(const std::string &from,
                                                   const std::string &to) {
  (void)from;
  (void)to;
  return unsupported_io_provider_operation("fs.copy");
}

namespace {

constexpr std::uint32_t kMethodFlagInstance =
    amber::bytecode::kMethodFlagInstance;
constexpr std::uint32_t kMethodFlagClass = amber::bytecode::kMethodFlagClass;
constexpr std::uint32_t kMethodFlagPropertyGetter =
    amber::bytecode::kMethodFlagPropertyGetter;
constexpr std::uint32_t kMethodFlagPropertySetter =
    amber::bytecode::kMethodFlagPropertySetter;
constexpr std::uint32_t kMethodFlagClauseFallback =
    amber::bytecode::kMethodFlagClauseFallback;
constexpr std::uint32_t kMethodFlagRuntimePreservedMask =
    kMethodFlagPropertyGetter | kMethodFlagPropertySetter |
    kMethodFlagClauseFallback;

const bytecode::BcCode *find_code(const bytecode::BcModule &module,
                                  std::uint32_t code_id) {
  for (const bytecode::BcCode &code : module.code_objects) {
    if (code.code_id == code_id) {
      return &code;
    }
  }
  return nullptr;
}

Value unwrap_watch_value(const Value &value) {
  if (!value.is_watch_cell()) {
    return value;
  }
  const std::shared_ptr<RuntimeWatchCell> cell = value.as_watch_cell();
  return cell == nullptr ? value : cell->read();
}

struct RuntimePackageImage {
  pkg::PackageManifest manifest;
  std::map<std::string, bytecode::BcModule> modules;
};

struct RuntimePackageImageDecode {
  bool ok = false;
  RuntimePackageImage image;
  std::vector<RuntimePackageReloadDiagnostic> diagnostics;
};

RuntimePackageReloadDiagnostic
runtime_reload_diagnostic(std::string error_name, std::string message,
                          std::string module_name = {}) {
  RuntimePackageReloadDiagnostic diagnostic;
  diagnostic.error_name = std::move(error_name);
  diagnostic.message = std::move(message);
  diagnostic.module_name = std::move(module_name);
  return diagnostic;
}

RuntimePackageImageDecode
decode_runtime_package_image(const pkg::PackageArtifact &artifact) {
  RuntimePackageImageDecode decoded;
  decoded.image.manifest = artifact.manifest;

  for (const pkg::PackageModuleBlob &blob : artifact.modules) {
    if (blob.name.empty()) {
      decoded.diagnostics.push_back(runtime_reload_diagnostic(
          "PackageReloadError", "package module name is empty"));
      continue;
    }
    bytecode::DecodeResult module = bytecode::deserialize_module(blob.bytes);
    if (!module.ok()) {
      decoded.diagnostics.push_back(runtime_reload_diagnostic(
          "BytecodeVerificationError",
          bytecode::verify_errors_to_json(module.errors), blob.name));
      continue;
    }
    if (!decoded.image.modules.emplace(blob.name, std::move(module.module))
             .second) {
      decoded.diagnostics.push_back(runtime_reload_diagnostic(
          "PackageReloadError", "duplicate package module: " + blob.name,
          blob.name));
    }
  }

  if (!artifact.manifest.root_module.empty() &&
      decoded.image.modules.find(artifact.manifest.root_module) ==
          decoded.image.modules.end()) {
    decoded.diagnostics.push_back(runtime_reload_diagnostic(
        "PackageReloadError",
        "package root module is missing: " + artifact.manifest.root_module,
        artifact.manifest.root_module));
  }

  decoded.ok = decoded.diagnostics.empty();
  return decoded;
}

bytecode::BcModule
root_module_or_empty_for_package(const pkg::PackageArtifact &artifact,
                                 std::optional<RuntimePackageImage> *image) {
  RuntimePackageImageDecode decoded = decode_runtime_package_image(artifact);
  if (!decoded.ok) {
    if (image != nullptr) {
      image->reset();
    }
    return {};
  }
  const auto found = decoded.image.modules.find(artifact.manifest.root_module);
  if (found == decoded.image.modules.end()) {
    if (image != nullptr) {
      image->reset();
    }
    return {};
  }
  bytecode::BcModule root = found->second;
  if (image != nullptr) {
    *image = std::move(decoded.image);
  }
  return root;
}

} // namespace

struct RuntimeWorld::Impl {
  explicit Impl(const bytecode::BcModule &module_ref)
      : Impl(bytecode::BcModule(module_ref), std::nullopt,
             RuntimeWorldOptions{}) {}

  Impl(bytecode::BcModule module_value,
       std::optional<RuntimePackageImage> package_image,
       RuntimeWorldOptions world_options = {})
      : owned_module(
            std::make_shared<bytecode::BcModule>(std::move(module_value))),
        module(owned_module.get()), state(std::make_shared<RuntimeState>()),
        package(std::move(package_image)), options(std::move(world_options)) {
    state->initialize_for_module(*module);
    register_builtin_stdlib(native_registry);
    register_core_prelude_bindings(module_registry);
    register_legacy_native_type_paths(module_registry);
    register_builtin_runtime_modules(module_registry, dispatch_registry,
                                     type_registry, &error_registry);
    RuntimeNativePackageDescriptor native_package =
        runtime_native_package_descriptor_from_module(*module);
    NativeExtRegistry::global().contribute_to(native_package);
    register_runtime_native_package_descriptor(
        dispatch_registry, type_registry, error_registry, native_package);
    capabilities = capability::resolve_capabilities(module->capabilities,
                                                    options.capability_grants);
    effects = effect::validate_effect_summaries(
        module->effects, options.allowed_effects, options.enforce_effects);
    schemas =
        data::validate_schemas(module->schemas, module->schema_migrations);
    table_plans = data::validate_table_plans(module->table_plans);
    wasm_components =
        wasm_accel::validate_wasm_components(module->wasm_components);
    accelerator_kernels =
        wasm_accel::validate_accelerator_kernels(module->accelerator_kernels);
    agent_metadata = modern::validate_agent_metadata(
        module->agent_symbols, module->agent_patches,
        module->provenance_records);
    contract_metadata = modern::validate_contract_metadata(module->contracts,
                                                           module->properties);
    privacy_metadata = modern::validate_privacy_metadata(
        module->privacy_labels, module->privacy_policies,
        module->lineage_nodes);
    workflow_metadata = modern::validate_workflow_metadata(
        module->workflow_steps, module->workflow_history);
    trace.schema = "amber.replay.v1";
    trace.capability_grants = options.capability_grants;
    trace.schema_versions.push_back("amber.replay.v1");
    for (const data::SchemaDefinition &schema : schemas.schemas) {
      trace.schema_versions.push_back(data::schema_version_id(schema));
    }
    if (options.record_replay_trace || options.enforce_replay) {
      replay_validation.ok = true;
      record_event(replay::make_event(
          "loader.module.load",
          {{"module",
            package.has_value() ? package->manifest.root_module : "module"}}));
    }
  }

  replay::TraceEvent record_event(replay::TraceEvent event) {
    if (!options.record_replay_trace && !options.enforce_replay) {
      return event;
    }
    if (event.event_id == 0) {
      event.event_id = static_cast<std::uint64_t>(trace.events.size()) + 1U;
    }
    if (event.timestamp_or_virtual_time == 0) {
      event.timestamp_or_virtual_time =
          options.virtual_time_start +
          (event.event_id - 1U) * options.virtual_time_step;
    }
    if (event.trace_id.empty()) {
      event.trace_id = options.trace_id.empty() ? "runtime" : options.trace_id;
    }
    if (event.module_id.empty() && package.has_value()) {
      event.module_id = package->manifest.root_module;
    }
    if (event.world_epoch == 0 && state != nullptr) {
      event.world_epoch = state->world_epoch;
    }
    event = replay::normalize_event(std::move(event));
    trace.events.push_back(event);

    if (options.enforce_replay) {
      const replay::ReplayTrace expected =
          replay::normalize_trace(options.expected_replay);
      if (replay_cursor >= expected.events.size()) {
        replay_validation.diagnostics.push_back(replay::ReplayDiagnostic{
            "ReplayDivergenceError", "replay produced an extra event", 0,
            event.event_id, event.name});
      } else {
        const replay::TraceEvent expected_event =
            replay::normalize_event(expected.events[replay_cursor]);
        if (replay::event_signature(expected_event) !=
            replay::event_signature(event)) {
          replay_validation.diagnostics.push_back(replay::ReplayDiagnostic{
              "ReplayDivergenceError",
              "replay event diverged at index " + std::to_string(replay_cursor),
              expected_event.event_id, event.event_id, event.name});
        }
      }
      ++replay_cursor;
      replay_validation.consumed_events = replay_cursor;
      replay_validation.ok = replay_validation.diagnostics.empty();
    }
    return event;
  }

  RuntimeReplayValidation current_replay_validation() const {
    RuntimeReplayValidation result = replay_validation;
    if (options.enforce_replay && result.diagnostics.empty() &&
        replay_cursor < options.expected_replay.events.size()) {
      result.diagnostics.push_back(replay::ReplayDiagnostic{
          "ReplayDivergenceError",
          "replay ended before consuming expected events",
          options.expected_replay.events[replay_cursor].event_id, 0,
          options.expected_replay.events[replay_cursor].name});
    }
    result.consumed_events = replay_cursor;
    result.ok = result.diagnostics.empty();
    return result;
  }

  std::shared_ptr<bytecode::BcModule> owned_module;
  const bytecode::BcModule *module = nullptr;
  std::shared_ptr<RuntimeState> state;
  std::optional<RuntimePackageImage> package;
  NativeRegistry native_registry;
  RuntimeModuleRegistry module_registry;
  RuntimeTypeRegistry type_registry;
  RuntimeDispatchRegistry dispatch_registry;
  RuntimeErrorRegistry error_registry;
  RuntimeWorldOptions options;
  RuntimeCapabilityResolution capabilities;
  RuntimeEffectValidation effects;
  RuntimeSchemaValidation schemas;
  RuntimeTablePlanValidation table_plans;
  RuntimeWasmValidation wasm_components;
  RuntimeAcceleratorValidation accelerator_kernels;
  RuntimeAgentValidation agent_metadata;
  RuntimeContractValidation contract_metadata;
  RuntimePrivacyValidation privacy_metadata;
  RuntimeWorkflowValidation workflow_metadata;
  RuntimeReplayTrace trace;
  RuntimeReplayValidation replay_validation;
  std::size_t replay_cursor = 0;
  std::mutex value_mutex;
  std::mutex execution_mutex;
};

RuntimeWorld::RuntimeWorld(const bytecode::BcModule &module)
    : impl_(std::make_shared<Impl>(module)) {}

RuntimeWorld::RuntimeWorld(const bytecode::BcModule &module,
                           RuntimeWorldOptions options)
    : impl_(std::make_shared<Impl>(bytecode::BcModule(module), std::nullopt,
                                   std::move(options))) {}

RuntimeWorld::RuntimeWorld(const pkg::PackageArtifact &artifact) {
  std::optional<RuntimePackageImage> image;
  bytecode::BcModule root = root_module_or_empty_for_package(artifact, &image);
  impl_ = std::make_shared<Impl>(std::move(root), std::move(image),
                                 RuntimeWorldOptions{});
}

RuntimeWorld::RuntimeWorld(const pkg::PackageArtifact &artifact,
                           RuntimeWorldOptions options) {
  std::optional<RuntimePackageImage> image;
  bytecode::BcModule root = root_module_or_empty_for_package(artifact, &image);
  impl_ = std::make_shared<Impl>(std::move(root), std::move(image),
                                 std::move(options));
}

RuntimeWorld::~RuntimeWorld() = default;

Value RuntimeWorld::string_value(std::string text) {
  if (impl_ == nullptr || impl_->owned_module == nullptr) {
    return Value::null();
  }
  std::lock_guard<std::mutex> execution_guard(impl_->execution_mutex);
  std::lock_guard<std::mutex> guard(impl_->value_mutex);
  std::vector<std::string> &strings = impl_->owned_module->strings;
  const auto found = std::find(strings.begin(), strings.end(), text);
  if (found != strings.end()) {
    return Value::string(
        static_cast<std::uint32_t>(std::distance(strings.begin(), found)));
  }
  const std::uint32_t id = static_cast<std::uint32_t>(strings.size());
  strings.push_back(std::move(text));
  return Value::string(id);
}

Value RuntimeWorld::list_value(std::vector<Value> items) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return Value::null();
  }
  return impl_->state->heap.make_list_value(std::move(items));
}

ExecutionResult RuntimeWorld::execute(std::uint32_t code_id,
                                      const std::vector<Value> &args,
                                      Value self, Value block) {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {Value::null(),
            Fault{"VMError", "runtime world is not bound", 0, 0}};
  }
  std::lock_guard<std::mutex> execution_guard(impl_->execution_mutex);
  impl_->state->initialize_for_module(*impl_->module);
  impl_->record_event(replay::make_event(
      "task.started", {{"code_id", std::to_string(code_id)}}));
  const auto wait_interest_name = [](RuntimeIoWaitInterest interest) {
    switch (interest) {
    case RuntimeIoWaitInterest::Read:
      return "read";
    case RuntimeIoWaitInterest::Write:
      return "write";
    case RuntimeIoWaitInterest::Accept:
      return "accept";
    case RuntimeIoWaitInterest::Connect:
      return "connect";
    case RuntimeIoWaitInterest::Flush:
      return "flush";
    case RuntimeIoWaitInterest::Close:
      return "close";
    case RuntimeIoWaitInterest::Metadata:
      return "metadata";
    case RuntimeIoWaitInterest::Open:
      return "open";
    case RuntimeIoWaitInterest::Other:
      break;
    }
    return "other";
  };
  RuntimeIoWaitObserver io_wait_observer =
      [impl = impl_, wait_interest_name](const RuntimeIoWaitRecord &record,
                                         bool entering) {
        std::vector<replay::TraceAttribute> attributes{
            {"phase", entering ? "enter" : "exit"},
            {"operation", record.operation},
            {"resource", record.resource},
            {"interest", wait_interest_name(record.interest)},
            {"wait_id", std::to_string(record.wait_id)},
            {"resource_id", std::to_string(record.resource_id)},
            {"task_id", std::to_string(record.task_id)},
            {"strand_id", std::to_string(record.strand_id)},
            {"worker_id", std::to_string(record.worker_id)}};
        if (record.has_timeout) {
          attributes.push_back(
              {"timeout_ms", std::to_string(record.timeout_millis)});
        }
        impl->record_event(
            replay::make_event("io.wait", std::move(attributes)));
      };
  RuntimeIoWaitObserverScope io_wait_scope(impl_->options.record_replay_trace ||
                                                   impl_->options.enforce_replay
                                               ? &io_wait_observer
                                               : nullptr);
  const std::string module_id =
      impl_->package.has_value() ? impl_->package->manifest.root_module : "";
  RuntimeVmExecutionContext vm_context;
  vm_context.state = impl_->state;
  vm_context.module_id = module_id;
  vm_context.world_options = &impl_->options;
  vm_context.capabilities = &impl_->capabilities;
  vm_context.effects = &impl_->effects;
  vm_context.trace_recorder = [impl = impl_](RuntimeTraceEvent event) {
    impl->record_event(std::move(event));
  };
  vm_context.native_registry = &impl_->native_registry;
  vm_context.module_registry = &impl_->module_registry;
  vm_context.type_registry = &impl_->type_registry;
  vm_context.dispatch_registry = &impl_->dispatch_registry;
  vm_context.error_registry = &impl_->error_registry;
  ExecutionResult result =
      execute_runtime_vm(*impl_->module, std::move(vm_context), code_id, args,
                         std::move(self), std::move(block));
  if (!result.runtime_strings.empty()) {
    impl_->owned_module->strings = result.runtime_strings;
  }
  if (!result.runtime_symbols.empty()) {
    impl_->owned_module->symbols = result.runtime_symbols;
  }
  // Values stored in the persistent world may refer to names interned by an
  // earlier execute call. Always return the world's current tables so callers
  // can decode such values even when this particular call added no names.
  result.runtime_strings = impl_->owned_module->strings;
  result.runtime_symbols = impl_->owned_module->symbols;
  if (result.ok()) {
    impl_->record_event(replay::make_event(
        "task.completed", {{"code_id", std::to_string(code_id)}}));
  } else {
    impl_->record_event(replay::make_event(
        "task.failed", {{"code_id", std::to_string(code_id)},
                        {"error_name", result.fault->error_name}}));
  }
  return result;
}

ExecutionResult RuntimeWorld::invoke_native_extension(
    std::uint32_t code_id, const std::vector<Value> &args, Value self) {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {Value::null(),
            Fault{"VMError", "runtime world is not bound", 0, 0}};
  }
  std::lock_guard<std::mutex> execution_guard(impl_->execution_mutex);
  impl_->state->initialize_for_module(*impl_->module);
  impl_->record_event(replay::make_event(
      "native_extension.started", {{"code_id", std::to_string(code_id)}}));

  RuntimeVmExecutionContext context;
  context.state = impl_->state;
  context.module_id =
      impl_->package.has_value() ? impl_->package->manifest.root_module : "";
  context.world_options = &impl_->options;
  context.capabilities = &impl_->capabilities;
  context.effects = &impl_->effects;
  context.trace_recorder = [impl = impl_](RuntimeTraceEvent event) {
    impl->record_event(std::move(event));
  };
  context.native_registry = &impl_->native_registry;
  context.module_registry = &impl_->module_registry;
  context.type_registry = &impl_->type_registry;
  context.dispatch_registry = &impl_->dispatch_registry;
  context.error_registry = &impl_->error_registry;

  ExecutionResult result = invoke_runtime_native_extension(
      *impl_->module, std::move(context), code_id, args, std::move(self));
  if (!result.runtime_strings.empty()) {
    impl_->owned_module->strings = result.runtime_strings;
  }
  if (!result.runtime_symbols.empty()) {
    impl_->owned_module->symbols = result.runtime_symbols;
  }
  result.runtime_strings = impl_->owned_module->strings;
  result.runtime_symbols = impl_->owned_module->symbols;
  impl_->record_event(replay::make_event(
      result.ok() ? "native_extension.completed" : "native_extension.failed",
      {{"code_id", std::to_string(code_id)}}));
  return result;
}

namespace {

ExecutionResult runtime_world_fault(const std::string &name,
                                    const std::string &message) {
  return {Value::null(), Fault{name, message, 0, 0}};
}

RuntimeOwnerKind owner_kind_for_index(const bytecode::BcModule &module,
                                      std::uint32_t owner_index) {
  if (owner_index < module.classes.size() &&
      (module.classes[owner_index].flags & bytecode::kClassFlagMixin) != 0U) {
    return RuntimeOwnerKind::Mixin;
  }
  return RuntimeOwnerKind::Class;
}

bool owner_is_mixin(const bytecode::BcModule &module,
                    std::uint32_t owner_index) {
  return owner_index < module.classes.size() &&
         (module.classes[owner_index].flags & bytecode::kClassFlagMixin) != 0U;
}

std::string join_class_path(const std::vector<std::string> &segments) {
  std::string out;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (i != 0U) {
      out += ".";
    }
    out += segments[i];
  }
  return out;
}

bool resolve_class_ref_for_world(const bytecode::BcModule &module,
                                 std::uint32_t path_ref,
                                 std::uint32_t *out_class_index,
                                 std::string *error_message) {
  if (path_ref >= module.const_pool.size()) {
    *error_message = "class path ref is out of range";
    return false;
  }
  const bytecode::Constant &constant = module.const_pool[path_ref];
  if (constant.kind != bytecode::ConstantKind::Path) {
    *error_message = "class ref must point to path constant";
    return false;
  }
  if (constant.items.empty()) {
    *error_message = "class path constant is empty";
    return false;
  }

  std::vector<std::string> segments;
  segments.reserve(constant.items.size());
  for (std::uint32_t symbol_id : constant.items) {
    if (symbol_id >= module.symbols.size()) {
      *error_message = "class path symbol ref is out of range";
      return false;
    }
    segments.push_back(module.symbols[symbol_id]);
  }

  const std::string full_path = join_class_path(segments);
  for (std::uint32_t index = 0; index < module.classes.size(); ++index) {
    const std::uint32_t symbol_id = module.classes[index].class_name_sym_id;
    if (symbol_id < module.symbols.size() &&
        module.symbols[symbol_id] == full_path) {
      *out_class_index = index;
      return true;
    }
  }

  const std::string &leaf = segments.back();
  std::optional<std::uint32_t> match;
  for (std::uint32_t index = 0; index < module.classes.size(); ++index) {
    const std::uint32_t symbol_id = module.classes[index].class_name_sym_id;
    if (symbol_id >= module.symbols.size() ||
        module.symbols[symbol_id] != leaf) {
      continue;
    }
    if (match.has_value()) {
      *error_message = "class path ref is ambiguous";
      return false;
    }
    match = index;
  }
  if (match.has_value()) {
    *out_class_index = *match;
    return true;
  }
  *error_message = "class path ref target is unknown";
  return false;
}

bool static_direct_refs_contain(const bytecode::BcModule &module,
                                const std::vector<std::uint32_t> &refs,
                                std::uint32_t mixin_index,
                                std::string *error_message) {
  for (std::uint32_t ref : refs) {
    std::uint32_t resolved = 0;
    if (!resolve_class_ref_for_world(module, ref, &resolved, error_message)) {
      return false;
    }
    if (resolved == mixin_index) {
      return true;
    }
  }
  return false;
}

bool effective_direct_mixins_contain(const bytecode::BcModule &module,
                                     const RuntimeState &state,
                                     std::uint32_t owner_index,
                                     std::uint32_t mixin_index, bool class_side,
                                     std::string *error_message) {
  if (owner_index < state.classes.size()) {
    const std::vector<std::uint32_t> &dynamic_mixins =
        class_side ? state.classes[owner_index].direct_extend_indices
                   : state.classes[owner_index].direct_include_indices;
    if (std::find(dynamic_mixins.begin(), dynamic_mixins.end(), mixin_index) !=
        dynamic_mixins.end()) {
      return true;
    }
  }
  if (owner_index >= module.classes.size()) {
    return false;
  }
  const std::vector<std::uint32_t> &static_refs =
      class_side ? module.classes[owner_index].direct_extend_refs
                 : module.classes[owner_index].direct_include_refs;
  return static_direct_refs_contain(module, static_refs, mixin_index,
                                    error_message);
}

bool append_static_include_edges(const bytecode::BcModule &module,
                                 std::uint32_t mixin_index,
                                 std::vector<std::uint32_t> *edges,
                                 Fault *fault) {
  for (std::uint32_t ref : module.classes[mixin_index].direct_include_refs) {
    std::uint32_t resolved = 0;
    std::string message;
    if (!resolve_class_ref_for_world(module, ref, &resolved, &message)) {
      *fault = Fault{"VMError", message, 0, 0};
      return false;
    }
    edges->push_back(resolved);
  }
  return true;
}

bool collect_include_edges_after_transaction(const bytecode::BcModule &module,
                                             const RuntimeState &state,
                                             const RuntimeWorldTransaction &tx,
                                             std::uint32_t mixin_index,
                                             std::vector<std::uint32_t> *edges,
                                             Fault *fault) {
  edges->clear();
  if (mixin_index < state.classes.size()) {
    const std::vector<std::uint32_t> &dynamic_mixins =
        state.classes[mixin_index].direct_include_indices;
    edges->insert(edges->end(), dynamic_mixins.begin(), dynamic_mixins.end());
  }
  if (tx.target_kind == RuntimeOwnerKind::Mixin &&
      tx.target_index == mixin_index) {
    edges->insert(edges->end(), tx.include_indices.begin(),
                  tx.include_indices.end());
  }
  return append_static_include_edges(module, mixin_index, edges, fault);
}

bool validate_mixin_include_graph_from(
    const bytecode::BcModule &module, const RuntimeState &state,
    const RuntimeWorldTransaction &tx, std::uint32_t mixin_index,
    std::vector<bool> *seen, std::vector<bool> *active, Fault *fault) {
  if (mixin_index >= module.classes.size()) {
    *fault = Fault{"VMError", "mixin target index is out of range", 0, 0};
    return false;
  }
  if (!owner_is_mixin(module, mixin_index)) {
    *fault = Fault{"TypeError", "include/extend target is not a mixin", 0, 0};
    return false;
  }
  if ((*active)[mixin_index]) {
    *fault = Fault{"IncludeCycleError", "cycle detected in mixin include graph",
                   0, 0};
    return false;
  }
  if ((*seen)[mixin_index]) {
    return true;
  }

  (*seen)[mixin_index] = true;
  (*active)[mixin_index] = true;

  std::vector<std::uint32_t> edges;
  if (!collect_include_edges_after_transaction(module, state, tx, mixin_index,
                                               &edges, fault)) {
    (*active)[mixin_index] = false;
    return false;
  }
  for (std::uint32_t edge : edges) {
    if (!validate_mixin_include_graph_from(module, state, tx, edge, seen,
                                           active, fault)) {
      (*active)[mixin_index] = false;
      return false;
    }
  }

  (*active)[mixin_index] = false;
  return true;
}

ExecutionResult validate_world_transaction(const bytecode::BcModule &module,
                                           const RuntimeState &state,
                                           const RuntimeWorldTransaction &tx) {
  if (state.world_frozen) {
    return runtime_world_fault("WorldFrozenError",
                               "world mutation after freeze barrier");
  }
  if (tx.target_index >= module.classes.size()) {
    return runtime_world_fault("VMError",
                               "world transaction target is out of range");
  }

  const RuntimeOwnerKind actual_kind =
      owner_kind_for_index(module, tx.target_index);
  if (actual_kind != tx.target_kind) {
    return runtime_world_fault("TypeError",
                               "world transaction target kind mismatch");
  }
  if (tx.target_kind == RuntimeOwnerKind::Mixin) {
    if (tx.has_superclass_ref) {
      return runtime_world_fault("TypeError",
                                 "mixin reopen cannot declare superclass");
    }
    if (!tx.class_methods.empty() || !tx.extend_indices.empty()) {
      return runtime_world_fault("TypeError",
                                 "mixin body cannot publish class-side state");
    }
  }
  if (tx.target_kind == RuntimeOwnerKind::Class && tx.has_superclass_ref) {
    const bytecode::BcClass &owner = module.classes[tx.target_index];
    if (!owner.has_superclass_ref) {
      return runtime_world_fault(
          "SuperclassMismatchError",
          "class reopen superclass does not match original declaration");
    }
    std::uint32_t existing_superclass = 0;
    std::uint32_t requested_superclass = 0;
    std::string message;
    if (!resolve_class_ref_for_world(module, owner.superclass_ref,
                                     &existing_superclass, &message) ||
        !resolve_class_ref_for_world(module, tx.superclass_ref,
                                     &requested_superclass, &message)) {
      return runtime_world_fault("VMError", message);
    }
    if (existing_superclass != requested_superclass) {
      return runtime_world_fault(
          "SuperclassMismatchError",
          "class reopen superclass does not match original declaration");
    }
  }

  for (const bytecode::BcMethod &method : tx.instance_methods) {
    if (method.selector_sym_id >= module.symbols.size()) {
      return runtime_world_fault("VMError",
                                 "define_method selector is out of range");
    }
    if (find_code(module, method.entry_code_id) == nullptr) {
      return runtime_world_fault("VMError",
                                 "define_method entry code id is unknown");
    }
  }
  for (const bytecode::BcMethod &method : tx.class_methods) {
    if (method.selector_sym_id >= module.symbols.size()) {
      return runtime_world_fault("VMError",
                                 "define_method selector is out of range");
    }
    if (find_code(module, method.entry_code_id) == nullptr) {
      return runtime_world_fault("VMError",
                                 "define_method entry code id is unknown");
    }
  }

  for (std::uint32_t mixin_index : tx.include_indices) {
    if (mixin_index >= module.classes.size()) {
      return runtime_world_fault("VMError",
                                 "mixin target index is out of range");
    }
    if (!owner_is_mixin(module, mixin_index)) {
      return runtime_world_fault("TypeError",
                                 "include/extend target is not a mixin");
    }
  }
  for (std::uint32_t mixin_index : tx.extend_indices) {
    if (mixin_index >= module.classes.size()) {
      return runtime_world_fault("VMError",
                                 "mixin target index is out of range");
    }
    if (!owner_is_mixin(module, mixin_index)) {
      return runtime_world_fault("TypeError",
                                 "include/extend target is not a mixin");
    }
  }

  if (!tx.include_indices.empty() || !tx.extend_indices.empty()) {
    std::vector<bool> seen(module.classes.size(), false);
    std::vector<bool> active(module.classes.size(), false);
    Fault fault;
    if (tx.target_kind == RuntimeOwnerKind::Mixin &&
        !validate_mixin_include_graph_from(module, state, tx, tx.target_index,
                                           &seen, &active, &fault)) {
      return {Value::null(), fault};
    }
    for (std::uint32_t mixin_index : tx.include_indices) {
      if (!validate_mixin_include_graph_from(module, state, tx, mixin_index,
                                             &seen, &active, &fault)) {
        return {Value::null(), fault};
      }
    }
    for (std::uint32_t mixin_index : tx.extend_indices) {
      if (!validate_mixin_include_graph_from(module, state, tx, mixin_index,
                                             &seen, &active, &fault)) {
        return {Value::null(), fault};
      }
    }
  }

  return {Value::null(), std::nullopt};
}

RuntimeOwnerKind owner_kind_for_module_ptr(const bytecode::BcModule *module,
                                           std::uint32_t owner_index) {
  if (module == nullptr || owner_index >= module->classes.size()) {
    return RuntimeOwnerKind::Class;
  }
  return owner_kind_for_index(*module, owner_index);
}

std::string symbol_name_for_mirror(const bytecode::BcModule &module,
                                   std::uint32_t symbol_id) {
  if (symbol_id >= module.symbols.size()) {
    return "";
  }
  return module.symbols[symbol_id];
}

std::string string_name_for_mirror(const bytecode::BcModule &module,
                                   std::uint32_t string_id) {
  if (string_id >= module.strings.size()) {
    return "";
  }
  return module.strings[string_id];
}

std::string owner_name_for_mirror(const bytecode::BcModule &module,
                                  std::uint32_t owner_index) {
  if (owner_index >= module.classes.size()) {
    return "";
  }
  return symbol_name_for_mirror(module,
                                module.classes[owner_index].class_name_sym_id);
}

RuntimeMirrorSourceLocation
source_location_for_code(const bytecode::BcModule &module,
                         std::uint32_t code_id) {
  RuntimeMirrorSourceLocation location;
  location.code_id = code_id;

  const bytecode::BcCode *code = find_code(module, code_id);
  if (code != nullptr && !code->source_spans.empty()) {
    const bytecode::SourceSpanEntry *best = &code->source_spans.front();
    for (const bytecode::SourceSpanEntry &span : code->source_spans) {
      if (span.pc_from < best->pc_from) {
        best = &span;
      }
    }
    location.present = true;
    location.pc = best->pc_from;
    location.file = best->span.file;
    location.line = static_cast<std::uint32_t>(best->span.start.line);
    location.column = static_cast<std::uint32_t>(best->span.start.col);
    return location;
  }

  const bytecode::LineEntry *best_line = nullptr;
  for (const bytecode::LineEntry &entry : module.line_table) {
    if (entry.code_id != code_id) {
      continue;
    }
    if (best_line == nullptr || entry.pc < best_line->pc) {
      best_line = &entry;
    }
  }
  if (best_line != nullptr) {
    location.present = true;
    location.pc = best_line->pc;
    location.line = best_line->line;
  }
  return location;
}

RuntimeMirrorSourceLocation
source_location_for_owner(const bytecode::BcModule &module,
                          const bytecode::BcClass &owner) {
  if (owner.has_class_init_code_id) {
    return source_location_for_code(module, owner.class_init_code_id);
  }
  if (owner.method_range_start + owner.method_range_count <=
      module.methods.size()) {
    for (std::uint32_t offset = 0; offset < owner.method_range_count;
         ++offset) {
      const bytecode::BcMethod &method =
          module.methods[owner.method_range_start + offset];
      RuntimeMirrorSourceLocation location =
          source_location_for_code(module, method.entry_code_id);
      if (location.present) {
        return location;
      }
    }
  }
  return {};
}

RuntimeMethodMirror method_mirror_for(const bytecode::BcModule &module,
                                      std::uint32_t owner_index,
                                      MethodTableSide side,
                                      const bytecode::BcMethod &method) {
  RuntimeMethodMirror mirror;
  mirror.selector_symbol_id = method.selector_sym_id;
  mirror.selector = symbol_name_for_mirror(module, method.selector_sym_id);
  mirror.owner_index = owner_index;
  mirror.owner_name = owner_name_for_mirror(module, owner_index);
  mirror.owner_kind = owner_kind_for_index(module, owner_index);
  mirror.side = side;
  mirror.signature_blob_id = method.signature_blob_id;
  mirror.entry_code_id = method.entry_code_id;
  mirror.flags = method.flags;
  mirror.parameter_count = method.params.size();
  mirror.default_count = method.default_thunk_ids.size();
  mirror.type_hook_count = method.type_hook_ids.size();
  mirror.clause_count = method.clause_table.size();
  mirror.source_location =
      source_location_for_code(module, method.entry_code_id);
  return mirror;
}

std::vector<RuntimeMethodMirror>
method_mirrors_for_table(const bytecode::BcModule &module,
                         std::uint32_t owner_index, MethodTableSide side,
                         const MethodTableDescriptor &table) {
  std::vector<RuntimeMethodMirror> mirrors;
  mirrors.reserve(table.entries.size());
  for (const auto &[selector_id, method] : table.entries) {
    (void)selector_id;
    mirrors.push_back(method_mirror_for(module, owner_index, side, method));
  }
  std::sort(
      mirrors.begin(), mirrors.end(),
      [](const RuntimeMethodMirror &left, const RuntimeMethodMirror &right) {
        if (left.selector != right.selector) {
          return left.selector < right.selector;
        }
        if (left.selector_symbol_id != right.selector_symbol_id) {
          return left.selector_symbol_id < right.selector_symbol_id;
        }
        return left.entry_code_id < right.entry_code_id;
      });
  return mirrors;
}

RuntimeDirectMixinMirror
direct_mixin_mirror_for_index(const bytecode::BcModule &module,
                              std::uint32_t mixin_index, bool dynamic) {
  RuntimeDirectMixinMirror mirror;
  mirror.index = mixin_index;
  mirror.name = owner_name_for_mirror(module, mixin_index);
  mirror.dynamic = dynamic;
  return mirror;
}

void append_static_direct_mixin_mirrors(
    const bytecode::BcModule &module, const std::vector<std::uint32_t> &refs,
    std::vector<RuntimeDirectMixinMirror> *mirrors) {
  for (std::uint32_t ref : refs) {
    std::uint32_t mixin_index = 0;
    std::string message;
    if (resolve_class_ref_for_world(module, ref, &mixin_index, &message)) {
      mirrors->push_back(
          direct_mixin_mirror_for_index(module, mixin_index, false));
    }
  }
}

void append_dynamic_direct_mixin_mirrors(
    const bytecode::BcModule &module, const std::vector<std::uint32_t> &indices,
    std::vector<RuntimeDirectMixinMirror> *mirrors) {
  for (std::uint32_t mixin_index : indices) {
    mirrors->push_back(
        direct_mixin_mirror_for_index(module, mixin_index, true));
  }
}

RuntimeOwnerMirror owner_mirror_for(const bytecode::BcModule &module,
                                    const RuntimeState &state,
                                    std::uint32_t owner_index) {
  RuntimeOwnerMirror mirror;
  mirror.index = owner_index;
  if (owner_index >= module.classes.size() ||
      owner_index >= state.classes.size()) {
    return mirror;
  }

  const bytecode::BcClass &owner = module.classes[owner_index];
  const ClassRuntimeState &runtime_owner = state.classes[owner_index];
  mirror.name = owner_name_for_mirror(module, owner_index);
  mirror.kind = owner_kind_for_index(module, owner_index);
  mirror.owner_flags = runtime_owner.owner_flags;
  mirror.ivar_schema_id = runtime_owner.ivar_schema_id;
  mirror.has_superclass = runtime_owner.has_superclass_ref;
  mirror.method_version = runtime_owner.method_version;
  mirror.world_epoch = state.world_epoch;
  mirror.source_location = source_location_for_owner(module, owner);

  if (runtime_owner.has_superclass_ref) {
    std::uint32_t superclass_index = 0;
    std::string message;
    if (resolve_class_ref_for_world(module, runtime_owner.superclass_ref,
                                    &superclass_index, &message)) {
      mirror.superclass_index = superclass_index;
      mirror.superclass_name = owner_name_for_mirror(module, superclass_index);
    }
  }

  append_static_direct_mixin_mirrors(module, owner.direct_include_refs,
                                     &mirror.direct_includes);
  append_dynamic_direct_mixin_mirrors(
      module, runtime_owner.direct_include_indices, &mirror.direct_includes);
  append_static_direct_mixin_mirrors(module, owner.direct_extend_refs,
                                     &mirror.direct_extends);
  append_dynamic_direct_mixin_mirrors(
      module, runtime_owner.direct_extend_indices, &mirror.direct_extends);

  mirror.instance_methods =
      method_mirrors_for_table(module, owner_index, MethodTableSide::Instance,
                               runtime_owner.instance_method_table);
  mirror.class_methods =
      method_mirrors_for_table(module, owner_index, MethodTableSide::Class,
                               runtime_owner.class_method_table);
  return mirror;
}

std::string package_name_for_mirror(const bytecode::BcModule &module) {
  for (const bytecode::AttrEntry &attr : module.attrs) {
    const std::string key = string_name_for_mirror(module, attr.key_str_id);
    if (key != "amber.package" && key != "package" && key != "module" &&
        key != "module.name") {
      continue;
    }
    const std::string value = string_name_for_mirror(module, attr.value_str_id);
    if (!value.empty()) {
      return value;
    }
  }
  return "";
}

RuntimePackageMirror package_mirror_for(const bytecode::BcModule &module) {
  RuntimePackageMirror mirror;
  mirror.name = package_name_for_mirror(module);
  mirror.format_version = module.format_version;
  mirror.language_version = module.language_version;
  mirror.profile_flags = module.profile_flags;
  mirror.file_flags = module.file_flags;
  mirror.has_init = module.init.has_entry_code_id;
  mirror.init_code_id = module.init.entry_code_id;

  for (const bytecode::DepEntry &dependency : module.dependencies) {
    RuntimePackageDependencyMirror dep;
    dep.module_name =
        string_name_for_mirror(module, dependency.module_name_str_id);
    dep.required_format = dependency.required_format;
    dep.min_language_version = dependency.min_language_version;
    dep.has_max_language_version = dependency.has_max_language_version;
    dep.max_language_version = dependency.max_language_version;
    dep.has_abi_requirement = dependency.has_abi_requirement;
    mirror.dependencies.push_back(dep);
  }
  std::sort(mirror.dependencies.begin(), mirror.dependencies.end(),
            [](const RuntimePackageDependencyMirror &left,
               const RuntimePackageDependencyMirror &right) {
              return left.module_name < right.module_name;
            });

  for (const bytecode::ExportEntry &entry : module.exports) {
    RuntimePackageExportMirror export_mirror;
    export_mirror.public_name = symbol_name_for_mirror(module, entry.symbol_id);
    export_mirror.target_kind =
        string_name_for_mirror(module, entry.target_kind_str_id);
    export_mirror.target_index = entry.target_index;
    export_mirror.visibility_flags = entry.visibility_flags;
    export_mirror.has_reexport = entry.has_reexport_module_name;
    export_mirror.reexport_module_name =
        string_name_for_mirror(module, entry.reexport_module_name_str_id);
    mirror.exports.push_back(export_mirror);
  }
  std::sort(mirror.exports.begin(), mirror.exports.end(),
            [](const RuntimePackageExportMirror &left,
               const RuntimePackageExportMirror &right) {
              if (left.public_name != right.public_name) {
                return left.public_name < right.public_name;
              }
              if (left.target_kind != right.target_kind) {
                return left.target_kind < right.target_kind;
              }
              return left.target_index < right.target_index;
            });

  for (const bytecode::AttrEntry &attr : module.attrs) {
    mirror.attrs.push_back({string_name_for_mirror(module, attr.key_str_id),
                            string_name_for_mirror(module, attr.value_str_id)});
  }
  std::sort(mirror.attrs.begin(), mirror.attrs.end(),
            [](const RuntimePackageAttrMirror &left,
               const RuntimePackageAttrMirror &right) {
              if (left.key != right.key) {
                return left.key < right.key;
              }
              return left.value < right.value;
            });

  mirror.capabilities = module.capabilities;
  std::sort(mirror.capabilities.begin(), mirror.capabilities.end(),
            [](const RuntimeCapabilityGrant &left,
               const RuntimeCapabilityGrant &right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.target < right.target;
            });
  mirror.effects = module.effects;
  std::sort(
      mirror.effects.begin(), mirror.effects.end(),
      [](const RuntimeEffectSummary &left, const RuntimeEffectSummary &right) {
        if (left.owner != right.owner) {
          return left.owner < right.owner;
        }
        return left.kind < right.kind;
      });
  mirror.observability_sites = module.observability_sites;
  std::sort(mirror.observability_sites.begin(),
            mirror.observability_sites.end(),
            [](const RuntimeObservabilitySite &left,
               const RuntimeObservabilitySite &right) {
              return left.site_id < right.site_id;
            });
  mirror.replay_metadata = replay::normalize_metadata(module.replay_metadata);
  const RuntimeSchemaValidation schema_validation =
      data::validate_schemas(module.schemas, module.schema_migrations);
  mirror.schemas = schema_validation.schemas;
  mirror.schema_migrations = schema_validation.migrations;
  mirror.table_plans = data::validate_table_plans(module.table_plans).plans;
  mirror.wasm_components =
      wasm_accel::validate_wasm_components(module.wasm_components).components;
  mirror.accelerator_kernels =
      wasm_accel::validate_accelerator_kernels(module.accelerator_kernels)
          .kernels;
  mirror.agent_symbols =
      modern::validate_agent_metadata(
          module.agent_symbols, module.agent_patches, module.provenance_records)
          .symbols;
  mirror.contracts =
      modern::validate_contract_metadata(module.contracts, module.properties)
          .contracts;
  mirror.privacy_labels =
      modern::validate_privacy_metadata(
          module.privacy_labels, module.privacy_policies, module.lineage_nodes)
          .labels;
  mirror.workflow_steps = modern::validate_workflow_metadata(
                              module.workflow_steps, module.workflow_history)
                              .steps;

  return mirror;
}

std::string version_signature(const bytecode::Version &version) {
  return std::to_string(version.major) + "." + std::to_string(version.minor);
}

std::string bytes_hex_signature(const std::array<std::uint8_t, 32> &bytes) {
  std::ostringstream out;
  const char *hex = "0123456789abcdef";
  for (const std::uint8_t byte : bytes) {
    out << hex[(byte >> 4U) & 0x0FU] << hex[byte & 0x0FU];
  }
  return out.str();
}

std::string module_contract_signature(const bytecode::BcModule &module) {
  std::ostringstream out;
  out << "format=" << version_signature(module.format_version)
      << "\nlanguage=" << version_signature(module.language_version)
      << "\nprofile=" << module.profile_flags << "\nfile=" << module.file_flags
      << "\nabi=" << bytes_hex_signature(module.abi_hash) << "\n";
  std::vector<RuntimeCapabilityGrant> capabilities = module.capabilities;
  std::sort(capabilities.begin(), capabilities.end(),
            [](const RuntimeCapabilityGrant &left,
               const RuntimeCapabilityGrant &right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.target < right.target;
            });
  for (const RuntimeCapabilityGrant &capability : capabilities) {
    out << "capability=" << capability.name << "|" << capability.target << "|"
        << capability.flags << "\n";
  }
  std::vector<RuntimeEffectSummary> effects = module.effects;
  std::sort(
      effects.begin(), effects.end(),
      [](const RuntimeEffectSummary &left, const RuntimeEffectSummary &right) {
        if (left.owner != right.owner) {
          return left.owner < right.owner;
        }
        return left.kind < right.kind;
      });
  for (const RuntimeEffectSummary &summary : effects) {
    out << "effect=" << summary.owner << "|" << summary.kind << "|"
        << effect::effect_row_to_text(summary.declared_effects) << "|"
        << effect::effect_row_to_text(summary.observed_effects) << "|"
        << summary.flags << "\n";
  }
  std::vector<RuntimeObservabilitySite> sites = module.observability_sites;
  std::sort(sites.begin(), sites.end(),
            [](const RuntimeObservabilitySite &left,
               const RuntimeObservabilitySite &right) {
              return left.site_id < right.site_id;
            });
  for (const RuntimeObservabilitySite &site : sites) {
    out << "observability=" << site.site_id << "|" << site.event_name << "|"
        << site.kind << "|" << site.owner << "|" << site.source.file << "|"
        << site.source.line << "|" << site.source.column << "|" << site.flags
        << "\n";
  }
  const RuntimeReplayMetadata replay_metadata =
      replay::normalize_metadata(module.replay_metadata);
  out << "replay.flags=" << replay_metadata.flags << "\n";
  for (const std::string &event_name : replay_metadata.required_event_names) {
    out << "replay.required_event=" << event_name << "\n";
  }
  for (const std::string &source : replay_metadata.deterministic_sources) {
    out << "replay.deterministic_source=" << source << "\n";
  }
  const RuntimeSchemaValidation schema_validation =
      data::validate_schemas(module.schemas, module.schema_migrations);
  for (const RuntimeSchemaDefinition &schema : schema_validation.schemas) {
    out << "schema=" << data::schema_version_id(schema) << "|" << schema.flags
        << "\n";
    for (const data::SchemaField &field : schema.fields) {
      out << "schema.field=" << data::schema_version_id(schema) << "|"
          << field.name << "|" << field.type << "|" << (field.required ? 1 : 0)
          << "|" << (field.nullable ? 1 : 0) << "|" << field.default_value
          << "|" << field.flags << "\n";
    }
  }
  for (const RuntimeSchemaMigration &migration : schema_validation.migrations) {
    out << "schema.migration=" << migration.schema_name << "|"
        << migration.from_version << "|" << migration.to_version << "|"
        << migration.kind << "|" << migration.flags << "\n";
  }
  const RuntimeTablePlanValidation table_validation =
      data::validate_table_plans(module.table_plans);
  for (const RuntimeTablePlan &plan : table_validation.plans) {
    out << "table.plan=" << plan.plan_id << "|"
        << data::table_plan_fingerprint(plan) << "|" << plan.flags << "\n";
  }
  const RuntimeWasmValidation wasm_validation =
      wasm_accel::validate_wasm_components(module.wasm_components);
  for (const RuntimeWasmComponent &component : wasm_validation.components) {
    out << "wasm.component=" << component.name << "|" << component.world << "|"
        << component.flags << "\n";
    for (const wasm_accel::WasmInterfaceEntry &entry : component.imports) {
      out << "wasm.import=" << component.name << "|" << entry.name << "|"
          << entry.kind << "|" << entry.type_signature << "|"
          << entry.schema_name << "|"
          << capability::request_to_text(entry.capability) << "|"
          << effect::effect_row_to_text(entry.effect_row) << "|" << entry.flags
          << "\n";
    }
    for (const wasm_accel::WasmInterfaceEntry &entry : component.exports) {
      out << "wasm.export=" << component.name << "|" << entry.name << "|"
          << entry.kind << "|" << entry.type_signature << "|"
          << entry.schema_name << "|"
          << effect::effect_row_to_text(entry.effect_row) << "|" << entry.flags
          << "\n";
    }
  }
  const RuntimeAcceleratorValidation accelerator_validation =
      wasm_accel::validate_accelerator_kernels(module.accelerator_kernels);
  for (const RuntimeAcceleratorKernel &kernel :
       accelerator_validation.kernels) {
    out << "accelerator.kernel=" << kernel.kernel_id << "|" << kernel.entry
        << "|" << kernel.target << "|"
        << effect::effect_row_to_text(kernel.effect_row) << "|" << kernel.flags
        << "\n";
    for (const wasm_accel::AcceleratorValue &value : kernel.params) {
      out << "accelerator.param=" << kernel.kernel_id << "|" << value.name
          << "|" << value.type << "|" << value.address_space << "|"
          << value.flags << "\n";
    }
    for (const wasm_accel::AcceleratorValue &value : kernel.captures) {
      out << "accelerator.capture=" << kernel.kernel_id << "|" << value.name
          << "|" << value.type << "|" << value.address_space << "|"
          << value.flags << "\n";
    }
    for (const std::string &feature : kernel.forbidden_features) {
      out << "accelerator.forbidden=" << kernel.kernel_id << "|" << feature
          << "\n";
    }
  }
  return out.str();
}

std::string path_ref_signature(const bytecode::BcModule &module,
                               std::uint32_t ref) {
  if (ref >= module.const_pool.size()) {
    return "#invalid";
  }
  const bytecode::Constant &constant = module.const_pool[ref];
  if (constant.kind != bytecode::ConstantKind::Path) {
    return "#non-path";
  }
  std::vector<std::string> segments;
  for (const std::uint32_t symbol_id : constant.items) {
    segments.push_back(symbol_name_for_mirror(module, symbol_id));
  }
  return join_class_path(segments);
}

std::string method_boundary_signature(const bytecode::BcModule &module,
                                      const bytecode::BcMethod &method) {
  std::ostringstream out;
  out << symbol_name_for_mirror(module, method.selector_sym_id) << "|"
      << ((method.flags & kMethodFlagClass) != 0U ? "class" : "instance")
      << "|property="
      << (((method.flags & kMethodFlagPropertyGetter) != 0U) ? "1" : "0")
      << "|property_setter="
      << (((method.flags & kMethodFlagPropertySetter) != 0U) ? "1" : "0") << "|"
      << "defaults=" << method.default_thunk_ids.size() << "|"
      << "type_hooks=" << method.type_hook_ids.size() << "|params=";
  for (std::size_t i = 0; i < method.params.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const bytecode::MethodParamEntry &param = method.params[i];
    out << symbol_name_for_mirror(module, param.external_name_sym_id) << ":"
        << param.flags;
  }
  return out.str();
}

std::vector<std::string>
owner_method_boundary_signatures(const bytecode::BcModule &module,
                                 const bytecode::BcClass &owner) {
  std::vector<std::string> out;
  if (owner.method_range_start + owner.method_range_count >
      module.methods.size()) {
    out.push_back("#invalid-method-range");
    return out;
  }
  for (std::uint32_t offset = 0; offset < owner.method_range_count; ++offset) {
    out.push_back(method_boundary_signature(
        module, module.methods[owner.method_range_start + offset]));
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string owner_boundary_signature(const bytecode::BcModule &module,
                                     std::uint32_t owner_index) {
  if (owner_index >= module.classes.size()) {
    return "#missing-owner";
  }
  const bytecode::BcClass &owner = module.classes[owner_index];
  std::ostringstream out;
  out << "name=" << owner_name_for_mirror(module, owner_index) << "\nkind="
      << ((owner.flags & bytecode::kClassFlagMixin) != 0U ? "mixin" : "class")
      << "\nivar_schema=" << owner.ivar_schema_id
      << "\nhas_super=" << (owner.has_superclass_ref ? "true" : "false");
  if (owner.has_superclass_ref) {
    out << "\nsuper=" << path_ref_signature(module, owner.superclass_ref);
  }
  out << "\ninclude=";
  for (std::uint32_t ref : owner.direct_include_refs) {
    out << path_ref_signature(module, ref) << ";";
  }
  out << "\nextend=";
  for (std::uint32_t ref : owner.direct_extend_refs) {
    out << path_ref_signature(module, ref) << ";";
  }
  out << "\nmethods=";
  const std::vector<std::string> methods =
      owner_method_boundary_signatures(module, owner);
  for (const std::string &method : methods) {
    out << method << "\n";
  }
  return out.str();
}

std::string reexport_export_name_for(const bytecode::BcModule &module,
                                     const bytecode::ExportEntry &entry,
                                     const std::string &public_name) {
  if (entry.target_index < module.strings.size()) {
    return module.strings[entry.target_index];
  }
  return public_name;
}

std::vector<std::string>
export_surface_signature(const bytecode::BcModule &module) {
  std::vector<std::string> out;
  for (const bytecode::ExportEntry &entry : module.exports) {
    const std::string public_name =
        symbol_name_for_mirror(module, entry.symbol_id);
    const std::string target_kind =
        string_name_for_mirror(module, entry.target_kind_str_id);
    std::ostringstream line;
    line << public_name << "|" << target_kind << "|" << entry.target_index
         << "|" << entry.visibility_flags << "|"
         << (entry.has_reexport_module_name ? "reexport" : "local") << "|"
         << string_name_for_mirror(module, entry.reexport_module_name_str_id)
         << "|" << reexport_export_name_for(module, entry, public_name);
    out.push_back(line.str());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string>
exported_callable_boundary_signature(const bytecode::BcModule &module) {
  std::vector<std::string> out;
  for (const bytecode::ExportEntry &entry : module.exports) {
    const std::string public_name =
        symbol_name_for_mirror(module, entry.symbol_id);
    const std::string target_kind =
        string_name_for_mirror(module, entry.target_kind_str_id);
    if (target_kind == "class") {
      out.push_back("class|" + public_name + "|" +
                    owner_boundary_signature(module, entry.target_index));
    } else if (target_kind == "method") {
      if (entry.target_index >= module.methods.size()) {
        out.push_back("method|" + public_name + "|#missing-method");
      } else {
        out.push_back("method|" + public_name + "|" +
                      method_boundary_signature(
                          module, module.methods[entry.target_index]));
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string>
manifest_identity_signature(const pkg::PackageManifest &manifest) {
  std::vector<std::string> out;
  out.push_back("name=" + manifest.name);
  out.push_back("version=" + manifest.version);
  out.push_back("root=" + manifest.root_module);
  for (const pkg::PackageModule &module : manifest.modules) {
    out.push_back("module=" + module.name + "|" + module.path);
  }
  for (const pkg::PackageDependency &dependency : manifest.dependencies) {
    out.push_back("dependency=" + dependency.name + "|" + dependency.version +
                  "|" + dependency.source + "|" + dependency.checksum);
  }
  for (const RuntimeCapabilityGrant &capability : manifest.capabilities) {
    out.push_back("capability=" + capability.name + "|" + capability.target +
                  "|" + std::to_string(capability.flags));
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool append_reload_incompatibility(
    std::vector<RuntimePackageReloadDiagnostic> *diagnostics,
    const std::string &message, const std::string &module_name = {}) {
  diagnostics->push_back(runtime_reload_diagnostic("ReloadIncompatibleError",
                                                   message, module_name));
  return false;
}

bool validate_module_reload_compatible(
    const bytecode::BcModule &current, const bytecode::BcModule &next,
    const std::string &module_name,
    std::vector<RuntimePackageReloadDiagnostic> *diagnostics) {
  if (module_contract_signature(current) != module_contract_signature(next)) {
    return append_reload_incompatibility(
        diagnostics, "package reload changes ABI/profile contract",
        module_name);
  }
  if (export_surface_signature(current) != export_surface_signature(next)) {
    return append_reload_incompatibility(
        diagnostics, "package reload changes public export surface",
        module_name);
  }
  if (exported_callable_boundary_signature(current) !=
      exported_callable_boundary_signature(next)) {
    return append_reload_incompatibility(
        diagnostics, "package reload changes exported selector/arity boundary",
        module_name);
  }
  return true;
}

bool validate_package_reload_compatible(
    const RuntimePackageImage *current_package,
    const bytecode::BcModule &current_root, const RuntimePackageImage &next,
    std::vector<RuntimePackageReloadDiagnostic> *diagnostics) {
  if (current_package == nullptr) {
    const auto root = next.modules.find(next.manifest.root_module);
    if (root == next.modules.end()) {
      return append_reload_incompatibility(
          diagnostics, "package reload root module is missing",
          next.manifest.root_module);
    }
    return validate_module_reload_compatible(
        current_root, root->second, next.manifest.root_module, diagnostics);
  }

  if (manifest_identity_signature(current_package->manifest) !=
      manifest_identity_signature(next.manifest)) {
    return append_reload_incompatibility(
        diagnostics, "package reload changes manifest identity");
  }

  if (current_package->modules.size() != next.modules.size()) {
    return append_reload_incompatibility(diagnostics,
                                         "package reload changes module set");
  }
  for (const auto &[module_name, current_module] : current_package->modules) {
    const auto found = next.modules.find(module_name);
    if (found == next.modules.end()) {
      return append_reload_incompatibility(
          diagnostics, "package reload removes module: " + module_name,
          module_name);
    }
    if (!validate_module_reload_compatible(current_module, found->second,
                                           module_name, diagnostics)) {
      return false;
    }
  }
  return true;
}

} // namespace

ExecutionResult
RuntimeWorld::define_instance_method(std::uint32_t class_index,
                                     bytecode::BcMethod method) {
  RuntimeWorldTransaction tx;
  tx.target_index = class_index;
  if (impl_ != nullptr) {
    tx.target_kind = owner_kind_for_module_ptr(impl_->module, class_index);
  }
  tx.instance_methods.push_back(std::move(method));
  return commit_transaction(tx);
}

ExecutionResult RuntimeWorld::define_class_method(std::uint32_t class_index,
                                                  bytecode::BcMethod method) {
  RuntimeWorldTransaction tx;
  tx.target_kind = RuntimeOwnerKind::Class;
  tx.target_index = class_index;
  tx.class_methods.push_back(std::move(method));
  return commit_transaction(tx);
}

ExecutionResult RuntimeWorld::include_mixin(std::uint32_t class_index,
                                            std::uint32_t mixin_index) {
  RuntimeWorldTransaction tx;
  tx.target_index = class_index;
  if (impl_ != nullptr) {
    tx.target_kind = owner_kind_for_module_ptr(impl_->module, class_index);
  }
  tx.include_indices.push_back(mixin_index);
  return commit_transaction(tx);
}

ExecutionResult RuntimeWorld::extend_mixin(std::uint32_t class_index,
                                           std::uint32_t mixin_index) {
  RuntimeWorldTransaction tx;
  tx.target_kind = RuntimeOwnerKind::Class;
  tx.target_index = class_index;
  tx.extend_indices.push_back(mixin_index);
  return commit_transaction(tx);
}

ExecutionResult
RuntimeWorld::commit_transaction(const RuntimeWorldTransaction &tx) {
  if (impl_ == nullptr || impl_->module == nullptr || impl_->state == nullptr) {
    return {Value::null(),
            Fault{"VMError", "runtime world is not bound", 0, 0}};
  }
  const bytecode::BcModule &module = *impl_->module;
  RuntimeState &state = *impl_->state;
  if (state.classes.size() < module.classes.size()) {
    state.classes.resize(module.classes.size());
  }
  state.initialize_for_module(module);

  const ExecutionResult validation =
      validate_world_transaction(module, state, tx);
  if (!validation.ok()) {
    return validation;
  }

  ClassRuntimeState &runtime_owner = state.classes[tx.target_index];
  bool changed = false;
  for (bytecode::BcMethod method : tx.instance_methods) {
    method.owner_dispatch_ref = tx.target_index;
    method.flags =
        kMethodFlagInstance | (method.flags & kMethodFlagRuntimePreservedMask);
    runtime_owner.instance_method_table.entries[method.selector_sym_id] =
        std::move(method);
    changed = true;
  }
  for (bytecode::BcMethod method : tx.class_methods) {
    method.owner_dispatch_ref = tx.target_index;
    method.flags =
        kMethodFlagClass | (method.flags & kMethodFlagRuntimePreservedMask);
    runtime_owner.class_method_table.entries[method.selector_sym_id] =
        std::move(method);
    changed = true;
  }

  for (std::uint32_t mixin_index : tx.include_indices) {
    std::string message;
    if (!effective_direct_mixins_contain(module, state, tx.target_index,
                                         mixin_index, false, &message)) {
      runtime_owner.direct_include_indices.push_back(mixin_index);
      changed = true;
    }
  }
  for (std::uint32_t mixin_index : tx.extend_indices) {
    std::string message;
    if (!effective_direct_mixins_contain(module, state, tx.target_index,
                                         mixin_index, true, &message)) {
      runtime_owner.direct_extend_indices.push_back(mixin_index);
      changed = true;
    }
  }

  if (changed) {
    state.invalidate_dispatch_owner(tx.target_index);
    impl_->record_event(replay::make_event(
        "world.mutation", {{"target_index", std::to_string(tx.target_index)}}));
  }
  return {Value::null(), std::nullopt};
}

ExecutionResult RuntimeWorld::freeze_world() {
  if (impl_ == nullptr || impl_->module == nullptr || impl_->state == nullptr) {
    return {Value::null(),
            Fault{"VMError", "runtime world is not bound", 0, 0}};
  }
  impl_->state->initialize_for_module(*impl_->module);
  if (!impl_->state->world_frozen) {
    impl_->state->world_frozen = true;
    ++impl_->state->world_epoch;
    impl_->record_event(replay::make_event("world.freeze"));
  }
  return {Value::null(), std::nullopt};
}

RuntimePackageReloadResult
RuntimeWorld::reload_package_artifact(const pkg::PackageArtifact &artifact) {
  RuntimePackageReloadResult result;
  if (impl_ == nullptr || impl_->module == nullptr || impl_->state == nullptr) {
    result.diagnostics.push_back(
        runtime_reload_diagnostic("VMError", "runtime world is not bound"));
    return result;
  }

  result.previous_world_epoch = impl_->state->world_epoch;
  result.new_world_epoch = impl_->state->world_epoch;
  result.package_name = impl_->package.has_value()
                            ? impl_->package->manifest.name
                            : package_name_for_mirror(*impl_->module);
  result.previous_version =
      impl_->package.has_value() ? impl_->package->manifest.version : "";
  result.new_version = artifact.manifest.version;
  result.root_module = artifact.manifest.root_module;

  impl_->state->initialize_for_module(*impl_->module);
  if (impl_->state->world_frozen) {
    result.diagnostics.push_back(runtime_reload_diagnostic(
        "WorldFrozenError", "package reload after freeze barrier",
        artifact.manifest.root_module));
    return result;
  }

  RuntimePackageImageDecode decoded = decode_runtime_package_image(artifact);
  if (!decoded.ok) {
    result.diagnostics = std::move(decoded.diagnostics);
    return result;
  }

  const RuntimePackageImage *current_package =
      impl_->package.has_value() ? &*impl_->package : nullptr;
  if (!validate_package_reload_compatible(current_package, *impl_->module,
                                          decoded.image, &result.diagnostics)) {
    return result;
  }

  const auto root =
      decoded.image.modules.find(decoded.image.manifest.root_module);
  if (root == decoded.image.modules.end()) {
    result.diagnostics.push_back(runtime_reload_diagnostic(
        "PackageReloadError",
        "package root module is missing: " + decoded.image.manifest.root_module,
        decoded.image.manifest.root_module));
    return result;
  }

  auto next_state = std::make_shared<RuntimeState>(*impl_->state);
  next_state->replace_module_runtime_state(root->second);
  auto next_module =
      std::make_shared<bytecode::BcModule>(bytecode::BcModule(root->second));

  impl_->state = std::move(next_state);
  impl_->owned_module = std::move(next_module);
  impl_->module = impl_->owned_module.get();
  impl_->package = std::move(decoded.image);
  impl_->capabilities = capability::resolve_capabilities(
      impl_->module->capabilities, impl_->options.capability_grants);
  impl_->effects = effect::validate_effect_summaries(
      impl_->module->effects, impl_->options.allowed_effects,
      impl_->options.enforce_effects);
  impl_->schemas = data::validate_schemas(impl_->module->schemas,
                                          impl_->module->schema_migrations);
  impl_->table_plans = data::validate_table_plans(impl_->module->table_plans);
  impl_->wasm_components =
      wasm_accel::validate_wasm_components(impl_->module->wasm_components);
  impl_->accelerator_kernels = wasm_accel::validate_accelerator_kernels(
      impl_->module->accelerator_kernels);
  impl_->agent_metadata = modern::validate_agent_metadata(
      impl_->module->agent_symbols, impl_->module->agent_patches,
      impl_->module->provenance_records);
  impl_->contract_metadata = modern::validate_contract_metadata(
      impl_->module->contracts, impl_->module->properties);
  impl_->privacy_metadata = modern::validate_privacy_metadata(
      impl_->module->privacy_labels, impl_->module->privacy_policies,
      impl_->module->lineage_nodes);
  impl_->workflow_metadata = modern::validate_workflow_metadata(
      impl_->module->workflow_steps, impl_->module->workflow_history);

  result.ok = true;
  result.swapped = true;
  result.package_name = impl_->package->manifest.name;
  result.previous_version = result.previous_version.empty()
                                ? impl_->package->manifest.version
                                : result.previous_version;
  result.new_version = impl_->package->manifest.version;
  result.root_module = impl_->package->manifest.root_module;
  result.new_world_epoch = impl_->state->world_epoch;
  impl_->record_event(replay::make_event("loader.module.load",
                                         {{"module", result.root_module}}));
  impl_->record_event(
      replay::make_event("world.mutation", {{"package", result.package_name},
                                            {"version", result.new_version}}));
  return result;
}

RuntimeCapabilityCheckResult
RuntimeWorld::check_capability(const std::string &capability_name,
                               const std::string &target) const {
  RuntimeCapabilityCheckResult result;
  result.capability = capability_name;
  result.target = target;
  if (impl_ == nullptr || impl_->module == nullptr) {
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  impl_->record_event(
      replay::make_event("capability.check", {{"capability", capability_name},
                                              {"target", target}}));
  if (capability::capability_set_allows(impl_->capabilities.effective,
                                        capability_name, target)) {
    result.ok = true;
    return result;
  }
  result.error_name = "CapabilityError";
  result.message = "capability is not granted: " + capability_name;
  if (!target.empty()) {
    result.message += "=" + target;
  }
  impl_->record_event(
      replay::make_event("capability.denied", {{"capability", capability_name},
                                               {"target", target}}));
  return result;
}

RuntimeCapabilityResolution RuntimeWorld::capability_resolution() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->capabilities;
}

RuntimeEffectCheckResult
RuntimeWorld::check_effects(const std::vector<std::string> &requested) const {
  RuntimeEffectCheckResult result;
  result.effects = effect::normalize_effects(
      std::vector<std::string>(requested.begin(), requested.end()));
  if (impl_ == nullptr || impl_->module == nullptr) {
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  impl_->record_event(replay::make_event(
      "effect.boundary",
      {{"effects", effect::effect_row_to_text(result.effects)}}));
  if (!impl_->effects.ok && !impl_->effects.diagnostics.empty()) {
    result.error_name = "EffectViolationError";
    result.message = impl_->effects.diagnostics.front().message;
    return result;
  }
  for (const std::string &label : result.effects) {
    if (!effect::valid_effect_name(label)) {
      result.error_name = "EffectViolationError";
      result.message = "invalid effect label: " + label;
      return result;
    }
  }
  if (impl_->options.enforce_effects &&
      !effect::effects_subset_of(result.effects,
                                 impl_->options.allowed_effects)) {
    result.error_name = "EffectViolationError";
    result.message =
        "effect is not allowed: " + effect::effect_row_to_text(result.effects);
    return result;
  }
  result.ok = true;
  return result;
}

RuntimeEffectValidation RuntimeWorld::effect_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->effects;
}

RuntimeSchemaValidation RuntimeWorld::schema_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->schemas;
}

RuntimeTablePlanValidation RuntimeWorld::table_plan_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->table_plans;
}

RuntimeWasmValidation RuntimeWorld::wasm_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->wasm_components;
}

RuntimeAcceleratorValidation RuntimeWorld::accelerator_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->accelerator_kernels;
}

RuntimeAgentValidation RuntimeWorld::agent_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->agent_metadata;
}

RuntimeContractValidation RuntimeWorld::contract_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->contract_metadata;
}

RuntimePrivacyValidation RuntimeWorld::privacy_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->privacy_metadata;
}

RuntimeWorkflowValidation RuntimeWorld::workflow_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->workflow_metadata;
}

RuntimeTraceEvent RuntimeWorld::record_trace_event(RuntimeTraceEvent event) {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return event;
  }
  return impl_->record_event(std::move(event));
}

RuntimeReplayTrace RuntimeWorld::replay_trace() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return replay::normalize_trace(impl_->trace);
}

RuntimeReplayValidation RuntimeWorld::replay_validation() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return impl_->current_replay_validation();
}

std::uint64_t RuntimeWorld::world_epoch() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->world_epoch;
}

std::uint64_t RuntimeWorld::watch_epoch() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->watch_epoch;
}

std::vector<RuntimeWatchEvent> RuntimeWorld::watch_events() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  return impl_->state->watch_events;
}

void RuntimeWorld::begin_dependency_capture(std::uint64_t notebook_cell_id) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return;
  }
  impl_->state->begin_dependency_capture(notebook_cell_id);
}

RuntimeDependencySet RuntimeWorld::end_dependency_capture() {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  return impl_->state->end_dependency_capture();
}

RuntimeDependencySet RuntimeWorld::dependency_capture_snapshot() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  return impl_->state->dependency_capture_snapshot();
}

RuntimeWorldState RuntimeWorld::world_state() const {
  if (impl_ == nullptr || impl_->state == nullptr ||
      !impl_->state->world_frozen) {
    return RuntimeWorldState::Open;
  }
  return RuntimeWorldState::Frozen;
}

bool RuntimeWorld::is_world_frozen() const {
  return world_state() == RuntimeWorldState::Frozen;
}

std::uint64_t RuntimeWorld::method_version(std::uint32_t class_index) const {
  if (impl_ == nullptr || impl_->state == nullptr ||
      class_index >= impl_->state->classes.size()) {
    return 0;
  }
  return impl_->state->classes[class_index].method_version;
}

std::size_t RuntimeWorld::method_table_size(std::uint32_t class_index,
                                            MethodTableSide side) const {
  if (impl_ == nullptr || impl_->state == nullptr ||
      class_index >= impl_->state->classes.size()) {
    return 0;
  }
  const ClassRuntimeState &owner = impl_->state->classes[class_index];
  return side == MethodTableSide::Class
             ? owner.class_method_table.entries.size()
             : owner.instance_method_table.entries.size();
}

RuntimePackageMirror RuntimeWorld::package_mirror() const {
  if (impl_ == nullptr || impl_->module == nullptr) {
    return {};
  }
  return package_mirror_for(*impl_->module);
}

std::optional<RuntimeOwnerMirror>
RuntimeWorld::owner_mirror(std::uint32_t owner_index) const {
  if (impl_ == nullptr || impl_->module == nullptr || impl_->state == nullptr ||
      owner_index >= impl_->module->classes.size()) {
    return std::nullopt;
  }
  impl_->state->initialize_for_module(*impl_->module);
  return owner_mirror_for(*impl_->module, *impl_->state, owner_index);
}

std::optional<RuntimeOwnerMirror>
RuntimeWorld::class_mirror(std::uint32_t class_index) const {
  if (impl_ == nullptr || impl_->module == nullptr ||
      class_index >= impl_->module->classes.size() ||
      owner_kind_for_index(*impl_->module, class_index) !=
          RuntimeOwnerKind::Class) {
    return std::nullopt;
  }
  return owner_mirror(class_index);
}

std::optional<RuntimeOwnerMirror>
RuntimeWorld::mixin_mirror(std::uint32_t mixin_index) const {
  if (impl_ == nullptr || impl_->module == nullptr ||
      mixin_index >= impl_->module->classes.size() ||
      owner_kind_for_index(*impl_->module, mixin_index) !=
          RuntimeOwnerKind::Mixin) {
    return std::nullopt;
  }
  return owner_mirror(mixin_index);
}

RuntimeWorldMirror RuntimeWorld::world_mirror() const {
  RuntimeWorldMirror mirror;
  if (impl_ == nullptr || impl_->module == nullptr || impl_->state == nullptr) {
    return mirror;
  }
  impl_->state->initialize_for_module(*impl_->module);
  mirror.state = impl_->state->world_frozen ? RuntimeWorldState::Frozen
                                            : RuntimeWorldState::Open;
  mirror.world_epoch = impl_->state->world_epoch;
  mirror.watch_epoch = impl_->state->watch_epoch;
  mirror.package = package_mirror_for(*impl_->module);
  mirror.owners.reserve(impl_->module->classes.size());
  for (std::uint32_t index = 0; index < impl_->module->classes.size();
       ++index) {
    mirror.owners.push_back(
        owner_mirror_for(*impl_->module, *impl_->state, index));
  }
  return mirror;
}

RuntimeDispatchCacheStats RuntimeWorld::dispatch_cache_stats() const {
  RuntimeDispatchCacheStats stats;
  if (impl_ == nullptr || impl_->state == nullptr) {
    return stats;
  }
  stats.call_cache_entries =
      static_cast<std::uint64_t>(impl_->state->call_caches.size());
  stats.call_cache_hits = impl_->state->call_cache_hits;
  stats.call_cache_misses = impl_->state->call_cache_misses;
  stats.call_cache_updates = impl_->state->call_cache_updates;
  return stats;
}

RuntimeHeapStats RuntimeWorld::heap_stats() const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  return impl_->state->heap.stats();
}

std::uint64_t RuntimeWorld::drain_remote_frees() {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->heap.drain_remote_frees();
}

std::uint64_t RuntimeWorld::drain_remote_frees(std::uint64_t worker_id) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->heap.drain_remote_frees(worker_id);
}

RuntimeWriteBarrierResult RuntimeWorld::write_barrier(const Value &owner,
                                                      const Value &value) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeWriteBarrierResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.write_barrier(owner, value);
}

RuntimeGcResult RuntimeWorld::collect_garbage(const std::vector<Value> &roots,
                                              RuntimeGcCycle cycle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return {};
  }
  std::vector<Value> all_roots = roots;
  for (const auto &[name, value] : impl_->state->module_bindings) {
    (void)name;
    const Value root = unwrap_watch_value(value);
    if (value_has_heap_payload_tag(root)) {
      all_roots.push_back(root);
    }
  }
  for (const ClassRuntimeState &klass : impl_->state->classes) {
    for (const auto &[name, value] : klass.cvars) {
      (void)name;
      if (value_has_heap_payload_tag(value)) {
        all_roots.push_back(value);
      }
    }
  }
  runtime_append_task_local_gc_roots(&all_roots);
  return impl_->state->heap.collect_garbage(all_roots, cycle);
}

void RuntimeWorld::request_garbage_collection(RuntimeGcCycle cycle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return;
  }
  impl_->state->heap.request_garbage_collection(cycle);
}

RuntimePinResult RuntimeWorld::pin(const Value &value,
                                   RuntimePinViewKind view_kind,
                                   RuntimePinPermission permissions) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimePinResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.pin(value, view_kind, permissions);
}

RuntimeUnpinResult RuntimeWorld::unpin(RuntimePinToken *token) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeUnpinResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.unpin(token);
}

std::uint64_t RuntimeWorld::pin_count(const Value &value) const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    return 0;
  }
  return impl_->state->heap.pin_count(value);
}

bool RuntimeWorld::is_pinned(const Value &value) const {
  return pin_count(value) > 0;
}

RuntimeOpaqueHandleResult
RuntimeWorld::opaque_handle_for(const RuntimePinToken &token) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeOpaqueHandleResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.opaque_handle_for(token);
}

RuntimeOpaqueHandleResult
RuntimeWorld::release_opaque_handle(RuntimeOpaqueHandle *handle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeOpaqueHandleResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.release_opaque_handle(handle);
}

RuntimeOpaqueHandleResult
RuntimeWorld::resolve_opaque_handle(const RuntimeOpaqueHandle &handle) const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeOpaqueHandleResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.resolve_opaque_handle(handle);
}

RuntimeValueBufferViewResult
RuntimeWorld::value_buffer_view(const RuntimePinToken &token) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeValueBufferViewResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.value_buffer_view(token);
}

RuntimeNativeWaitResult
RuntimeWorld::register_native_wait(const RuntimePinToken &token) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeNativeWaitResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.register_native_wait(token);
}

RuntimeNativeWaitResult
RuntimeWorld::cancel_native_wait(RuntimeNativeWaitHandle *handle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeNativeWaitResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.cancel_native_wait(handle);
}

RuntimeNativeWaitResult
RuntimeWorld::poll_native_wait(const RuntimeNativeWaitHandle &handle) const {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeNativeWaitResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.poll_native_wait(handle);
}

RuntimeNativeWaitResult
RuntimeWorld::finish_native_wait(RuntimeNativeWaitHandle *handle) {
  if (impl_ == nullptr || impl_->state == nullptr) {
    RuntimeNativeWaitResult result;
    result.ok = false;
    result.error_name = "VMError";
    result.message = "runtime world is not bound";
    return result;
  }
  return impl_->state->heap.finish_native_wait(handle);
}

} // namespace amber::runtime
