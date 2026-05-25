#include "bytecode/format.h"

#include "frontend/lexer/token.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "bytecode test failed: " << message << "\n";
    std::exit(1);
  }
}

bool has_error_code(const amber::bytecode::DecodeResult &result,
                    const std::string &code) {
  for (const amber::bytecode::VerifyError &error : result.errors) {
    if (error.code == code) {
      return true;
    }
  }
  return false;
}

std::string path_constant_text(const amber::bytecode::BcModule &module,
                               std::uint32_t ref_id) {
  expect(ref_id < module.const_pool.size(), "path ref in range");
  const amber::bytecode::Constant &constant = module.const_pool[ref_id];
  expect(constant.kind == amber::bytecode::ConstantKind::Path,
         "expected path constant");
  std::string out;
  for (std::size_t i = 0; i < constant.items.size(); ++i) {
    expect(constant.items[i] < module.symbols.size(), "path symbol in range");
    if (i != 0U) {
      out += ".";
    }
    out += module.symbols[constant.items[i]];
  }
  return out;
}

amber::bytecode::BcModule sample_module() {
  using namespace amber::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};
  module.profile_flags = 1;
  module.file_flags = 2;
  for (std::size_t i = 0; i < module.abi_hash.size(); ++i) {
    module.abi_hash[i] = static_cast<std::uint8_t>(i);
  }

  module.strings = {"x",        "param",      "local", "method",
                    "std/core", "build.mode", "debug"};
  module.symbols = {"compute"};
  Constant integer_constant;
  integer_constant.kind = ConstantKind::Integer;
  integer_constant.int_value = 42;
  Constant string_ref_constant;
  string_ref_constant.kind = ConstantKind::StringRef;
  string_ref_constant.ref_id = 0;
  Constant symbol_ref_constant;
  symbol_ref_constant.kind = ConstantKind::SymbolRef;
  symbol_ref_constant.ref_id = 0;
  module.const_pool = {integer_constant, string_ref_constant,
                       symbol_ref_constant};

  BcCode code;
  code.code_id = 7;
  code.kind = CodeKind::Method;
  code.reg_count = 2;
  code.local_layout.push_back({0, 0, 1, 2});
  code.instructions.push_back({Opcode::GetLast, {}});
  code.instructions.push_back({Opcode::Jump, {InstructionOperand{0, false}}});
  code.instructions.push_back({Opcode::Return, {}});
  code.safepoint_table.push_back({0, 0});
  code.source_spans.push_back(
      {0, 2, {"/tmp/sample.am", {1, 1, 0}, {1, 14, 13}}});
  module.code_objects.push_back(code);

  BcMethod method;
  method.selector_sym_id = 0;
  method.signature_blob_id = 0;
  method.clause_table.push_back({1, 7, 7, 7, 0});
  method.entry_code_id = 7;
  module.methods.push_back(method);

  amber::bytecode::DepEntry dependency;
  dependency.module_name_str_id = 4;
  dependency.required_format = {1, 0};
  dependency.min_language_version = {1, 0};
  module.dependencies.push_back(dependency);
  module.exports.push_back({0, 3, 0, 1});
  module.init = {true, 7, 0};
  module.pattern_programs.push_back({1, 1, 0});
  module.line_table.push_back({7, 0, 1});
  module.line_table.push_back({7, 2, 2});
  module.local_debug.push_back({7, 0, 0, 0, 3});
  module.attrs.push_back({5, 6});
  module.capabilities.push_back(
      amber::capability::make_capability("fs.read", "./data"));
  module.effects.push_back(amber::effect::make_effect_summary(
      "compute", "function", {"fs"}, {"fs"}, true));
  module.observability_sites.push_back(
      {1,
       "task.started",
       "runtime",
       "compute",
       {"sample.am", 1, 1},
       amber::replay::kObservabilitySiteFlagRequired});
  module.replay_metadata.required_event_names = {"task.started",
                                                 "task.completed"};
  module.replay_metadata.deterministic_sources = {"time", "random"};
  module.replay_metadata.flags =
      amber::replay::kReplayMetadataFlagDeterministic;
  amber::data::SchemaDefinition order_v1;
  order_v1.name = "Order";
  order_v1.version = 1;
  order_v1.fields.push_back({"id",
                             "integer",
                             true,
                             false,
                             {},
                             amber::data::kSchemaFieldFlagPrimaryKey});
  order_v1.fields.push_back({"amount", "float", true, false, {}, 0});
  amber::data::SchemaDefinition order_v2 = order_v1;
  order_v2.version = 2;
  order_v2.fields.push_back({"status", "string", false, false, "new", 0});
  module.schemas = {order_v1, order_v2};
  module.schema_migrations.push_back(
      {"Order", 1, 2, "compatible",
       amber::data::kSchemaMigrationFlagCompatible});
  amber::data::TablePlan high_value;
  high_value.plan_id = "orders.high_value";
  high_value.op = "filter";
  high_value.input_refs = {"orders"};
  high_value.arguments = {"amount > 100"};
  high_value.column_dependencies = {{"orders", "amount"}};
  high_value.effect_row = {};
  high_value.flags = amber::data::kTablePlanFlagLazy;
  module.table_plans.push_back(high_value);
  amber::wasm_accel::WasmInterfaceEntry import_entry;
  import_entry.name = "fs.read";
  import_entry.kind = "resource";
  import_entry.type_signature = "resource";
  import_entry.capability =
      amber::capability::make_capability("fs.read", "./data");
  amber::wasm_accel::WasmInterfaceEntry export_entry;
  export_entry.name = "normalize";
  export_entry.kind = "func";
  export_entry.type_signature = "(Order) -> Order";
  export_entry.schema_name = "Order";
  amber::wasm_accel::WasmComponent component;
  component.name = "analytics.plugin";
  component.world = "analytics-plugin";
  component.flags = amber::wasm_accel::kWasmComponentFlagFrozenWorld |
                    amber::wasm_accel::kWasmComponentFlagRawFfiDenied |
                    amber::wasm_accel::kWasmComponentFlagWorldMutationDenied;
  component.imports.push_back(import_entry);
  component.exports.push_back(export_entry);
  module.wasm_components.push_back(component);
  amber::wasm_accel::AcceleratorKernel kernel;
  kernel.kernel_id = "scale.f32";
  kernel.entry = "scale";
  kernel.target = "gpu";
  kernel.effect_row = {"gpu"};
  kernel.params.push_back({"xs", "Tensor[F32]", "device", 0});
  kernel.params.push_back({"factor", "F32", "scalar", 0});
  kernel.flags = amber::wasm_accel::kAcceleratorKernelFlagPureHelpersOnly;
  module.accelerator_kernels.push_back(kernel);
  module.hashes.push_back(
      {SectionKind::Code, std::vector<std::uint8_t>(32, 0xAB)});
  return module;
}

