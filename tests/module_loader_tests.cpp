#include "runtime/module_loader.h"

#include "bytecode/format.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "module loader test failed: " << message << "\n";
    std::exit(1);
  }
}

std::vector<std::uint8_t>
serialized_module(const amber::bytecode::BcModule &module) {
  return amber::bytecode::serialize_module(module);
}

std::uint32_t append_string(amber::bytecode::BcModule *module,
                            const std::string &value) {
  module->strings.push_back(value);
  return static_cast<std::uint32_t>(module->strings.size() - 1U);
}

std::uint32_t append_symbol(amber::bytecode::BcModule *module,
                            const std::string &value) {
  module->symbols.push_back(value);
  return static_cast<std::uint32_t>(module->symbols.size() - 1U);
}

void add_dependency(amber::bytecode::BcModule *module,
                    const std::string &dep_name) {
  amber::bytecode::DepEntry dep;
  dep.module_name_str_id = append_string(module, dep_name);
  dep.required_format = {1, 0};
  dep.min_language_version = {1, 0};
  module->dependencies.push_back(dep);
}

void add_code_export(amber::bytecode::BcModule *module,
                     const std::string &public_name) {
  amber::bytecode::ExportEntry entry;
  entry.symbol_id = append_symbol(module, public_name);
  entry.target_kind_str_id = append_string(module, "code");
  entry.target_index = 1;
  entry.visibility_flags = 1;
  module->exports.push_back(entry);
}

void add_reexport(amber::bytecode::BcModule *module,
                  const std::string &public_name,
                  const std::string &dependency_name,
                  const std::string &source_name) {
  amber::bytecode::ExportEntry entry;
  entry.symbol_id = append_symbol(module, public_name);
  entry.target_kind_str_id = append_string(module, "reexport");
  entry.target_index = append_string(module, source_name);
  entry.visibility_flags = 1;
  entry.has_reexport_module_name = true;
  entry.reexport_module_name_str_id = append_string(module, dependency_name);
  module->exports.push_back(entry);
}

amber::bytecode::BcModule make_module(const std::vector<std::string> &deps,
                                      std::int64_t init_value,
                                      bool failing_init = false) {
  using namespace amber::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};

  for (const std::string &dep_name : deps) {
    add_dependency(&module, dep_name);
  }

  BcCode init;
  init.code_id = 1;
  init.kind = CodeKind::Module;
  init.reg_count = 1;

  Constant constant;
  if (failing_init) {
    const std::uint32_t error_name_id =
        static_cast<std::uint32_t>(module.strings.size());
    module.strings.push_back("BoomInit");
    constant.kind = ConstantKind::StringRef;
    constant.ref_id = error_name_id;
    module.const_pool.push_back(constant);
    init.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
    init.instructions.push_back({Opcode::Raise, {{0, false}}});
    init.instructions.push_back({Opcode::Return, {{0, false}}});
  } else {
    constant.kind = ConstantKind::Integer;
    constant.int_value = init_value;
    module.const_pool.push_back(constant);
    init.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
    init.instructions.push_back({Opcode::Return, {{0, false}}});
  }

  module.code_objects.push_back(init);
  module.init = {true, 1, 0};
  return module;
}

const amber::runtime::RuntimeModuleSnapshot &
snapshot_named(const amber::runtime::RuntimeModuleLoadResult &result,
               const std::string &name) {
  for (const amber::runtime::RuntimeModuleSnapshot &snapshot : result.modules) {
    if (snapshot.name == name) {
      return snapshot;
    }
  }
  std::cerr << "module loader test failed: missing snapshot for " << name
            << "\n";
  std::exit(1);
}

void add_ok(amber::runtime::RuntimeModuleLoader &loader,
            const std::string &name, const amber::bytecode::BcModule &module) {
  const amber::runtime::RuntimeModuleLoadResult added =
      loader.add_serialized_module(name, serialized_module(module));
  expect(added.ok, "module add failed for " + name + ": " + added.message);
}

