#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

using amber::ast::Expr;

std::unique_ptr<Expr> parse_ok(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseResult parse_result = parser.parse_expression_unit();
  if (!parse_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }
  return std::move(parse_result.expr);
}

const Expr &node_field(const Expr &expr, const std::string &name) {
  for (const amber::ast::NodeField &field : expr.node_fields) {
    if (field.name == name) {
      return *field.value;
    }
  }
  std::cerr << "missing node field " << name << " on " << expr.kind << "\n";
  std::exit(1);
}

const std::string &string_field(const Expr &expr, const std::string &name) {
  for (const amber::ast::StringField &field : expr.string_fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  std::cerr << "missing string field " << name << " on " << expr.kind << "\n";
  std::exit(1);
}

bool bool_field(const Expr &expr, const std::string &name) {
  for (const amber::ast::BoolField &field : expr.bool_fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  std::cerr << "missing bool field " << name << " on " << expr.kind << "\n";
  std::exit(1);
}

const amber::ast::ListField &list_field(const Expr &expr,
                                        const std::string &name) {
  for (const amber::ast::ListField &field : expr.list_fields) {
    if (field.name == name) {
      return field;
    }
  }
  std::cerr << "missing list field " << name << " on " << expr.kind << "\n";
  std::exit(1);
}

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "parser test failed: " << message << "\n";
    std::exit(1);
  }
}

void test_precedence() {
  std::unique_ptr<Expr> expr = parse_ok("x + 1 * 2\n");
  expect(expr->kind == "AstBinary", "top-level binary");
  expect(string_field(*expr, "op") == "+", "addition remains outer op");
  expect(node_field(*expr, "right").kind == "AstBinary",
         "multiplication binds tighter");
  expect(string_field(node_field(*expr, "right"), "op") == "*",
         "multiplication op");
}

void test_bare_call() {
  std::unique_ptr<Expr> expr = parse_ok("puts x + 1\n");
  expect(expr->kind == "AstPostfixChain", "bare call produces postfix chain");
  const amber::ast::ListField &tails = list_field(*expr, "tails");
  expect(tails.values.size() == 1, "one bare call tail");
  expect(tails.values[0]->kind == "AstTailCall", "bare call tail kind");
  expect(string_field(*tails.values[0], "call_style") == "bare",
         "bare call style");
  expect(list_field(*tails.values[0], "args").values.size() == 1,
         "one bare arg");
  expect(list_field(*tails.values[0], "args").values[0]->kind == "AstBinary",
         "bare arg parses as expression");
}

void test_safe_nav_and_index() {
  std::unique_ptr<Expr> expr = parse_ok("obj.?.items[0].?.name\n");
  expect(expr->kind == "AstPostfixChain", "safe nav postfix chain");
  const amber::ast::ListField &tails = list_field(*expr, "tails");
  expect(tails.values.size() == 3, "safe nav tail count");
  expect(tails.values[0]->kind == "AstTailSafeMember", "safe member tail");
  expect(string_field(*tails.values[0], "name") == "items", "safe member name");
  expect(tails.values[1]->kind == "AstTailIndex", "index tail");
  expect(tails.values[2]->kind == "AstTailSafeMember",
         "second safe member tail");
  expect(string_field(*tails.values[2], "name") == "name", "safe member name");
}

void test_inline_block_chain_boundary() {
  std::unique_ptr<Expr> expr =
      parse_ok("numbers.map: _1.email.downcase() .uniq()\n");
  expect(expr->kind == "AstPostfixChain", "inline block postfix chain");
  const amber::ast::ListField &tails = list_field(*expr, "tails");
  expect(tails.values.size() == 4, "inline chain tail count");
  expect(tails.values[0]->kind == "AstTailDotMember", "map member tail");
  expect(string_field(*tails.values[0], "name") == "map", "map method name");
  expect(tails.values[1]->kind == "AstTailBlockSuffix", "map block tail");
  expect(node_field(*tails.values[1], "block").kind == "AstBlock", "map block");
  expect(tails.values[2]->kind == "AstTailDotMember", "uniq member tail");
  expect(string_field(*tails.values[2], "name") == "uniq", "uniq method name");
  expect(bool_field(*tails.values[2], "chain_boundary"),
         "uniq continues outer chain");
  expect(tails.values[3]->kind == "AstTailCall", "uniq call tail");
}