std::string bytes_hash(const std::vector<std::uint8_t> &bytes) {
  const std::string binary(bytes.begin(), bytes.end());
  return amber::lexer::sha256_hex(binary);
}

void test_round_trip_and_dump() {
  const amber::bytecode::BcModule module = sample_module();
  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(module);
  amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(bytes);
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));

  const std::string artifact_hash = bytes_hash(bytes);
  const std::string dump1 = amber::bytecode::module_to_json(
      decoded.module, decoded.sections, artifact_hash);
  const std::vector<std::uint8_t> bytes2 =
      amber::bytecode::serialize_module(decoded.module);
  expect(bytes == bytes2,
         "serialize -> deserialize -> serialize is not stable");

  amber::bytecode::DecodeResult decoded2 =
      amber::bytecode::deserialize_module(bytes2);
  expect(decoded2.ok(),
         amber::bytecode::verify_errors_to_json(decoded2.errors));
  const std::string dump2 = amber::bytecode::module_to_json(
      decoded2.module, decoded2.sections, bytes_hash(bytes2));
  expect(dump1 == dump2, "JSON dump changed across round-trip");
  expect(decoded.sections.size() == 23,
         "expected required and optional sections");
}

void test_disasm_is_stable() {
  const amber::bytecode::BcModule module = sample_module();
  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(module);
  amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(bytes);
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));

  const std::string disasm = amber::bytecode::module_to_disasm(
      decoded.module, decoded.sections, bytes_hash(bytes));
  expect(disasm.find(".header format=1.0 language=1.0") != std::string::npos,
         "missing header line in disasm");
  expect(disasm.find("0001 JUMP 0") != std::string::npos,
         "missing canonical jump line in disasm");
  expect(
      disasm.find("export y0(compute) kind=s3(method) target=0 visibility=1") !=
          std::string::npos,
      "missing export line in disasm");
  expect(disasm.find(".hash\n  CODE sha256=") != std::string::npos,
         "missing hash section in disasm");
  expect(disasm.find(".caps\n  fs.read target=\"./data\"") != std::string::npos,
         "missing capability section in disasm");
  expect(disasm.find(".efct\n  compute kind=\"function\" declared=\"!{fs}\"") !=
             std::string::npos,
         "missing effect section in disasm");
  expect(disasm.find(".obsv\n  site=1 event=\"task.started\"") !=
             std::string::npos,
         "missing observability section in disasm");
  expect(disasm.find(".rply flags=1") != std::string::npos &&
             disasm.find("deterministic_source=\"random\"") !=
                 std::string::npos,
         "missing replay section in disasm");
  expect(disasm.find(".scma\n  schema Order version=1") != std::string::npos &&
             disasm.find("migration Order 1->2 kind=\"compatible\"") !=
                 std::string::npos,
         "missing schema section in disasm");
  expect(disasm.find(".tabl\n  plan orders.high_value op=\"filter\"") !=
                 std::string::npos &&
             disasm.find("dep=\"orders.amount\"") != std::string::npos,
         "missing table section in disasm");
  expect(disasm.find(".wasm\n  component analytics.plugin") !=
                 std::string::npos &&
             disasm.find("export normalize kind=\"func\"") != std::string::npos,
         "missing wasm section in disasm");
  expect(disasm.find(".accl\n  kernel scale.f32 entry=\"scale\"") !=
                 std::string::npos &&
             disasm.find("param xs type=\"Tensor[F32]\"") != std::string::npos,
         "missing accelerator section in disasm");
}