void test_loader_initializes_dependencies_once_in_order() {
  amber::runtime::RuntimeModuleLoader loader;
  add_ok(loader, "app.main", make_module({"core.util", "core.base"}, 3));
  add_ok(loader, "core.base", make_module({}, 1));
  add_ok(loader, "core.util", make_module({"core.base"}, 2));

  const amber::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_module("app.main");
  expect(initialized.ok, "app.main initialization should succeed");
  expect(initialized.init_order.size() == 3, "expected three init records");
  expect(initialized.init_order[0] == "core.base",
         "base module should initialize first");
  expect(initialized.init_order[1] == "core.util",
         "util module should initialize after base");
  expect(initialized.init_order[2] == "app.main",
         "requested module should initialize last");

  expect(snapshot_named(initialized, "core.base").state ==
             amber::runtime::RuntimeModuleState::Ready,
         "base module should be ready");
  expect(snapshot_named(initialized, "core.util").init_runs == 1,
         "dependency init should run once");
  expect(snapshot_named(initialized, "app.main").init_runs == 1,
         "root init should run once");

  const amber::runtime::RuntimeModuleLoadResult second =
      loader.initialize_module("app.main");
  expect(second.ok, "second app.main initialization should succeed");
  expect(snapshot_named(second, "core.base").init_runs == 1,
         "base init should not rerun");
  expect(snapshot_named(second, "core.util").init_runs == 1,
         "util init should not rerun");
  expect(snapshot_named(second, "app.main").init_runs == 1,
         "root init should not rerun");
}

void test_loader_reports_missing_dependency() {
  amber::runtime::RuntimeModuleLoader loader;
  add_ok(loader, "app.main", make_module({"core.missing"}, 1));

  const amber::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(!linked.ok, "link should fail for missing dependency");
  expect(linked.error_name == "ImportError",
         "missing dependency should report ImportError");
  expect(linked.message.find("core.missing") != std::string::npos,
         "missing dependency message should name dependency");
}

void test_loader_rejects_unverified_bytecode() {
  amber::runtime::RuntimeModuleLoader loader;
  amber::bytecode::BcModule module = make_module({}, 1);
  module.init.entry_code_id = 99;

  const amber::runtime::RuntimeModuleLoadResult added =
      loader.add_serialized_module("bad.module", serialized_module(module));
  expect(!added.ok, "bad bytecode should be rejected at load time");
  expect(added.error_name == "BytecodeVerificationError",
         "bad bytecode should report BytecodeVerificationError");
  expect(snapshot_named(added, "bad.module").state ==
             amber::runtime::RuntimeModuleState::Failed,
         "bad module should snapshot as failed");
}

void test_loader_detects_init_cycles() {
  amber::runtime::RuntimeModuleLoader loader;
  add_ok(loader, "cycle.a", make_module({"cycle.b"}, 1));
  add_ok(loader, "cycle.b", make_module({"cycle.a"}, 2));

  const amber::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(linked.ok, "dependency cycles should link before init access");

  const amber::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_all();
  expect(!initialized.ok, "cyclic module init should fail");
  expect(initialized.error_name == "ModuleInitError",
         "cyclic module init should report ModuleInitError");
  expect(snapshot_named(initialized, "cycle.a").state ==
             amber::runtime::RuntimeModuleState::Failed,
         "cycle.a should fail");
  expect(snapshot_named(initialized, "cycle.b").state ==
             amber::runtime::RuntimeModuleState::Failed,
         "cycle.b should fail");
}

void test_loader_marks_failed_init() {
  amber::runtime::RuntimeModuleLoader loader;
  add_ok(loader, "bad.init", make_module({}, 1, true));

  const amber::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_module("bad.init");
  expect(!initialized.ok, "raising module init should fail");
  expect(initialized.error_name == "ModuleInitError",
         "raising module init should report ModuleInitError");
  const amber::runtime::RuntimeModuleSnapshot &snapshot =
      snapshot_named(initialized, "bad.init");
  expect(snapshot.state == amber::runtime::RuntimeModuleState::Failed,
         "raising init should mark module failed");
  expect(snapshot.init_runs == 0, "failed init should not count as successful");
  expect(snapshot.message.find("BoomInit") != std::string::npos,
         "failed init message should preserve VM fault");
}