void test_unicode_names() {
  std::unique_ptr<Expr> expr = parse_ok("коэффициент = α + β2\n");
  expect(expr->kind == "AstAssign", "unicode assignment parses");
  expect(string_field(node_field(*expr, "left"), "name") == "коэффициент",
         "unicode local name");
  expect(node_field(*expr, "right").kind == "AstBinary",
         "unicode operands parse in binary expression");

  std::unique_ptr<Expr> call = parse_ok("частица.скорость!()\n");
  expect(call->kind == "AstPostfixChain", "unicode method call parses");
  const amber::ast::ListField &tails = list_field(*call, "tails");
  expect(tails.values.size() == 2, "unicode method tail count");
  expect(string_field(*tails.values[0], "name") == "скорость!",
         "unicode method name");
}

void test_clause_def_forms() {
  const std::string source = "def area(shape):\n"
                             "  when Point(x, y):\n"
                             "    x * y\n"
                             "  when Rect(w:, h:):\n"
                             "    w * h\n"
                             "  else:\n"
                             "    0\n"
                             "\n"
                             "def fact(0): 1\n"
                             "def fact(n) if n > 0: n * fact(n - 1)\n";

  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }
  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult result = parser.parse_module_unit();
  if (!result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(result.diagnostics);
    std::exit(1);
  }

  expect(result.items.size() == 2, "clause module item count");
  expect(result.items[0]->kind == "AstClauseDef", "canonical clause def kind");
  expect(result.items[1]->kind == "AstClauseDef",
         "many-def sugar lowers to clause def");

  const Expr &area = *result.items[0];
  const Expr &area_signature = node_field(area, "base_signature");
  expect(list_field(area_signature, "params").values.size() == 1,
         "area base signature arity");
  const amber::ast::ListField &area_clauses = list_field(area, "clauses");
  expect(area_clauses.values.size() == 2, "area clause count");
  expect(string_field(*area_clauses.values[0], "pattern") == "Point(x, y)",
         "head pattern preserved");
  expect(string_field(*area_clauses.values[1], "pattern") == "Rect(w:, h:)",
         "keyword head pattern preserved");
  expect(list_field(area, "else_body").values.size() == 1, "area else body");

  const Expr &fact = *result.items[1];
  const Expr &fact_signature = node_field(fact, "base_signature");
  expect(list_field(fact_signature, "params").values.size() == 1,
         "fact synthetic signature arity");
  expect(string_field(*list_field(fact_signature, "params").values[0],
                      "local_name") == "__arg0",
         "fact synthetic local name");
  const amber::ast::ListField &fact_clauses = list_field(fact, "clauses");
  expect(fact_clauses.values.size() == 2, "fact clauses merged");
  expect(string_field(*fact_clauses.values[0], "pattern") == "0",
         "literal pattern preserved");
  expect(string_field(*fact_clauses.values[1], "pattern") == "n",
         "identifier pattern preserved");
  expect(node_field(*fact_clauses.values[1], "guard_expr").kind == "AstBinary",
         "guard expression preserved");
}

void test_pattern_assignment_and_block_param_patterns() {
  const std::string source = "def transform(xs):\n"
                             "  xs.map |[head, *tail], {scale: σ}|: head\n"
                             "  [α, *ω] = xs\n";

  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }
  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult result = parser.parse_module_unit();
  if (!result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(result.diagnostics);
    std::exit(1);
  }

  expect(result.items.size() == 1, "pattern module item count");
  const Expr &def = *result.items[0];
  expect(def.kind == "AstDefStmt", "pattern forms stay inside def body");
  const amber::ast::ListField &body = list_field(def, "body");
  expect(body.values.size() == 2, "pattern body item count");

  const Expr &map_stmt = *body.values[0];
  const Expr &map_expr = node_field(map_stmt, "expr");
  expect(map_expr.kind == "AstPostfixChain",
         "block pattern uses postfix chain");
  const amber::ast::ListField &tails = list_field(map_expr, "tails");
  expect(tails.values.size() == 2, "block pattern chain tail count");
  expect(tails.values[1]->kind == "AstTailBlockSuffix", "block suffix tail");
  const Expr &block = node_field(*tails.values[1], "block");
  const amber::ast::ListField &params = list_field(block, "params");
  expect(params.values.size() == 2, "block pattern param count");
  expect(string_field(*params.values[0], "pattern") == "[head, *tail]",
         "first block param pattern text");
  expect(string_field(*params.values[1], "pattern") == "{scale:σ}",
         "second block param pattern text");

  const Expr &assign_expr = *body.values[1];
  expect(assign_expr.kind == "AstPatternAssign", "pattern assignment node");
  expect(string_field(assign_expr, "pattern") == "[α, *ω]",
         "pattern assignment text preserved");
  expect(node_field(assign_expr, "right").kind == "AstName",
         "pattern assignment rhs preserved");
}

