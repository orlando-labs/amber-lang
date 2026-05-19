#pragma once

#include "frontend/hir/hir.h"
#include "frontend/lexer/token.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace amber::mir {

struct Operand {
  std::string kind;
  std::string value;
};

struct Attribute {
  std::string key;
  std::string value;
};

struct Instruction {
  std::string result;
  std::string op;
  std::vector<Operand> operands;
  std::vector<Attribute> attrs;
  lexer::Span span;
};

struct Terminator {
  std::string op;
  std::vector<Operand> operands;
  std::vector<std::string> targets;
  lexer::Span span;
};

struct Block {
  std::string id;
  std::vector<Instruction> instructions;
  Terminator terminator;
  bool has_terminator = false;
};

struct Local {
  std::string slot;
  std::string name;
  std::string role;
  std::string binding_kind;
};

struct Capture {
  std::string slot;
  std::string name;
  std::string source_kind;
  std::string source_slot;
  std::string source_name;
};

struct Function {
  std::string id;
  std::string name;
  std::string kind;
  std::string owner;
  std::string entry_block;
  std::vector<Local> locals;
  std::vector<Capture> captures;
  std::vector<Block> blocks;
};

struct PassRecord {
  std::string name;
  std::uint32_t phase_order = 0;
  std::uint32_t invalidates = 0;
};

struct Module {
  std::string module_name;
  std::vector<Function> functions;
  std::vector<PassRecord> pass_log;
};

struct ValidationError {
  std::string code;
  std::string message;
  std::string function_id;
  std::string block_id;
};

struct ValidationResult {
  std::vector<ValidationError> errors;

  bool ok() const { return errors.empty(); }
};

inline constexpr std::uint32_t kInvalidatesControlFlow = 0x1U;
inline constexpr std::uint32_t kInvalidatesSsa = 0x2U;
inline constexpr std::uint32_t kInvalidatesAnalyses = 0x4U;

struct Pass {
  std::string name;
  std::uint32_t phase_order = 0;
  std::uint32_t invalidates = 0;
  bool requires_valid_ssa = true;
  bool preserves_valid_ssa = true;
  std::function<void(Module &)> run;
};

struct PassPipelineResult {
  std::vector<PassRecord> records;
  std::vector<ValidationError> errors;

  bool ok() const { return errors.empty(); }
};

Operand value_operand(std::string value);
Operand local_operand(std::string slot);
Operand capture_operand(std::string slot);
Operand const_operand(std::string value);
Operand symbol_operand(std::string value);
Operand block_operand(std::string value);
Operand procedure_operand(std::string value);
Operand text_operand(std::string value);

Module lower_program(const hir::Program &program,
                     const std::string &module_name);
ValidationResult validate_module(const Module &module);
PassPipelineResult run_pass_pipeline(Module &module,
                                     const std::vector<Pass> &passes);

std::string module_to_json(const Module &module,
                           const std::string &source_hash);
std::string module_to_dump(const Module &module,
                           const std::string &source_hash);
std::string
validation_errors_to_json(const std::vector<ValidationError> &errors);

} // namespace amber::mir
