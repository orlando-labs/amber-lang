#include "profile/wasm_accel.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "wasm_accel test failed: " << message << "\n";
    std::exit(1);
  }
}

void test_wasm_component_profile_document() {
  const std::string source = "schema: amber.wasm_accelerator.v1\n"
                             "wasm.count=1\n"
                             "wasm.0.name=analytics.plugin\n"
                             "wasm.0.world=analytics-plugin\n"
                             "wasm.0.frozen=true\n"
                             "wasm.0.raw_ffi=false\n"
                             "wasm.0.world_mutation=false\n"
                             "wasm.0.import.count=1\n"
                             "wasm.0.import.0.name=fs.read\n"
                             "wasm.0.import.0.kind=resource\n"
                             "wasm.0.import.0.type=resource\n"
                             "wasm.0.import.0.capability=fs.read:./data\n"
                             "wasm.0.export.count=1\n"
                             "wasm.0.export.0.name=normalize\n"
                             "wasm.0.export.0.kind=func\n"
                             "wasm.0.export.0.type=(Order) -> Order\n"
                             "wasm.0.export.0.schema=Order\n"
                             "wasm.0.export.0.effects=!{}\n";
  const amber::wasm_accel::WasmAccelDocumentParseResult parsed =
      amber::wasm_accel::parse_wasm_accel_document(source);
  expect(parsed.ok(), "wasm profile document should parse");
  const amber::wasm_accel::WasmComponentValidationResult validated =
      amber::wasm_accel::validate_wasm_components(parsed.document.components);
  expect(validated.ok, "wasm profile document should validate");
  expect(validated.components.size() == 1,
         "one wasm component should be exposed");
  expect(validated.components[0].imports[0].capability.name == "fs.read",
         "host import capability should be parsed");
  const std::string json =
      amber::wasm_accel::wasm_component_validation_to_json(validated);
  expect(json.find("\"schema\": \"amber.wasm_component.v1\"") !=
             std::string::npos,
         "wasm validation JSON should expose schema");
}

void test_wasm_component_rejects_world_mutation() {
  const std::string source = "schema: amber.wasm_accelerator.v1\n"
                             "wasm.count=1\n"
                             "wasm.0.name=bad.plugin\n"
                             "wasm.0.world=bad-world\n"
                             "wasm.0.world_mutation=true\n"
                             "wasm.0.export.count=1\n"
                             "wasm.0.export.0.name=normalize\n"
                             "wasm.0.export.0.kind=func\n"
                             "wasm.0.export.0.type=(Order) -> Order\n";
  const amber::wasm_accel::WasmAccelDocumentParseResult parsed =
      amber::wasm_accel::parse_wasm_accel_document(source);
  expect(parsed.ok(), "negative wasm document syntax should parse");
  const amber::wasm_accel::WasmComponentValidationResult validated =
      amber::wasm_accel::validate_wasm_components(parsed.document.components);
  expect(!validated.ok, "wasm profile should reject world mutation");
  expect(!validated.diagnostics.empty() &&
             validated.diagnostics[0].error_name == "WasmProfileError",
         "wasm rejection should be a profile diagnostic");
}

void test_accelerator_kernel_profile_document() {
  const std::string source = "schema: amber.wasm_accelerator.v1\n"
                             "kernel.count=1\n"
                             "kernel.0.id=scale.f32\n"
                             "kernel.0.entry=scale\n"
                             "kernel.0.target=gpu\n"
                             "kernel.0.effects=!{gpu}\n"
                             "kernel.0.param.count=2\n"
                             "kernel.0.param.0.name=xs\n"
                             "kernel.0.param.0.type=Tensor[F32]\n"
                             "kernel.0.param.0.space=device\n"
                             "kernel.0.param.1.name=factor\n"
                             "kernel.0.param.1.type=F32\n"
                             "kernel.0.param.1.space=scalar\n";
  const amber::wasm_accel::WasmAccelDocumentParseResult parsed =
      amber::wasm_accel::parse_wasm_accel_document(source);
  expect(parsed.ok(), "accelerator document should parse");
  const amber::wasm_accel::AcceleratorValidationResult validated =
      amber::wasm_accel::validate_accelerator_kernels(parsed.document.kernels);
  expect(validated.ok, "accelerator kernel should validate");
  expect(validated.kernels[0].params.size() == 2,
         "accelerator params should be exposed");
}

void test_accelerator_rejects_dynamic_dispatch() {
  const std::string source = "schema: amber.wasm_accelerator.v1\n"
                             "kernel.count=1\n"
                             "kernel.0.id=bad.kernel\n"
                             "kernel.0.entry=bad\n"
                             "kernel.0.target=gpu\n"
                             "kernel.0.effects=!{gpu}\n"
                             "kernel.0.dynamic_dispatch=true\n"
                             "kernel.0.param.count=1\n"
                             "kernel.0.param.0.name=xs\n"
                             "kernel.0.param.0.type=Tensor[F32]\n";
  const amber::wasm_accel::WasmAccelDocumentParseResult parsed =
      amber::wasm_accel::parse_wasm_accel_document(source);
  expect(parsed.ok(), "negative accelerator document syntax should parse");
  const amber::wasm_accel::AcceleratorValidationResult validated =
      amber::wasm_accel::validate_accelerator_kernels(parsed.document.kernels);
  expect(!validated.ok, "accelerator kernel should reject dynamic dispatch");
  expect(!validated.diagnostics.empty() &&
             validated.diagnostics[0].error_name == "AcceleratorError",
         "accelerator rejection should be an AcceleratorError");
}

} // namespace

int main() {
  test_wasm_component_profile_document();
  test_wasm_component_rejects_world_mutation();
  test_accelerator_kernel_profile_document();
  test_accelerator_rejects_dynamic_dispatch();
  std::cout << "wasm_accel_tests: ok\n";
  return 0;
}
