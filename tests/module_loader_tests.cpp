#include "runtime/module_loader.h"

#include "bytecode/format.h"

#include <cstdlib>
#include <iostream>
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

amber::bytecode::BcModule make_module(const std::vector<std::string> &deps,
                                      std::int64_t init_value,
                                      bool failing_init = false) {
  using namespace amber::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};

  for (const std::string &dep_name : deps) {
    const std::uint32_t dep_name_id =
        static_cast<std::uint32_t>(module.strings.size());
    module.strings.push_back(dep_name);
    DepEntry dep;
    dep.module_name_str_id = dep_name_id;
    dep.required_format = {1, 0};
    dep.min_language_version = {1, 0};
    module.dependencies.push_back(dep);
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

} // namespace

int main() {
  test_loader_initializes_dependencies_once_in_order();
  test_loader_reports_missing_dependency();
  test_loader_rejects_unverified_bytecode();
  test_loader_detects_init_cycles();
  test_loader_marks_failed_init();
  return 0;
}
