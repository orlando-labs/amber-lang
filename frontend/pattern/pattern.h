#pragma once

#include "frontend/ast/expr.h"
#include "frontend/lexer/token.h"

#include <memory>
#include <string>
#include <vector>

namespace amber::pattern {

enum class PatternContext {
  General,
  Clause,
  Case,
  BlockParam,
  PatternAssignment
};

std::unique_ptr<ast::Expr> parse_pattern_text(const std::string &text,
                                              const lexer::Span &span);
std::unique_ptr<ast::Expr> parse_matcher_expression(const std::string &text,
                                                    const lexer::Span &span);
std::vector<std::string> collect_binding_names(const ast::Expr &pattern);
std::vector<std::string> collect_reference_names(const ast::Expr &pattern);
std::vector<lexer::Diagnostic>
validate_pattern(const ast::Expr &pattern,
                 PatternContext context = PatternContext::General);
std::unique_ptr<ast::Expr> compile_pattern_ir(const ast::Expr &pattern);
bool is_map_subject_pattern(const ast::Expr &pattern);
bool is_tuple_subject_pattern(const ast::Expr &pattern);

} // namespace amber::pattern
