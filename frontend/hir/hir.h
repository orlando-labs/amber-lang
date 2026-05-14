#pragma once

#include "frontend/ast/expr.h"
#include "frontend/binder/binder.h"

#include <memory>
#include <string>
#include <vector>

namespace amber::hir {

struct ProcedureLocal {
  std::string slot;
  std::string name;
  std::string role;
  std::string binding_kind;
  lexer::Span span;
};

struct ProcedureCapture {
  std::string slot;
  std::string name;
  std::string source_kind;
  std::string source_slot;
  std::string source_name;
  lexer::Span span;
};

struct Procedure {
  std::string id;
  std::string name;
  std::string kind;
  std::string owner;
  lexer::Span span;
  std::unique_ptr<ast::Expr> signature;
  std::vector<std::unique_ptr<ast::Expr>> param_patterns;
  std::vector<ProcedureLocal> locals;
  std::vector<ProcedureCapture> captures;
  std::unique_ptr<ast::Expr> body;
};

struct Program {
  std::unique_ptr<ast::Expr> root;
  std::vector<Procedure> procedures;
};

Program lower_module(const std::vector<std::unique_ptr<ast::Expr>> &items,
                     const std::string &module_name,
                     const binder::BindGraph &bind_graph);

std::string program_to_json(const Program &program,
                            const std::string &module_name,
                            const std::string &source_hash);

} // namespace amber::hir