void test_bad_magic_rejected() {
  std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(sample_module());
  bytes[0] = 'X';
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(bytes);
  expect(!decoded.ok(), "bad magic unexpectedly decoded");
  expect(has_error_code(decoded, "BC1001"), "expected BC1001 for bad magic");
}

void test_missing_required_section_rejected() {
  std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(sample_module());
  expect(bytes.size() > 60, "fixture too small for section directory mutation");
  bytes[56] = 'X';
  bytes[57] = 'X';
  bytes[58] = 'X';
  bytes[59] = 'X';
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(bytes);
  expect(!decoded.ok(), "unknown section tag unexpectedly decoded");
  expect(has_error_code(decoded, "BC1105"),
         "expected BC1105 for unknown section");
  expect(has_error_code(decoded, "BC1102"),
         "expected BC1102 for missing required section");
}

void test_invalid_code_ref_rejected() {
  amber::bytecode::BcModule module = sample_module();
  module.init.entry_code_id = 99;
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid init code ref unexpectedly decoded");
  expect(has_error_code(decoded, "BC1204"), "expected BC1204 for bad code ref");
}

void test_back_edge_requires_safepoint() {
  amber::bytecode::BcModule module = sample_module();
  module.code_objects[0].safepoint_table.clear();
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(module));
  expect(!decoded.ok(), "missing safepoint unexpectedly accepted");
  expect(has_error_code(decoded, "BC1303"),
         "expected BC1303 for back-edge without safepoint");
}