void test_loader_materializes_exports_and_import_aliases() {
  amber::runtime::RuntimeModuleLoader loader;
  amber::bytecode::BcModule core = make_module({}, 42);
  add_code_export(&core, "Answer");
  amber::bytecode::BcModule app = make_module({"core.values"}, 1);

  add_ok(loader, "core.values", core);
  add_ok(loader, "app.main", app);
  const amber::runtime::RuntimeModuleLoadResult alias_added =
      loader.add_import_alias("app.main", "Answer", "core.values", "Answer");
  expect(alias_added.ok, "import alias registration should succeed");

  const amber::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(linked.ok, "export/import link should succeed");
  const std::optional<amber::runtime::RuntimeExportCellSnapshot> before =
      loader.export_snapshot("core.values", "Answer");
  expect(before.has_value(), "linked export cell should be visible");
  expect(before->state == amber::runtime::RuntimeExportCellState::Uninitialized,
         "linked export should be uninitialized before module init");

  const amber::runtime::RuntimeModuleLoadResult early_read =
      loader.read_import_alias("app.main", "Answer");
  expect(!early_read.ok, "early import alias read should fail");
  expect(early_read.error_name == "ModuleInitError",
         "early import alias read should report ModuleInitError");

  const amber::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_all();
  expect(initialized.ok, "export/import modules should initialize");
  const std::optional<amber::runtime::RuntimeImportAliasSnapshot> alias =
      loader.import_alias_snapshot("app.main", "Answer");
  expect(alias.has_value(), "import alias snapshot should be visible");
  expect(alias->read_only, "import alias should be read-only");
  expect(alias->export_cell.state ==
             amber::runtime::RuntimeExportCellState::Ready,
         "import alias should observe ready export cell after init");
  expect(alias->export_cell.resolved_module_name == "core.values",
         "import alias should resolve to exporting module");
}

void test_loader_reports_missing_export() {
  amber::runtime::RuntimeModuleLoader loader;
  add_ok(loader, "core.values", make_module({}, 1));
  add_ok(loader, "app.main", make_module({"core.values"}, 1));
  const amber::runtime::RuntimeModuleLoadResult alias_added =
      loader.add_import_alias("app.main", "Missing", "core.values", "Missing");
  expect(alias_added.ok,
         "missing export alias registration should be accepted");

  const amber::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(!linked.ok, "link should fail for missing export");
  expect(linked.error_name == "ImportError",
         "missing export should report ImportError");
  expect(linked.message.find("Missing") != std::string::npos,
         "missing export message should name export");
  expect(!linked.diagnostics.empty(),
         "missing export should include diagnostic");
  expect(linked.diagnostics[0].module_name == "app.main",
         "missing export diagnostic should name importer");
  expect(linked.diagnostics[0].dependency_name == "core.values",
         "missing export diagnostic should name dependency");
  expect(linked.diagnostics[0].export_name == "Missing",
         "missing export diagnostic should name export");
}

void test_loader_reports_version_and_abi_mismatch() {
  amber::runtime::RuntimeModuleLoader version_loader;
  amber::bytecode::BcModule versioned_app = make_module({"core.versioned"}, 1);
  versioned_app.dependencies[0].required_format = {1, 1};
  add_ok(version_loader, "core.versioned", make_module({}, 1));
  add_ok(version_loader, "app.versioned", versioned_app);

  const amber::runtime::RuntimeModuleLoadResult version_linked =
      version_loader.link();
  expect(!version_linked.ok, "link should fail for dependency format mismatch");
  expect(version_linked.error_name == "ImportError",
         "version mismatch should report ImportError");
  expect(version_linked.message.find("1.1") != std::string::npos,
         "version mismatch should include required version");

  amber::runtime::RuntimeModuleLoader abi_loader;
  amber::bytecode::BcModule abi_app = make_module({"core.abi"}, 1);
  abi_app.dependencies[0].has_abi_requirement = true;
  abi_app.dependencies[0].abi_requirement.fill(0xAB);
  add_ok(abi_loader, "core.abi", make_module({}, 1));
  add_ok(abi_loader, "app.abi", abi_app);

  const amber::runtime::RuntimeModuleLoadResult abi_linked = abi_loader.link();
  expect(!abi_linked.ok, "link should fail for dependency ABI mismatch");
  expect(abi_linked.error_name == "ImportError",
         "ABI mismatch should report ImportError");
  expect(abi_linked.message.find("ABI") != std::string::npos,
         "ABI mismatch should include ABI context");
}

