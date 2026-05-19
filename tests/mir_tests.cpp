#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "optimizer/mir.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "mir test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::mir::Module lower_mir_ok(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    std::exit(1);
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::mir::Module module =
      amber::mir::lower_program(program, parse_result.module_name);
  amber::mir::ValidationResult validation = amber::mir::validate_module(module);
  if (!validation.ok()) {
    std::cerr << amber::mir::validation_errors_to_json(validation.errors);
    std::exit(1);
  }
  return module;
}

const amber::mir::Function *function_by_name(const amber::mir::Module &module,
                                             const std::string &name) {
  for (const amber::mir::Function &function : module.functions) {
    if (function.name == name) {
      return &function;
    }
  }
  return nullptr;
}

bool function_contains_op(const amber::mir::Function &function,
                          const std::string &op) {
  for (const amber::mir::Block &block : function.blocks) {
    for (const amber::mir::Instruction &instruction : block.instructions) {
      if (instruction.op == op) {
        return true;
      }
    }
    if (block.has_terminator && block.terminator.op == op) {
      return true;
    }
  }
  return false;
}

bool has_error_code(const std::vector<amber::mir::ValidationError> &errors,
                    const std::string &code) {
  for (const amber::mir::ValidationError &error : errors) {
    if (error.code == code) {
      return true;
    }
  }
  return false;
}

void test_hir_to_mir_if_ssa() {
  const amber::mir::Module module = lower_mir_ok("def choose(x):\n"
                                                 "  if x > 0:\n"
                                                 "    x\n"
                                                 "  else:\n"
                                                 "    0\n");
  const amber::mir::Function *choose = function_by_name(module, "choose");
  expect(choose != nullptr, "choose function is lowered");
  expect(choose->entry_block == "bb0", "entry block is stable");
  expect(function_contains_op(*choose, "send"), "binary op lowers to send");
  expect(function_contains_op(*choose, "branch_if"), "if lowers to branch_if");
  expect(function_contains_op(*choose, "phi"), "if result lowers to phi");

  const std::string dump = amber::mir::module_to_dump(module, "abc123");
  expect(dump.find("amber.mir.v1") != std::string::npos,
         "dump includes format");
  expect(dump.find("func @") != std::string::npos, "dump includes function");
  expect(dump.find("source=sha256:abc123") != std::string::npos,
         "dump includes source hash");
}

void test_closure_capture_operands_use_parent_slots() {
  const amber::mir::Module module = lower_mir_ok("def offsetter(xs, delta):\n"
                                                 "  xs.map: _1 + delta\n");
  const amber::mir::Function *offsetter = function_by_name(module, "offsetter");
  expect(offsetter != nullptr, "offsetter function is lowered");
  expect(function_contains_op(*offsetter, "closure.make"),
         "closure creation is represented");
}

void test_validator_rejects_duplicate_ssa_definition() {
  amber::mir::Module module;
  amber::mir::Function function;
  function.id = "p0";
  function.name = "bad";
  function.kind = "method";
  function.entry_block = "bb0";

  amber::mir::Block block;
  block.id = "bb0";
  amber::mir::Instruction first;
  first.result = "%v0";
  first.op = "const";
  amber::mir::Instruction second = first;
  block.instructions.push_back(first);
  block.instructions.push_back(second);
  block.terminator.op = "return";
  block.terminator.operands.push_back(amber::mir::value_operand("%v0"));
  block.has_terminator = true;
  function.blocks.push_back(block);
  module.functions.push_back(function);

  const amber::mir::ValidationResult validation =
      amber::mir::validate_module(module);
  expect(!validation.ok(), "duplicate SSA definition is rejected");
  expect(has_error_code(validation.errors, "MIR1004"),
         "duplicate definition code is stable");
}

void test_pass_harness_phase_order_and_validation() {
  amber::mir::Module module = lower_mir_ok("def one():\n"
                                           "  1\n");

  amber::mir::Pass noop;
  noop.name = "noop";
  noop.phase_order = 10;
  noop.invalidates = amber::mir::kInvalidatesAnalyses;
  noop.run = [](amber::mir::Module &) {};

  amber::mir::Pass second;
  second.name = "second";
  second.phase_order = 20;
  second.run = [](amber::mir::Module &) {};

  amber::mir::PassPipelineResult ok =
      amber::mir::run_pass_pipeline(module, {noop, second});
  expect(ok.ok(), "ordered preserving passes run");
  expect(module.pass_log.size() == 2, "pass records are attached to module");

  amber::mir::Module order_module = lower_mir_ok("def two():\n"
                                                 "  2\n");
  amber::mir::PassPipelineResult order_error =
      amber::mir::run_pass_pipeline(order_module, {second, noop});
  expect(!order_error.ok(), "out-of-order passes are rejected");
  expect(has_error_code(order_error.errors, "MIR2001"),
         "phase-order error code is stable");

  amber::mir::Module invalid_module = lower_mir_ok("def three():\n"
                                                   "  3\n");
  amber::mir::Pass duplicate;
  duplicate.name = "duplicate-result";
  duplicate.phase_order = 30;
  duplicate.run = [](amber::mir::Module &edited) {
    for (amber::mir::Function &function : edited.functions) {
      if (function.name != "three") {
        continue;
      }
      for (amber::mir::Block &block : function.blocks) {
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
          if (!block.instructions[i].result.empty()) {
            amber::mir::Instruction copy = block.instructions[i];
            block.instructions.push_back(copy);
            return;
          }
        }
      }
    }
  };
  amber::mir::PassPipelineResult invalid =
      amber::mir::run_pass_pipeline(invalid_module, {duplicate});
  expect(!invalid.ok(), "post-pass SSA validation runs");
  expect(has_error_code(invalid.errors, "MIR1004"),
         "post-pass duplicate definition is reported");
}

} // namespace

int main() {
  test_hir_to_mir_if_ssa();
  test_closure_capture_operands_use_parent_slots();
  test_validator_rejects_duplicate_ssa_definition();
  test_pass_harness_phase_order_and_validation();
  return 0;
}
