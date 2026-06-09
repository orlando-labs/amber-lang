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

amber::parser::ParseResult parse_raw(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  return parser.parse_expression_unit();
}

amber::parser::ParseModuleResult parse_module_raw(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  return parser.parse_module_unit();
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

bool has_diagnostic(const amber::parser::ParseResult &result,
                    const std::string &code) {
  for (const amber::lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool has_diagnostic(const amber::parser::ParseModuleResult &result,
                    const std::string &code) {
  for (const amber::lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
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

void test_compound_assignment() {
  std::unique_ptr<Expr> expr = parse_ok("x += 1\n");
  expect(expr->kind == "AstAssign", "compound assignment parses");
  expect(string_field(*expr, "op") == "+=", "compound assignment op");
  expect(node_field(*expr, "left").kind == "AstName",
         "compound assignment left");
  expect(node_field(*expr, "right").kind == "AstLiteral",
         "compound assignment right");

  amber::parser::ParseModuleResult module = parse_module_raw("x += 1\n");
  expect(module.ok(), "compound assignment module parse should not recurse");
  expect(module.items.size() == 1, "compound assignment module item count");
  const Expr &stmt = *module.items[0];
  expect(node_field(stmt, "expr").kind == "AstAssign",
         "compound assignment module expr");
}

void test_new_binary_operators() {
  std::unique_ptr<Expr> floor_div = parse_ok("x // 2\n");
  expect(floor_div->kind == "AstBinary", "floor division parses as binary");
  expect(string_field(*floor_div, "op") == "//", "floor division op");

  std::unique_ptr<Expr> modulo = parse_ok("x % 2\n");
  expect(modulo->kind == "AstBinary", "modulo parses as binary");
  expect(string_field(*modulo, "op") == "%", "modulo op");

  std::unique_ptr<Expr> spaceship = parse_ok("x <=> y\n");
  expect(spaceship->kind == "AstBinary", "spaceship parses as binary");
  expect(string_field(*spaceship, "op") == "<=>", "spaceship op");

  std::unique_ptr<Expr> power = parse_ok("2 ** 3 ** 2\n");
  expect(power->kind == "AstBinary", "power parses as binary");
  expect(string_field(*power, "op") == "**", "power op");
  expect(node_field(*power, "right").kind == "AstBinary",
         "power is right-associative");
  expect(string_field(node_field(*power, "right"), "op") == "**",
         "nested power stays on right");

  std::unique_ptr<Expr> unary_power = parse_ok("-2 ** 2\n");
  expect(unary_power->kind == "AstUnary", "unary minus stays outer");
  expect(node_field(*unary_power, "operand").kind == "AstBinary",
         "power binds tighter than unary");
  expect(string_field(node_field(*unary_power, "operand"), "op") == "**",
         "unary operand is power");

  std::unique_ptr<Expr> unary_multiply = parse_ok("-2 * 3\n");
  expect(unary_multiply->kind == "AstBinary",
         "multiplication stays outside unary minus");
  expect(string_field(*unary_multiply, "op") == "*",
         "unary binds tighter than multiplication");
  expect(node_field(*unary_multiply, "left").kind == "AstUnary",
         "multiplication lhs is unary");

  std::unique_ptr<Expr> xor_expr = parse_ok("x ^ y\n");
  expect(xor_expr->kind == "AstBinary", "xor parses as binary");
  expect(string_field(*xor_expr, "op") == "^", "xor op");

  std::unique_ptr<Expr> and_expr = parse_ok("x & y\n");
  expect(and_expr->kind == "AstBinary", "bit and parses as binary");
  expect(string_field(*and_expr, "op") == "&", "bit and op");

  std::unique_ptr<Expr> or_expr = parse_ok("x | y\n");
  expect(or_expr->kind == "AstBinary", "bit or parses as binary");
  expect(string_field(*or_expr, "op") == "|", "bit or op");

  std::unique_ptr<Expr> bit_precedence = parse_ok("x | y ^ z & w\n");
  expect(bit_precedence->kind == "AstBinary", "bit precedence root");
  expect(string_field(*bit_precedence, "op") == "|",
         "bit or has lowest bitwise precedence");
  expect(node_field(*bit_precedence, "right").kind == "AstBinary",
         "bit or rhs is xor");
  expect(string_field(node_field(*bit_precedence, "right"), "op") == "^",
         "xor binds above or");
  expect(node_field(node_field(*bit_precedence, "right"), "right").kind ==
             "AstBinary",
         "xor rhs is bit and");
  expect(string_field(node_field(node_field(*bit_precedence, "right"), "right"),
                      "op") == "&",
         "bit and binds above xor");

  std::unique_ptr<Expr> shl = parse_ok("x << 5\n");
  expect(shl->kind == "AstBinary", "left shift parses as binary");
  expect(string_field(*shl, "op") == "<<", "left shift op");

  std::unique_ptr<Expr> shr = parse_ok("x >> 5\n");
  expect(shr->kind == "AstBinary", "right shift parses as binary");
  expect(string_field(*shr, "op") == ">>", "right shift op");
}

void test_comparison_chains() {
  std::unique_ptr<Expr> ascending = parse_ok("a < x <= b\n");
  expect(ascending->kind == "AstCompareChain",
         "ascending comparison chain parses as chain");
  expect(node_field(*ascending, "first").kind == "AstName",
         "chain first operand");
  const amber::ast::ListField &ascending_links =
      list_field(*ascending, "links");
  expect(ascending_links.values.size() == 2, "ascending chain link count");
  expect(string_field(*ascending_links.values[0], "op") == "<",
         "ascending first op");
  expect(string_field(*ascending_links.values[1], "op") == "<=",
         "ascending second op");

  std::unique_ptr<Expr> descending = parse_ok("a > x >= b\n");
  expect(descending->kind == "AstCompareChain",
         "descending comparison chain parses as chain");
  const amber::ast::ListField &descending_links =
      list_field(*descending, "links");
  expect(descending_links.values.size() == 2, "descending chain link count");
  expect(string_field(*descending_links.values[0], "op") == ">",
         "descending first op");
  expect(string_field(*descending_links.values[1], "op") == ">=",
         "descending second op");

  std::unique_ptr<Expr> mixed = parse_ok("a < x > b\n");
  expect(mixed->kind == "AstBinary",
         "mixed comparison directions stay nested binaries");
  expect(string_field(*mixed, "op") == ">", "mixed outer op");
  expect(node_field(*mixed, "left").kind == "AstBinary", "mixed left binary");
  expect(string_field(node_field(*mixed, "left"), "op") == "<",
         "mixed inner op");
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

void test_optional_bracket_access() {
  std::unique_ptr<Expr> expr = parse_ok("xs[?i]\n");
  expect(expr->kind == "AstPostfixChain", "optional index postfix chain");
  const amber::ast::ListField &tails = list_field(*expr, "tails");
  expect(tails.values.size() == 1, "optional index tail count");
  expect(tails.values[0]->kind == "AstTailIndex", "optional index tail kind");
  expect(bool_field(*tails.values[0], "optional"), "optional index marker");
  expect(node_field(*tails.values[0], "index_expr").kind == "AstName",
         "optional index expression");

  std::unique_ptr<Expr> negative = parse_ok("xs[?-1]\n");
  const amber::ast::ListField &negative_tails = list_field(*negative, "tails");
  expect(bool_field(*negative_tails.values[0], "optional"),
         "optional negative index marker");

  amber::parser::ParseResult assignment = parse_raw("xs[?i] = 1\n");
  expect(has_diagnostic(assignment, "E_OPTIONAL_BRACKET_ASSIGNMENT"),
         "optional index assignment diagnostic");

  amber::parser::ParseModuleResult module_assignment =
      parse_module_raw("xs[?i] = 1\n");
  expect(has_diagnostic(module_assignment, "E_OPTIONAL_BRACKET_ASSIGNMENT"),
         "optional index assignment module diagnostic");
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

void test_indented_block_suffix_body() {
  std::unique_ptr<Expr> expr =
      parse_ok("numbers.map |x|:\n"
               "  doubled = x * 2\n"
               "  doubled + 1\n");
  expect(expr->kind == "AstPostfixChain", "indented block postfix chain");
  const amber::ast::ListField &tails = list_field(*expr, "tails");
  expect(tails.values.size() == 2, "indented block chain tail count");
  expect(tails.values[1]->kind == "AstTailBlockSuffix",
         "indented block suffix tail");

  const Expr &block = node_field(*tails.values[1], "block");
  expect(block.kind == "AstBlock", "indented suffix block");
  const amber::ast::ListField &params = list_field(block, "params");
  expect(params.values.size() == 1, "indented block param count");
  expect(string_field(*params.values[0], "pattern") == "x",
         "indented block param pattern");

  const amber::ast::ListField &body = list_field(block, "body");
  expect(body.values.size() == 2, "indented block body statement count");
  expect(body.values[0]->kind == "AstExprStmt",
         "indented block first statement");
  expect(node_field(*body.values[0], "expr").kind == "AstAssign",
         "indented block assignment preserved");
  expect(body.values[1]->kind == "AstExprStmt",
         "indented block second statement");
  expect(node_field(*body.values[1], "expr").kind == "AstBinary",
         "indented block result expression preserved");
}

void test_indented_postfix_continuation() {
  amber::parser::ParseModuleResult result =
      parse_module_raw("[1, 2]\n"
                       "  .each _1 * 2\n");
  if (!result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(result.diagnostics);
    std::exit(1);
  }

  expect(result.items.size() == 1, "continued postfix item count");
  const Expr &stmt = *result.items[0];
  expect(stmt.kind == "AstExprStmt", "continued postfix statement");
  const Expr &expr = node_field(stmt, "expr");
  expect(expr.kind == "AstPostfixChain", "continued postfix chain");
  const amber::ast::ListField &tails = list_field(expr, "tails");
  expect(tails.values.size() == 2, "continued postfix tail count");
  expect(tails.values[0]->kind == "AstTailDotMember", "continued member");
  expect(string_field(*tails.values[0], "name") == "each",
         "continued member name");
  expect(tails.values[1]->kind == "AstTailCall", "continued bare call");
}

void test_module_stray_indent_progresses() {
  amber::parser::ParseModuleResult result = parse_module_raw("value\n"
                                                             "  stray\n");
  expect(!result.ok(), "stray top-level indent is rejected");
  expect(!result.diagnostics.empty(), "stray indent emits diagnostic");
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

void test_range_precedence() {
  std::unique_ptr<Expr> expr = parse_ok("a == b..c + 1\n");
  expect(expr->kind == "AstBinary", "range comparison top binary");
  expect(string_field(*expr, "op") == "==", "comparison remains outer op");
  const Expr &right = node_field(*expr, "right");
  expect(right.kind == "AstBinary", "comparison rhs is range");
  expect(string_field(right, "op") == "..", "range op");
  expect(node_field(right, "right").kind == "AstBinary",
         "range rhs keeps additive expression");
  expect(string_field(node_field(right, "right"), "op") == "+",
         "range rhs addition op");
}

void test_v20_4_range_surface() {
  std::unique_ptr<Expr> exclusive = parse_ok("1...5:2\n");
  expect(exclusive->kind == "AstBinary", "exclusive range parses");
  expect(string_field(*exclusive, "op") == "...", "exclusive range op");
  expect(!bool_field(*exclusive, "inclusive_end"),
         "exclusive range is not inclusive");
  expect(node_field(*exclusive, "step").kind == "AstLiteral",
         "exclusive range step parses");

  std::unique_ptr<Expr> open = parse_ok("1..:2\n");
  expect(open->kind == "AstBinary", "open-ended stepped range parses");
  expect(bool_field(*open, "open_ended"), "open-ended range flag");
  expect(string_field(node_field(*open, "right"), "token") == "KEYWORD_NULL",
         "open-ended range carries null finish");

  amber::parser::ParseResult float_range = parse_raw("1.0..5.0\n");
  expect(has_diagnostic(float_range, "E_RANGE_FLOAT_STEP_REQUIRED"),
         "float range without step diagnostic");

  amber::parser::ParseResult exclusive_open = parse_raw("1...:2\n");
  expect(has_diagnostic(exclusive_open, "E_RANGE_EXCLUSIVE_OPEN_ENDED"),
         "exclusive open-ended range diagnostic");
}

void test_collection_literals() {
  std::unique_ptr<Expr> list = parse_ok("[1, :ok]\n");
  expect(list->kind == "AstListLiteral", "list literal parses");
  expect(list_field(*list, "elements").values.size() == 2,
         "list literal element count");
  expect(list_field(*list, "elements").values[1]->kind == "AstLiteral",
         "symbol element parses as literal");
  expect(string_field(*list_field(*list, "elements").values[1], "token") ==
             "SYMBOL",
         "symbol literal token");

  std::unique_ptr<Expr> tuple = parse_ok("(1, 2,)\n");
  expect(tuple->kind == "AstTupleLiteral", "tuple literal parses");
  expect(list_field(*tuple, "elements").values.size() == 2,
         "tuple literal element count");

  std::unique_ptr<Expr> group = parse_ok("(1 + 2)\n");
  expect(group->kind == "AstGroup", "single parenthesized expression groups");

  std::unique_ptr<Expr> set = parse_ok("{1, :ok,}\n");
  expect(set->kind == "AstSetLiteral", "set literal parses");
  expect(list_field(*set, "elements").values.size() == 2,
         "set literal element count");
  std::unique_ptr<Expr> single_set = parse_ok("{1}\n");
  expect(single_set->kind == "AstSetLiteral",
         "single element set does not require comma");
  expect(list_field(*single_set, "elements").values.size() == 1,
         "single element set count");

  std::unique_ptr<Expr> map = parse_ok("{id: 1, \"name\": :ok, :kind: 3}\n");
  expect(map->kind == "AstMapLiteral", "map literal parses");
  const amber::ast::ListField &entries = list_field(*map, "entries");
  expect(entries.values.size() == 3, "map literal entry count");
  expect(string_field(*entries.values[0], "key_kind") == "symbol",
         "identifier key is symbol key");
  expect(string_field(*entries.values[0], "key") == "id",
         "identifier key text");
  expect(string_field(*entries.values[1], "key_kind") == "string",
         "string key is preserved");
  expect(node_field(*entries.values[1], "key_expr").kind == "AstStringLiteral",
         "string key is expression key");
  expect(string_field(*entries.values[2], "key") == "kind",
         "explicit symbol key text");

  std::unique_ptr<Expr> expr_key_map =
      parse_ok("{1: :int, (name): value, [1, 2]: :pair}\n");
  expect(expr_key_map->kind == "AstMapLiteral",
         "expression-key map literal parses");
  const amber::ast::ListField &expr_entries =
      list_field(*expr_key_map, "entries");
  expect(expr_entries.values.size() == 3,
         "expression-key map entry count");
  expect(string_field(*expr_entries.values[0], "key_kind") == "expression",
         "integer key is expression key");
  expect(node_field(*expr_entries.values[1], "key_expr").kind == "AstGroup",
         "parenthesized map key preserves expression");
  expect(node_field(*expr_entries.values[2], "key_expr").kind ==
             "AstListLiteral",
         "list map key preserves expression");

  std::unique_ptr<Expr> typed_map = parse_ok("Map{\"x\": 1}\n");
  expect(typed_map->kind == "AstMapLiteral", "Map{} parses as map literal");
  expect(string_field(*typed_map, "collection_type") == "Map",
         "Map{} collection type is preserved");
  std::unique_ptr<Expr> typed_set = parse_ok("Set{1, 2}\n");
  expect(typed_set->kind == "AstSetLiteral", "Set{} parses as set literal");
  expect(string_field(*typed_set, "collection_type") == "Set",
         "Set{} collection type is preserved");
}

void test_v20_7_spread_surface() {
  std::unique_ptr<Expr> call =
      parse_ok("fn(1, *args, mode: :fast, **opts)\n");
  expect(call->kind == "AstPostfixChain", "spread call parses");
  const amber::ast::ListField &call_tails = list_field(*call, "tails");
  const amber::ast::ListField &args = list_field(*call_tails.values[0], "args");
  expect(args.values.size() == 4, "spread call arg count");
  expect(args.values[1]->kind == "AstSpreadArg", "positional spread arg");
  expect(args.values[2]->kind == "AstKeywordArg", "ordinary keyword arg");
  expect(args.values[3]->kind == "AstKeywordSpreadArg",
         "keyword spread arg");

  std::unique_ptr<Expr> list =
      parse_ok("[1, *items if include_items?, 9]\n");
  const amber::ast::ListField &list_elements = list_field(*list, "elements");
  expect(list_elements.values[1]->kind == "AstArraySpread",
         "array spread element");
  expect(node_field(*list_elements.values[1], "condition").kind ==
             "AstCollectionCondition",
         "array spread condition");

  std::unique_ptr<Expr> set = parse_ok("{1, *items}\n");
  const amber::ast::ListField &set_elements = list_field(*set, "elements");
  expect(set_elements.values[1]->kind == "AstSetSpread",
         "set spread element");

  std::unique_ptr<Expr> map = parse_ok("{a: 1, **other, b: 2}\n");
  const amber::ast::ListField &entries = list_field(*map, "entries");
  expect(entries.values.size() == 3, "map spread entry count");
  expect(entries.values[1]->kind == "AstMapSpread", "map spread entry");

  amber::parser::ParseResult stray_spread = parse_raw("*items\n");
  expect(has_diagnostic(stray_spread, "E_SPREAD_POSITION"),
         "stray positional spread diagnostic");
  amber::parser::ParseResult kw_after_spread =
      parse_raw("fn(**opts, mode: :fast)\n");
  expect(has_diagnostic(kw_after_spread, "E_ARGUMENT_ORDER"),
         "keyword after keyword spread order diagnostic");
}

void test_inline_conditional_expression() {
  std::unique_ptr<Expr> expr = parse_ok("if ready? then :ready else :idle\n");
  expect(expr->kind == "AstInlineIfExpr", "inline if expression parses");
  expect(string_field(*expr, "form") == "inline", "inline if form preserved");
  expect(node_field(*expr, "condition").kind == "AstName",
         "inline if condition");
  expect(string_field(node_field(*expr, "condition"), "name") == "ready?",
         "question-mark method name stays intact");
  expect(node_field(*expr, "consequent").kind == "AstLiteral",
         "inline if consequent");
  expect(node_field(*expr, "alternative").kind == "AstLiteral",
         "inline if alternative");

  std::unique_ptr<Expr> nested =
      parse_ok("if if a then b else c then d else e\n");
  expect(nested->kind == "AstInlineIfExpr", "nested inline if parses");
  expect(node_field(*nested, "condition").kind == "AstInlineIfExpr",
         "nested inline if condition preserved");
}

void test_conditional_collection_elements() {
  std::unique_ptr<Expr> list =
      parse_ok("[1, 2 if include_two?, 3 unless skip_three?]\n");
  expect(list->kind == "AstListLiteral", "conditional list parses");
  const amber::ast::ListField &list_elements = list_field(*list, "elements");
  expect(list_elements.values.size() == 3, "conditional list element count");
  expect(list_elements.values[1]->kind == "AstArrayElement",
         "conditional list element wraps value");
  expect(string_field(node_field(*list_elements.values[1], "condition"),
                      "kind") == "if",
         "list condition kind");
  expect(node_field(*list_elements.values[1], "expr").kind == "AstLiteral",
         "list conditional element value");
  expect(list_elements.values[2]->kind == "AstArrayElement",
         "unless list element wraps value");
  expect(string_field(node_field(*list_elements.values[2], "condition"),
                      "kind") == "unless",
         "list unless condition kind");

  std::unique_ptr<Expr> set = parse_ok("{:read, :write if can_write?}\n");
  expect(set->kind == "AstSetLiteral", "conditional set parses");
  const amber::ast::ListField &set_elements = list_field(*set, "elements");
  expect(set_elements.values[1]->kind == "AstSetElement",
         "conditional set element wraps value");

  std::unique_ptr<Expr> map =
      parse_ok("{a: 1, b: 2 if x == 3, c: 3 unless disabled?}\n");
  expect(map->kind == "AstMapLiteral", "conditional map parses");
  const amber::ast::ListField &entries = list_field(*map, "entries");
  expect(entries.values.size() == 3, "conditional map entry count");
  expect(node_field(*entries.values[1], "condition").kind ==
             "AstCollectionCondition",
         "map condition node preserved");
  expect(string_field(node_field(*entries.values[2], "condition"), "kind") ==
             "unless",
         "map unless condition kind");
}

void test_string_literal_surface() {
  std::unique_ptr<Expr> plain = parse_ok("\"plain\"\n");
  expect(plain->kind == "AstStringLiteral", "plain double string AST kind");
  expect(!bool_field(*plain, "interpolation"), "plain string flag");
  const amber::ast::ListField &plain_parts = list_field(*plain, "parts");
  expect(plain_parts.values.size() == 1, "plain string part count");
  expect(plain_parts.values[0]->kind == "AstStringText",
         "plain string text part");
  expect(string_field(*plain_parts.values[0], "value") == "plain",
         "plain string text preserved");

  std::unique_ptr<Expr> escaped = parse_ok("\"\\#{literal}\"\n");
  expect(escaped->kind == "AstStringLiteral", "escaped interpolation AST kind");
  expect(!bool_field(*escaped, "interpolation"),
         "escaped interpolation marker stays literal");
  const amber::ast::ListField &escaped_parts = list_field(*escaped, "parts");
  expect(escaped_parts.values.size() == 2, "escaped marker part count");
  expect(escaped_parts.values[0]->kind == "AstStringEscape",
         "escaped hash part kind");
  expect(string_field(*escaped_parts.values[0], "value") == "#",
         "escaped hash value");

  std::unique_ptr<Expr> interp =
      parse_ok("\"hello #{name} #{if ok then \"yes\" else \"no\"}\"\n");
  expect(interp->kind == "AstStringLiteral", "interpolated string AST kind");
  expect(bool_field(*interp, "interpolation"), "interpolation flag");
  const amber::ast::ListField &parts = list_field(*interp, "parts");
  expect(parts.values.size() == 4, "interpolated string part count");
  expect(parts.values[1]->kind == "AstStringExpr", "first expr part");
  expect(node_field(*parts.values[1], "expr").kind == "AstName",
         "interpolation expression span parses as name");
  expect(parts.values[2]->kind == "AstStringText" &&
             string_field(*parts.values[2], "value") == " ",
         "text between interpolation expressions is preserved");
  expect(parts.values[3]->kind == "AstStringExpr", "second expr part");
  expect(node_field(*parts.values[3], "expr").kind == "AstInlineIfExpr",
         "inline conditional expression inside interpolation parses");

  amber::parser::ParseResult empty = parse_raw("\"#{}\"\n");
  expect(has_diagnostic(empty, "AMB_STRING_INTERP_EMPTY"),
         "empty interpolation diagnostic");
}

void test_inline_conditional_diagnostics() {
  amber::parser::ParseResult ternary = parse_raw("cond ? a : b\n");
  expect(has_diagnostic(ternary, "AMB-SYN-INLINE-TERNARY-CSTYLE"),
         "C-style ternary diagnostic");

  amber::parser::ParseResult missing_else = parse_raw("if cond then a\n");
  expect(has_diagnostic(missing_else, "AMB-SYN-INLINE-IF-MISSING-ELSE"),
         "inline if missing else diagnostic");

  amber::parser::ParseResult missing_value = parse_raw("[if cond,]\n");
  expect(has_diagnostic(missing_value,
                        "AMB-SYN-CONDITIONAL-ELEMENT-MISSING-VALUE"),
         "conditional element missing value diagnostic");
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

void test_effect_row_signature() {
  const std::string source = "def fetch(id as UserId) -> User !{net, async}:\n"
                             "  id\n"
                             "def pure(id) !{}:\n"
                             "  id\n";

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

  expect(result.items.size() == 2, "effect row item count");
  const Expr &fetch_signature = node_field(*result.items[0], "signature");
  expect(string_field(fetch_signature, "return_type_expr") == "User",
         "return type stops before effect row");
  expect(bool_field(fetch_signature, "has_effect_row"), "effect row flag");
  expect(string_field(fetch_signature, "effect_row_expr") == "!{net, async}",
         "effect row text preserved");
  const Expr &pure_signature = node_field(*result.items[1], "signature");
  expect(bool_field(pure_signature, "has_effect_row"), "pure effect row flag");
  expect(string_field(pure_signature, "effect_row_expr") == "!{}",
         "pure effect row preserved");
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

void test_property_forms() {
  amber::parser::ParseModuleResult result = parse_module_raw(
      "prop answer: 42\n"
      "\n"
      "class User:\n"
      "  prop full_name:\n"
      "    get:\n"
      "      @first\n"
      "    set(value):\n"
      "      @first = value\n"
      "\n"
      "class Build:\n"
      "  class_prop version: \"20.3\"\n");
  if (!result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(result.diagnostics);
    std::exit(1);
  }

  expect(result.items.size() == 3, "property module item count");
  expect(result.items[0]->kind == "AstPropDef", "top-level prop kind");
  expect(string_field(*result.items[0], "name") == "answer",
         "top-level prop name");
  const amber::ast::ListField &user_body = list_field(*result.items[1], "body");
  expect(user_body.values.size() == 1 &&
             user_body.values[0]->kind == "AstPropDef",
         "class instance prop kind");
  expect(bool_field(*user_body.values[0], "grouped_descriptor"),
         "grouped property descriptor marker");
  expect(bool_field(*user_body.values[0], "has_getter"),
         "grouped property getter marker");
  expect(bool_field(*user_body.values[0], "has_setter"),
         "grouped property setter marker");
  expect(list_field(*user_body.values[0], "getter_body").values.size() == 1,
         "grouped property getter body");
  expect(list_field(*user_body.values[0], "setter_body").values.size() == 1,
         "grouped property setter body");
  expect(node_field(*user_body.values[0], "setter_signature").kind ==
             "AstSignature",
         "grouped property setter signature");
  const amber::ast::ListField &build_body = list_field(*result.items[2], "body");
  expect(build_body.values.size() == 1 &&
             build_body.values[0]->kind == "AstClassPropDef",
         "class property kind");
}

void test_attribute_forms() {
  amber::parser::ParseModuleResult result =
      parse_module_raw("class User:\n"
                       "  attr email\n"
                       "  attr var name from @raw_name\n"
                       "  attr set password\n");
  if (!result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(result.diagnostics);
    std::exit(1);
  }

  const amber::ast::ListField &body = list_field(*result.items[0], "body");
  expect(body.values.size() == 3, "attribute class body item count");

  const Expr &email = *body.values[0];
  expect(email.kind == "AstAttrDef", "getter-only attr kind");
  expect(string_field(email, "name") == "email", "attr name");
  expect(string_field(email, "attr_mode") == "get_only",
         "getter-only attr mode");
  expect(string_field(email, "storage_field") == "email",
         "default attr storage");
  expect(bool_field(email, "has_getter") && !bool_field(email, "has_setter"),
         "getter-only attr flags");
  const Expr &email_get =
      node_field(*list_field(email, "getter_body").values[0], "expr");
  expect(email_get.kind == "AstIvar" &&
             string_field(email_get, "name") == "email",
         "getter-only attr synthetic getter body");

  const Expr &name = *body.values[1];
  expect(name.kind == "AstAttrDef", "read-write attr kind");
  expect(string_field(name, "attr_mode") == "get_set", "read-write attr mode");
  expect(string_field(name, "storage_field") == "raw_name",
         "explicit attr storage");
  expect(bool_field(name, "explicit_storage"), "explicit storage marker");
  expect(bool_field(name, "has_getter") && bool_field(name, "has_setter"),
         "read-write attr flags");
  expect(node_field(name, "setter_signature").kind == "AstSignature",
         "attr setter signature");
  expect(list_field(name, "setter_body").values.size() == 1,
         "attr setter body");

  const Expr &password = *body.values[2];
  expect(string_field(password, "attr_mode") == "set_only",
         "setter-only attr mode");
  expect(!bool_field(password, "has_getter") &&
             bool_field(password, "has_setter"),
         "setter-only attr flags");
}

void test_attribute_diagnostics() {
  amber::parser::ParseModuleResult missing_name =
      parse_module_raw("class User:\n"
                       "  attr from @email\n");
  expect(!missing_name.ok(), "attr missing name rejected");
  expect(!missing_name.diagnostics.empty() &&
             missing_name.diagnostics[0].code == "E_ATTR_EXPECTED_NAME",
         "attr missing name diagnostic");

  amber::parser::ParseModuleResult missing_storage =
      parse_module_raw("class User:\n"
                       "  attr email from\n");
  expect(!missing_storage.ok(), "attr missing storage rejected");
  expect(!missing_storage.diagnostics.empty() &&
             missing_storage.diagnostics[0].code ==
                 "E_ATTR_EXPECTED_STORAGE_FIELD",
         "attr missing storage diagnostic");

  amber::parser::ParseModuleResult invalid_storage =
      parse_module_raw("class User:\n"
                       "  attr email from self.email\n");
  expect(!invalid_storage.ok(), "attr invalid storage rejected");
  expect(!invalid_storage.diagnostics.empty() &&
             invalid_storage.diagnostics[0].code == "E_ATTR_INVALID_STORAGE",
         "attr invalid storage diagnostic");

  amber::parser::ParseModuleResult invalid_call_storage =
      parse_module_raw("class User:\n"
                       "  attr email from foo()\n");
  expect(!invalid_call_storage.ok(), "attr call storage rejected");
  expect(!invalid_call_storage.diagnostics.empty() &&
             invalid_call_storage.diagnostics[0].code ==
                 "E_ATTR_INVALID_STORAGE",
         "attr call storage diagnostic");

  amber::parser::ParseModuleResult invalid_class_storage =
      parse_module_raw("class User:\n"
                       "  attr email from @@shared\n");
  expect(!invalid_class_storage.ok(), "attr class storage rejected");
  expect(!invalid_class_storage.diagnostics.empty() &&
             invalid_class_storage.diagnostics[0].code ==
                 "E_ATTR_INVALID_STORAGE",
         "attr class storage diagnostic");

  amber::parser::ParseModuleResult invalid_context =
      parse_module_raw("attr email\n");
  expect(!invalid_context.ok(), "top-level attr rejected");
  expect(!invalid_context.diagnostics.empty() &&
             invalid_context.diagnostics[0].code == "E_ATTR_INVALID_CONTEXT",
         "attr invalid context diagnostic");
}

void test_property_parameter_diagnostic() {
  amber::parser::ParseModuleResult result = parse_module_raw("prop f(x): x\n");
  expect(!result.ok(), "property parameter list rejected");
  expect(!result.diagnostics.empty() &&
             result.diagnostics[0].code == "AMB_PROP_PARAM_LIST_FORBIDDEN",
         "property parameter diagnostic code");
}

void test_property_grouped_diagnostics_and_context() {
  amber::parser::ParseModuleResult local_result =
      parse_module_raw("def f():\n"
                       "  prop local: 1\n");
  expect(!local_result.ok(), "local property rejected by parser");
  expect(!local_result.diagnostics.empty() &&
             local_result.diagnostics[0].code == "AMB_PROP_INVALID_CONTEXT",
         "local property diagnostic code");

  amber::parser::ParseModuleResult setter_result =
      parse_module_raw("class Box:\n"
                       "  prop value:\n"
                       "    set(a, b):\n"
                       "      pass\n");
  expect(!setter_result.ok(), "bad setter arity rejected");
  expect(!setter_result.diagnostics.empty() &&
             setter_result.diagnostics[0].code == "AMB_PROP_SETTER_ARITY",
         "bad setter arity diagnostic code");
}

void test_property_keywords_are_contextual_names() {
  amber::parser::ParseModuleResult result =
      parse_module_raw("prop = 1\n"
                       "class_prop = prop\n"
                       "attr = class_prop\n"
                       "box.prop\n"
                       "box.attr\n");
  if (!result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(result.diagnostics);
    std::exit(1);
  }

  expect(result.items.size() == 5, "contextual property keyword item count");
  const Expr &first_assign = node_field(*result.items[0], "expr");
  expect(first_assign.kind == "AstAssign", "prop assignment parses");
  expect(string_field(node_field(first_assign, "left"), "name") == "prop",
         "prop keyword remains name outside declaration form");

  const Expr &second_assign = node_field(*result.items[1], "expr");
  expect(second_assign.kind == "AstAssign", "class_prop assignment parses");
  expect(string_field(node_field(second_assign, "left"), "name") ==
             "class_prop",
         "class_prop keyword remains name outside declaration form");
  const Expr &third_assign = node_field(*result.items[2], "expr");
  expect(third_assign.kind == "AstAssign", "attr assignment parses");
  expect(string_field(node_field(third_assign, "left"), "name") == "attr",
         "attr keyword remains name outside declaration form");
  expect(result.items[3]->kind == "AstExprStmt",
         "member named prop parses as expression statement");
  expect(result.items[4]->kind == "AstExprStmt",
         "member named attr parses as expression statement");
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

void test_try_rescue_ensure_forms() {
  std::unique_ptr<Expr> expr =
      parse_ok("try:\n"
               "  1\n"
               "rescue TypeError, ArgumentError |e|:\n"
               "  2\n"
               "ensure:\n"
               "  3\n");
  expect(expr->kind == "AstTry", "try expression parses");
  expect(list_field(*expr, "body").values.size() == 1, "try body item count");
  const amber::ast::ListField &rescues = list_field(*expr, "rescues");
  expect(rescues.values.size() == 1, "try rescue count");
  const Expr &rescue = *rescues.values[0];
  expect(string_field(rescue, "binding") == "e", "rescue binding parses");
  expect(list_field(rescue, "matchers").values.size() == 2,
         "comma-separated rescue matchers parse");
  expect(bool_field(*expr, "has_ensure"), "try ensure flag parses");
  expect(list_field(*expr, "ensure_body").values.size() == 1,
         "ensure body parses");

  amber::parser::ParseModuleResult function_handlers = parse_module_raw(
      "def load():\n"
      "  1\n"
      "rescue |e|:\n"
      "  e\n"
      "ensure:\n"
      "  2\n");
  expect(function_handlers.ok(), "function-level rescue/ensure parses");
  const Expr &def = *function_handlers.items[0];
  expect(def.kind == "AstDefStmt", "handler suffix def parses");
  expect(list_field(def, "rescues").values.size() == 1,
         "def rescue suffix parses");
  expect(bool_field(def, "has_ensure"), "def ensure suffix parses");

  amber::parser::ParseModuleResult rescue_after_ensure = parse_module_raw(
      "try:\n"
      "  1\n"
      "ensure:\n"
      "  2\n"
      "rescue:\n"
      "  3\n");
  expect(has_diagnostic(rescue_after_ensure, "E_RESCUE_AFTER_ENSURE"),
         "rescue after ensure is diagnosed");

  amber::parser::ParseModuleResult pipe_union = parse_module_raw(
      "try:\n"
      "  1\n"
      "rescue TypeError | ArgumentError |e|:\n"
      "  2\n");
  expect(has_diagnostic(pipe_union, "E_RESCUE_PIPE_UNION_FORBIDDEN"),
         "pipe-union rescue matcher spelling is diagnosed");
}

void test_throw_catch_forms() {
  std::unique_ptr<Expr> thrown = parse_ok("throw :enough, 42\n");
  expect(thrown->kind == "AstThrow", "throw expression parses");
  expect(node_field(*thrown, "tag").kind == "AstLiteral",
         "throw tag parses as literal");
  expect(string_field(node_field(*thrown, "tag"), "token") == "SYMBOL",
         "throw symbol tag token");
  expect(string_field(node_field(*thrown, "tag"), "value") == "enough",
         "throw symbol tag value");
  expect(node_field(*thrown, "value").kind == "AstLiteral",
         "throw payload parses");

  std::unique_ptr<Expr> paren_catch =
      parse_ok("catch(:enough):\n"
               "  1\n");
  expect(paren_catch->kind == "AstCatch", "paren catch expression parses");
  expect(string_field(node_field(*paren_catch, "tag"), "value") == "enough",
         "paren catch tag parses");
  expect(list_field(*paren_catch, "body").values.size() == 1,
         "paren catch body parses");

  std::unique_ptr<Expr> bare_catch =
      parse_ok("catch :enough:\n"
               "  1\n");
  expect(bare_catch->kind == "AstCatch", "bare catch expression parses");
  expect(string_field(node_field(*bare_catch, "tag"), "value") == "enough",
         "bare catch tag parses");
  expect(list_field(*bare_catch, "body").values.size() == 1,
         "bare catch body parses");
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
  test_compound_assignment();
  test_new_binary_operators();
  test_comparison_chains();
  test_bare_call();
  test_safe_nav_and_index();
  test_optional_bracket_access();
  test_inline_block_chain_boundary();
  test_indented_block_suffix_body();
  test_indented_postfix_continuation();
  test_module_stray_indent_progresses();
  test_unicode_names();
  test_range_precedence();
  test_v20_4_range_surface();
  test_collection_literals();
  test_v20_7_spread_surface();
  test_inline_conditional_expression();
  test_conditional_collection_elements();
  test_string_literal_surface();
  test_inline_conditional_diagnostics();
  test_clause_def_forms();
  test_effect_row_signature();
  test_pattern_assignment_and_block_param_patterns();
  test_module_forms();
  test_property_forms();
  test_attribute_forms();
  test_attribute_diagnostics();
  test_property_parameter_diagnostic();
  test_property_grouped_diagnostics_and_context();
  test_property_keywords_are_contextual_names();
  test_control_flow_forms();
  test_try_rescue_ensure_forms();
  test_throw_catch_forms();
  test_typed_signature_surface();
  std::cout << "parser_tests: ok\n";
  return 0;
}