void test_module_forms() {
  const std::string source = "package physics.core\n"
                             "import math.constants as consts\n"
                             "from lab.units import Meter as Метр, Second\n"
                             "export Particle, Метр\n"
                             "\n"
                             "mixin Timestamped:\n"
                             "  def touch!():\n"
                             "    noop\n"
                             "\n"
                             "class Particle < Entity:\n"
                             "  include Timestamped\n"
                             "  extend Serializable\n"
                             "  class_method def find(id):\n"
                             "    id\n"
                             "  def init(@масса, α = 1):\n"
                             "    pass\n";

  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }
  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult result = parser.parse_module_unit();
  if (!result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(result.diagnostics);
    std::exit(1);
  }

  expect(result.module_name == "physics.core", "package module name");
  expect(result.items.size() == 6, "module item count");
  expect(result.items[0]->kind == "AstPackageDecl", "package item");
  expect(result.items[1]->kind == "AstImportStmt", "import item");
  expect(result.items[2]->kind == "AstImportStmt", "from import item");
  expect(result.items[3]->kind == "AstExportStmt", "export item");
  expect(result.items[4]->kind == "AstMixinDef", "mixin item");
  expect(result.items[5]->kind == "AstClassDef", "class item");
  expect(list_field(*result.items[5], "body").values.size() == 4,
         "class body items");
}

void test_control_flow_forms() {
  const std::string source = "def choose(x):\n"
                             "  if x > 10:\n"
                             "    break x\n"
                             "  elif x == 10:\n"
                             "    x\n"
                             "  else:\n"
                             "    unless x:\n"
                             "      0\n"
                             "  case! x:\n"
                             "    when 0:\n"
                             "      1\n"
                             "    when n if n > 0:\n"
                             "      n\n"
                             "    else:\n"
                             "      -1\n"
                             "  while x < 3:\n"
                             "    x = x + 1\n"
                             "  until done?:\n"
                             "    tick()\n"
                             "  loop:\n"
                             "    break x\n"
                             "  do:\n"
                             "    x = x + 1\n"
                             "  while x < 5\n";

  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }
  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult result = parser.parse_module_unit();
  if (!result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(result.diagnostics);
    std::exit(1);
  }

  expect(result.items.size() == 1, "control module item count");
  const Expr &def = *result.items[0];
  expect(def.kind == "AstDefStmt", "control def");
  const amber::ast::ListField &body = list_field(def, "body");
  expect(body.values.size() == 6, "control body item count");
  expect(node_field(*body.values[0], "expr").kind == "AstIf", "if stmt");
  expect(node_field(*body.values[1], "expr").kind == "AstCase", "case stmt");
  expect(node_field(*body.values[2], "expr").kind == "AstWhile", "while stmt");
  expect(node_field(*body.values[3], "expr").kind == "AstUntil", "until stmt");
  expect(node_field(*body.values[4], "expr").kind == "AstLoop", "loop stmt");
  expect(node_field(*body.values[5], "expr").kind == "AstDoWhile",
         "do while stmt");
}

void test_typed_signature_surface() {
  const std::string source =
      "def load(path as Str?, headers as Map[Str, Str]:) -> Result[Str, Err]:\n"
      "  path\n";

  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }
  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult result = parser.parse_module_unit();
  if (!result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(result.diagnostics);
    std::exit(1);
  }

  expect(result.items.size() == 1, "typed signature module item count");
  const Expr &def = *result.items[0];
  const Expr &signature = node_field(def, "signature");
  expect(string_field(signature, "return_type_expr") == "Result[Str, Err]",
         "return TypeTerm preserved");
  const amber::ast::ListField &params = list_field(signature, "params");
  expect(params.values.size() == 2, "typed param count");
  expect(string_field(*params.values[0], "type_expr") == "Str?",
         "optional param TypeTerm preserved");
  expect(string_field(*params.values[1], "type_expr") == "Map[Str, Str]",
         "generic keyword param TypeTerm preserved");
}

} // namespace

int main() {
  test_precedence();
  test_bare_call();
  test_safe_nav_and_index();
  test_inline_block_chain_boundary();
  test_unicode_names();
  test_clause_def_forms();
  test_pattern_assignment_and_block_param_patterns();
  test_module_forms();
  test_control_flow_forms();
  test_typed_signature_surface();
  std::cout << "parser_tests: ok\n";
  return 0;
}