void test_loader_rejects_unsupported_required_profile() {
  amber::runtime::RuntimeModuleLoader loader;
  amber::bytecode::BcModule module = make_module({}, 1);
  module.required_features = {"ffi.v1"};
  add_ok(loader, "profile.bad", module);

  const amber::runtime::RuntimeModuleLoadResult linked = loader.link();
  expect(!linked.ok, "link should fail for unsupported required profile");
  expect(linked.error_name == "UnsupportedProfileError",
         "unsupported profile should report UnsupportedProfileError");
  expect(linked.message.find("ffi.v1") != std::string::npos,
         "unsupported profile message should name feature");
}

void test_loader_resolves_reexport_chain() {
  amber::runtime::RuntimeModuleLoader loader;
  amber::bytecode::BcModule leaf = make_module({}, 1);
  add_code_export(&leaf, "Thing");

  amber::bytecode::BcModule facade = make_module({"core.leaf"}, 2);
  add_reexport(&facade, "Thing", "core.leaf", "Thing");

  amber::bytecode::BcModule app = make_module({"core.facade"}, 3);

  add_ok(loader, "core.leaf", leaf);
  add_ok(loader, "core.facade", facade);
  add_ok(loader, "app.main", app);
  const amber::runtime::RuntimeModuleLoadResult alias_added =
      loader.add_import_alias("app.main", "Thing", "core.facade", "Thing");
  expect(alias_added.ok, "re-export import alias registration should succeed");

  const amber::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_all();
  expect(initialized.ok, "re-export module chain should initialize");
  const std::optional<amber::runtime::RuntimeImportAliasSnapshot> alias =
      loader.import_alias_snapshot("app.main", "Thing");
  expect(alias.has_value(), "re-export alias snapshot should be visible");
  expect(alias->export_cell.has_reexport,
         "facade export should be marked as re-export");
  expect(alias->export_cell.resolved_module_name == "core.leaf",
         "re-export should resolve to leaf module");
  expect(alias->export_cell.resolved_export_name == "Thing",
         "re-export should resolve target export name");
  expect(alias->export_cell.state ==
             amber::runtime::RuntimeExportCellState::Ready,
         "re-export alias should observe ready leaf export");
}

void test_loader_reports_source_mapped_init_failure() {
  amber::runtime::RuntimeModuleLoader loader;
  amber::bytecode::BcModule module = make_module({}, 1, true);
  module.code_objects[0].source_spans.push_back(
      {1, 2, {"bad_init.am", {9, 3, 80}, {9, 12, 89}}});
  module.line_table.push_back({1, 1, 9});
  add_ok(loader, "bad.init", module);

  const amber::runtime::RuntimeModuleLoadResult initialized =
      loader.initialize_module("bad.init");
  expect(!initialized.ok, "source-mapped failing init should fail");
  expect(initialized.error_name == "ModuleInitError",
         "source-mapped failing init should report ModuleInitError");
  expect(!initialized.diagnostics.empty(),
         "source-mapped failing init should include diagnostic");
  expect(initialized.diagnostics[0].location.file == "bad_init.am",
         "loader diagnostic should preserve source file");
  expect(initialized.diagnostics[0].location.line == 9,
         "loader diagnostic should preserve source line");
  expect(initialized.message.find("bad_init.am") != std::string::npos,
         "loader message should include trace text");
}

} // namespace

int main() {
  test_loader_initializes_dependencies_once_in_order();
  test_loader_reports_missing_dependency();
  test_loader_rejects_unverified_bytecode();
  test_loader_detects_init_cycles();
  test_loader_marks_failed_init();
  test_loader_materializes_exports_and_import_aliases();
  test_loader_reports_missing_export();
  test_loader_reports_version_and_abi_mismatch();
  test_loader_rejects_unsupported_required_profile();
  test_loader_resolves_reexport_chain();
  test_loader_reports_source_mapped_init_failure();
  return 0;
}
