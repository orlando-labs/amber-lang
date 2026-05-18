#pragma once

#include "frontend/ast/expr.h"
#include "frontend/binder/binder.h"
#include "frontend/lexer/token.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace amber::checker {

struct TypeTerm {
  std::string kind;
  std::string name;
  std::vector<TypeTerm> args;
  std::vector<std::pair<std::string, TypeTerm>> fields;
  bool exact_record = false;
};

struct TypeParseResult {
  TypeTerm term;
  std::vector<lexer::Diagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

struct ParamBoundary {
  std::string name;
  std::string kind;
  std::string type;
  bool has_default = false;
};

struct CallableBoundary {
  std::string owner;
  std::string kind;
  bool exported = false;
  std::string return_type;
  std::string observed_return_type;
  std::vector<ParamBoundary> params;
  std::vector<std::string> type_hooks;
};

struct CheckResult {
  std::vector<CallableBoundary> boundaries;
  std::vector<lexer::Diagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

TypeParseResult parse_type_term(const std::string &source,
                                const lexer::Span &span);
std::string type_term_to_string(const TypeTerm &term);

CheckResult check_module(const std::vector<std::unique_ptr<ast::Expr>> &items,
                         const std::string &module_name,
                         const binder::BindGraph &bind_graph);

std::string check_result_to_json(const CheckResult &result,
                                 const std::string &module_name);

} // namespace amber::checker
