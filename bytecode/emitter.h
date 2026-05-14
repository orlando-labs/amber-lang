#pragma once

#include "bytecode/format.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/token.h"

#include <string>
#include <vector>

namespace amber::bytecode {

struct EmitResult {
  BcModule module;
  std::vector<lexer::Diagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

EmitResult emit_program(const hir::Program &program,
                        const std::string &module_name);

} // namespace amber::bytecode