void test_class_descriptor_round_trip() {
  using namespace amber::bytecode;

  BcModule module = sample_module();
  module.symbols.push_back("Trackable");
  module.symbols.push_back("Owner");
  module.symbols.push_back("Base");
  module.symbols.push_back("Serializable");

  Constant empty_keyset;
  empty_keyset.kind = ConstantKind::KeySet;
  const std::uint32_t keyset_id =
      static_cast<std::uint32_t>(module.const_pool.size());
  module.const_pool.push_back(empty_keyset);

  Constant trackable_path;
  trackable_path.kind = ConstantKind::Path;
  trackable_path.items = {1};
  const std::uint32_t trackable_path_id =
      static_cast<std::uint32_t>(module.const_pool.size());
  module.const_pool.push_back(trackable_path);

  Constant base_path;
  base_path.kind = ConstantKind::Path;
  base_path.items = {3};
  const std::uint32_t base_path_id =
      static_cast<std::uint32_t>(module.const_pool.size());
  module.const_pool.push_back(base_path);

  Constant serializable_path;
  serializable_path.kind = ConstantKind::Path;
  serializable_path.items = {4};
  const std::uint32_t serializable_path_id =
      static_cast<std::uint32_t>(module.const_pool.size());
  module.const_pool.push_back(serializable_path);

  BcClass mixin;
  mixin.class_name_sym_id = 1;
  mixin.ivar_schema_id = keyset_id;
  mixin.flags = kClassFlagMixin;

  BcClass klass;
  klass.class_name_sym_id = 2;
  klass.has_superclass_ref = true;
  klass.superclass_ref = base_path_id;
  klass.ivar_schema_id = keyset_id;
  klass.direct_include_refs.push_back(trackable_path_id);
  klass.direct_extend_refs.push_back(serializable_path_id);

  module.classes = {mixin, klass};

  const std::vector<std::uint8_t> bytes = serialize_module(module);
  const DecodeResult decoded = deserialize_module(bytes);
  expect(decoded.ok(), verify_errors_to_json(decoded.errors));
  expect(decoded.module.classes.size() == 2, "expected two class descriptors");
  expect((decoded.module.classes[0].flags & kClassFlagMixin) != 0U,
         "mixin flag preserved");
  expect(decoded.module.classes[1].has_superclass_ref,
         "superclass ref preserved");
  expect(path_constant_text(decoded.module,
                            decoded.module.classes[1].superclass_ref) == "Base",
         "superclass path round-trips");
  expect(decoded.module.classes[1].direct_include_refs.size() == 1,
         "include refs round-trip");
  expect(path_constant_text(decoded.module,
                            decoded.module.classes[1].direct_include_refs[0]) ==
             "Trackable",
         "include path round-trips");
  expect(decoded.module.classes[1].direct_extend_refs.size() == 1,
         "extend refs round-trip");
  expect(path_constant_text(decoded.module,
                            decoded.module.classes[1].direct_extend_refs[0]) ==
             "Serializable",
         "extend path round-trips");
}

void test_invalid_class_path_ref_rejected() {
  using namespace amber::bytecode;

  BcModule module = sample_module();
  Constant empty_keyset;
  empty_keyset.kind = ConstantKind::KeySet;
  const std::uint32_t keyset_id =
      static_cast<std::uint32_t>(module.const_pool.size());
  module.const_pool.push_back(empty_keyset);

  BcClass klass;
  klass.class_name_sym_id = 0;
  klass.has_superclass_ref = true;
  klass.superclass_ref = 0;
  klass.ivar_schema_id = keyset_id;
  module.classes.push_back(klass);

  const DecodeResult decoded = deserialize_module(serialize_module(module));
  expect(!decoded.ok(), "non-path superclass unexpectedly accepted");
  expect(has_error_code(decoded, "BC1206"),
         "expected BC1206 for non-path superclass ref");
}

void test_invalid_schema_metadata_rejected() {
  amber::bytecode::BcModule module = sample_module();
  module.schemas[0].fields[0].type = "not a type";
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid schema metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1406"),
         "expected BC1406 for invalid schema metadata");
}

void test_invalid_table_plan_metadata_rejected() {
  amber::bytecode::BcModule module = sample_module();
  module.table_plans[0].op = "teleport";
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid table plan metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1407"),
         "expected BC1407 for invalid table metadata");
}

void test_invalid_wasm_metadata_rejected() {
  amber::bytecode::BcModule module = sample_module();
  module.wasm_components[0].flags &=
      ~amber::wasm_accel::kWasmComponentFlagWorldMutationDenied;
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid wasm metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1408"),
         "expected BC1408 for invalid wasm metadata");
}

void test_invalid_accelerator_metadata_rejected() {
  amber::bytecode::BcModule module = sample_module();
  module.accelerator_kernels[0].forbidden_features.push_back(
      "dynamic_dispatch");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(module));
  expect(!decoded.ok(), "invalid accelerator metadata unexpectedly accepted");
  expect(has_error_code(decoded, "BC1409"),
         "expected BC1409 for invalid accelerator metadata");
}

} // namespace

int main() {
  test_round_trip_and_dump();
  test_disasm_is_stable();
  test_bad_magic_rejected();
  test_missing_required_section_rejected();
  test_invalid_code_ref_rejected();
  test_back_edge_requires_safepoint();
  test_class_descriptor_round_trip();
  test_invalid_class_path_ref_rejected();
  test_invalid_schema_metadata_rejected();
  test_invalid_table_plan_metadata_rejected();
  test_invalid_wasm_metadata_rejected();
  test_invalid_accelerator_metadata_rejected();
  std::cout << "bytecode_tests: ok\n";
  return 0;
}
