#include "runtime/macro_expander.h"

#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/value.h"
#include "runtime/vm.h"
#include "runtime/vm_internal.h"
#include "runtime/world.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace amber::macros {
namespace {

constexpr int kExpansionDepthLimit = 128;

// Expander sandbox budget (§10): generous enough for real code generation,
// small enough that a looping macro fails the build in well under a second.
constexpr std::int64_t kExpansionStepBudget = 50'000'000;

// Sandbox world for macro execution (§10): non-null world options ARM the
// VM's capability gate (a null world means ambient authority), and the empty
// resolution grants nothing — any IO attempt faults with CapabilityError.
// Clock/random are not yet capability-gated and remain a documented
// determinism gap.
const runtime::RuntimeWorldOptions &sandbox_world_options() {
  static const runtime::RuntimeWorldOptions options{};
  return options;
}

const runtime::RuntimeCapabilityResolution &sandbox_no_capabilities() {
  static const runtime::RuntimeCapabilityResolution resolution{};
  return resolution;
}

const std::string *string_field(const ast::Expr &expr, const std::string &name) {
  for (const ast::StringField &field : expr.string_fields) {
    if (field.name == name) {
      return &field.value;
    }
  }
  return nullptr;
}

bool is_macro_def(const ast::Expr &expr) {
  if (expr.kind != "AstDefStmt" && expr.kind != "AstClassMethodDef") {
    return false;
  }
  for (const ast::BoolField &field : expr.bool_fields) {
    if (field.name == "is_macro") {
      return field.value;
    }
  }
  return false;
}

// A `string_tag macro def` (§8.5): a macro whose only invocation surface is
// the tag trigger `name"""…"""`.
bool is_string_tag_def(const ast::Expr &expr) {
  if (!is_macro_def(expr)) {
    return false;
  }
  for (const ast::BoolField &field : expr.bool_fields) {
    if (field.name == "is_string_tag") {
      return field.value;
    }
  }
  return false;
}

const ast::Expr *node_field(const ast::Expr &expr, const std::string &name) {
  for (const ast::NodeField &field : expr.node_fields) {
    if (field.name == name) {
      return field.value.get();
    }
  }
  return nullptr;
}

const ast::ListField *list_field(const ast::Expr &expr,
                                 const std::string &name) {
  for (const ast::ListField &field : expr.list_fields) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

ast::ListField *mutable_list_field(ast::Expr &expr, const std::string &name) {
  for (ast::ListField &field : expr.list_fields) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

// Template-vs-kernel body detection (DESIGN-macro-system §6). A `macro def`
// body is a template by default: its statements are the emitted code, wrapped
// in an implicit quote. The §5 kernel escape hatches keep their meaning: a
// body with a top-level `return` (returning a constructed Ast) or one whose
// final statement is an explicit `quote:` block is procedural and runs as
// written.
bool is_kernel_style_body(const std::vector<std::unique_ptr<ast::Expr>> &body) {
  if (body.empty()) {
    return true;
  }
  // `return` is an expression in Amber, so a return statement arrives as
  // AstExprStmt{AstReturn}; unwrap the statement wrapper before checking.
  const auto statement_expr = [](const ast::Expr &stmt) -> const ast::Expr * {
    if (stmt.kind == "AstExprStmt") {
      return node_field(stmt, "expr");
    }
    return &stmt;
  };
  for (const std::unique_ptr<ast::Expr> &stmt : body) {
    if (!stmt) {
      continue;
    }
    const ast::Expr *expr = statement_expr(*stmt);
    if (expr != nullptr && expr->kind == "AstReturn") {
      return true;
    }
  }
  const ast::Expr *last =
      body.back() ? statement_expr(*body.back()) : nullptr;
  return last != nullptr && last->kind == "AstQuote";
}

// The template accumulator local for `%`-control bodies. Reserved: a macro
// body binding this name on a `%`-line shadows the accumulator (documented).
constexpr const char *kEmitAccumulator = "__macro_emitted";

bool is_macro_control(const ast::Expr &expr) {
  for (const ast::BoolField &field : expr.bool_fields) {
    if (field.name == "macro_control") {
      return field.value;
    }
  }
  return false;
}

void strip_control_marker(ast::Expr &expr) {
  expr.bool_fields.erase(
      std::remove_if(expr.bool_fields.begin(), expr.bool_fields.end(),
                     [](const ast::BoolField &field) {
                       return field.name == "macro_control";
                     }),
      expr.bool_fields.end());
}

bool body_has_control_lines(
    const std::vector<std::unique_ptr<ast::Expr>> &body) {
  for (const std::unique_ptr<ast::Expr> &stmt : body) {
    if (stmt && is_macro_control(*stmt)) {
      return true;
    }
  }
  return false;
}

// `__macro_emitted.push!(quote: <stmt>)` — one emitted node per template
// line, so repetition inside `%`-loops accumulates sibling statements.
std::unique_ptr<ast::Expr> make_emit_push(std::unique_ptr<ast::Expr> stmt) {
  const lexer::Span span = stmt->span;
  auto quote = ast::make_expr("AstQuote", span);
  std::vector<std::unique_ptr<ast::Expr>> quoted;
  quoted.push_back(std::move(stmt));
  quote->list_field("body", std::move(quoted));

  auto base = ast::make_expr("AstName", span);
  base->string_field("name", kEmitAccumulator);
  auto member = ast::make_expr("AstTailDotMember", span);
  member->string_field("name", "push!");
  member->bool_field("chain_boundary", false);
  auto call = ast::make_expr("AstTailCall", span);
  call->string_field("call_style", "paren");
  std::vector<std::unique_ptr<ast::Expr>> args;
  args.push_back(std::move(quote));
  call->list_field("args", std::move(args));
  std::vector<std::unique_ptr<ast::Expr>> tails;
  tails.push_back(std::move(member));
  tails.push_back(std::move(call));
  auto chain = ast::make_expr("AstPostfixChain", span);
  chain->node_field("base", std::move(base));
  chain->list_field("tails", std::move(tails));
  auto wrapper = ast::make_expr("AstExprStmt", span);
  wrapper->node_field("expr", std::move(chain));
  return wrapper;
}

void lower_control_statements(std::vector<std::unique_ptr<ast::Expr>> &stmts);

// Statement lists inside a `%`-control statement that may mix further
// template and control lines: control-flow bodies (`%if`/`%while`/…) and the
// trailing block suffix of the control line itself (`%exprs.each |e|:`).
// Anything else on a control line — lambda bodies, nested defs — is ordinary
// compile-time code and is left untouched.
void lower_control_bodies(ast::Expr &stmt) {
  const ast::Expr *inner_view =
      stmt.kind == "AstExprStmt" ? node_field(stmt, "expr") : &stmt;
  if (inner_view == nullptr) {
    return;
  }
  ast::Expr &inner = const_cast<ast::Expr &>(*inner_view);
  if (inner.kind == "AstIf" || inner.kind == "AstUnless") {
    for (ast::ListField &field : inner.list_fields) {
      if (field.name == "then_body" || field.name == "else_body") {
        lower_control_statements(field.values);
      }
    }
    return;
  }
  if (inner.kind == "AstWhile" || inner.kind == "AstUntil" ||
      inner.kind == "AstDoWhile" || inner.kind == "AstLoop") {
    for (ast::ListField &field : inner.list_fields) {
      if (field.name == "body") {
        lower_control_statements(field.values);
      }
    }
    return;
  }
  if (inner.kind == "AstPostfixChain") {
    ast::ListField *tails = mutable_list_field(inner, "tails");
    if (tails == nullptr || tails->values.empty() || !tails->values.back() ||
        tails->values.back()->kind != "AstTailBlockSuffix") {
      return;
    }
    for (ast::NodeField &field : tails->values.back()->node_fields) {
      if (field.name == "block" && field.value) {
        for (ast::ListField &block_field : field.value->list_fields) {
          if (block_field.name == "body") {
            lower_control_statements(block_field.values);
          }
        }
      }
    }
  }
}

// §6 `%`-control lowering: control lines run as written at expansion time;
// template lines become per-line `push!(quote: …)` emissions into the
// accumulator, so a control loop emits one node per iteration.
void lower_control_statements(std::vector<std::unique_ptr<ast::Expr>> &stmts) {
  for (std::unique_ptr<ast::Expr> &stmt : stmts) {
    if (!stmt) {
      continue;
    }
    if (is_macro_control(*stmt)) {
      strip_control_marker(*stmt);
      lower_control_bodies(*stmt);
      continue;
    }
    stmt = make_emit_push(std::move(stmt));
  }
}

// Wrap a template-default macro body in the implicit quote it desugars to:
// `body...` becomes `quote: body...` as the single (returned) expression. A
// body with `%`-control lines instead lowers to the accumulator kernel:
//   __macro_emitted = []
//   <control lines as written; template lines push per-line quotes>
//   return __macro_emitted
// The macro then returns List[Ast] — statement-position callers splice the
// nodes as siblings; expression-position callers accept a single element.
void wrap_template_body(ast::Expr &def) {
  ast::ListField *body = mutable_list_field(def, "body");
  if (body == nullptr || is_kernel_style_body(body->values)) {
    return;
  }
  lexer::Span span = def.span;
  if (!body->values.empty() && body->values.front() && body->values.back()) {
    span = ast::join_spans(body->values.front()->span,
                           body->values.back()->span);
  }
  if (body_has_control_lines(body->values)) {
    lower_control_statements(body->values);

    auto init_name = ast::make_expr("AstName", span);
    init_name->string_field("name", kEmitAccumulator);
    auto init_list = ast::make_expr("AstListLiteral", span);
    init_list->list_field("elements", {});
    auto init_assign = ast::make_expr("AstAssign", span);
    init_assign->string_field("op", "=");
    init_assign->node_field("left", std::move(init_name));
    init_assign->node_field("right", std::move(init_list));
    auto init_stmt = ast::make_expr("AstExprStmt", span);
    init_stmt->node_field("expr", std::move(init_assign));

    auto return_name = ast::make_expr("AstName", span);
    return_name->string_field("name", kEmitAccumulator);
    auto return_expr = ast::make_expr("AstReturn", span);
    return_expr->node_field("value", std::move(return_name));
    auto return_stmt = ast::make_expr("AstExprStmt", span);
    return_stmt->node_field("expr", std::move(return_expr));

    std::vector<std::unique_ptr<ast::Expr>> lowered;
    lowered.reserve(body->values.size() + 2U);
    lowered.push_back(std::move(init_stmt));
    for (std::unique_ptr<ast::Expr> &stmt : body->values) {
      lowered.push_back(std::move(stmt));
    }
    lowered.push_back(std::move(return_stmt));
    body->values = std::move(lowered);
    return;
  }
  auto quote = ast::make_expr("AstQuote", span);
  quote->list_field("body", std::move(body->values));
  auto stmt = ast::make_expr("AstExprStmt", span);
  stmt->node_field("expr", std::move(quote));
  body->values.clear();
  body->values.push_back(std::move(stmt));
}

const bytecode::BcMethod *method_by_name(const bytecode::BcModule &module,
                                         const std::string &name) {
  for (const bytecode::BcMethod &method : module.methods) {
    if (method.selector_sym_id < module.symbols.size() &&
        module.symbols[method.selector_sym_id] == name) {
      return &method;
    }
  }
  return nullptr;
}

// The compile-time macro namespace: invocation spelling ("m" for local and
// from-imported macros, "pg.sql" for module-alias imports, §11) mapped to the
// selector the definition compiles under in the macro module (identical for
// plain names; a mangled identifier for dotted spellings).
using MacroNameTable = std::map<std::string, std::string>;

// A macro invocation (DESIGN-macro-system §8 trigger surfaces): the callee is
// a bare macro name — or a dotted `alias.name` head resolving through a
// module-alias import — and the tails are one call channel and/or one block
// suffix. Recognized shapes: `m(args)` / `m args` (AstTailCall), `m.(args)`
// (explicit dot-call, AstTailDotCall), `m(args) |p|: body` / `m(): body`
// (call + AstTailBlockSuffix), and the paren-less statement form `m:` + INDENT
// (AstTailBlockSuffix alone). Longer chains (`m(1).abs`) stay runtime code.
struct MacroCall {
  std::string name;                     // invocation spelling (may be dotted)
  const ast::Expr *call_tail = nullptr; // AstTailCall / AstTailDotCall or null
  const ast::Expr *block = nullptr;     // AstBlock of a block suffix or null
  lexer::Span span;                     // invocation site, for diagnostics
};

// The invocation spelling of a chain head: the base name, extended to
// `base.member` when the first tail is a dot member (module-alias macro
// calls). Returns the number of tails the head consumed via `head_tails`.
std::string chain_head_spelling(const ast::Expr &base,
                                const ast::ListField &tails,
                                std::size_t *head_tails) {
  *head_tails = 0;
  const std::string *name = string_field(base, "name");
  if (name == nullptr) {
    return {};
  }
  if (!tails.values.empty() && tails.values[0] &&
      tails.values[0]->kind == "AstTailDotMember") {
    if (const std::string *member = string_field(*tails.values[0], "name")) {
      *head_tails = 1;
      return *name + "." + *member;
    }
  }
  return *name;
}

// Call-site suffix for expansion diagnostics (§12): "… (at file:line:col)".
std::string at_location(const lexer::Span &span) {
  if (span.file.empty()) {
    return {};
  }
  return " (at " + span.file + ":" + std::to_string(span.start.line) + ":" +
         std::to_string(span.start.col) + ")";
}

std::optional<MacroCall>
match_macro_call(const ast::Expr &expr, const MacroNameTable &macro_names) {
  if (expr.kind != "AstPostfixChain") {
    return std::nullopt;
  }
  const ast::Expr *base = node_field(expr, "base");
  const ast::ListField *tails = list_field(expr, "tails");
  if (base == nullptr || base->kind != "AstName" || tails == nullptr ||
      tails->values.empty() || tails->values.size() > 3) {
    return std::nullopt;
  }
  std::size_t index = 0;
  const std::string spelling = chain_head_spelling(*base, *tails, &index);
  if (spelling.empty() ||
      macro_names.find(spelling) == macro_names.end()) {
    return std::nullopt;
  }
  MacroCall call{spelling, nullptr, nullptr, expr.span};
  if (index < tails->values.size() && tails->values[index] &&
      (tails->values[index]->kind == "AstTailCall" ||
       tails->values[index]->kind == "AstTailDotCall")) {
    call.call_tail = tails->values[index].get();
    ++index;
  }
  if (index < tails->values.size()) {
    if (!tails->values[index] ||
        tails->values[index]->kind != "AstTailBlockSuffix") {
      return std::nullopt;
    }
    call.block = node_field(*tails->values[index], "block");
    if (call.block == nullptr) {
      return std::nullopt;
    }
    ++index;
  }
  if (index != tails->values.size() ||
      (call.call_tail == nullptr && call.block == nullptr)) {
    return std::nullopt;
  }
  return call;
}

// `use m` — the injection-macro trigger (§8.4): a bare call whose callee is
// the literal name `use` and whose single argument names a macro. The macro
// receives the enclosing class/mixin declaration Ast and expands to member
// declarations spliced in place of the `use` statement. When the argument
// does not resolve to a macro the statement is left alone (`use` stays an
// ordinary identifier).
struct UseTrigger {
  std::string name;
  lexer::Span span;
};

std::optional<UseTrigger>
match_use_trigger(const ast::Expr &expr, const MacroNameTable &macro_names) {
  if (expr.kind != "AstPostfixChain") {
    return std::nullopt;
  }
  const ast::Expr *base = node_field(expr, "base");
  const ast::ListField *tails = list_field(expr, "tails");
  if (base == nullptr || base->kind != "AstName" || tails == nullptr ||
      tails->values.size() != 1 || !tails->values[0] ||
      tails->values[0]->kind != "AstTailCall") {
    return std::nullopt;
  }
  const std::string *callee = string_field(*base, "name");
  if (callee == nullptr || *callee != "use") {
    return std::nullopt;
  }
  const ast::ListField *args = list_field(*tails->values[0], "args");
  if (args == nullptr || args->values.size() != 1 || !args->values[0]) {
    return std::nullopt;
  }
  // The argument names the macro: a plain name, or a dotted `alias.name`
  // head (a bare chain of one dot member) for module-alias imports.
  std::string spelling;
  const ast::Expr &arg = *args->values[0];
  if (arg.kind == "AstName") {
    if (const std::string *macro = string_field(arg, "name")) {
      spelling = *macro;
    }
  } else if (arg.kind == "AstPostfixChain") {
    const ast::Expr *arg_base = node_field(arg, "base");
    const ast::ListField *arg_tails = list_field(arg, "tails");
    if (arg_base != nullptr && arg_base->kind == "AstName" &&
        arg_tails != nullptr && arg_tails->values.size() == 1) {
      std::size_t head_tails = 0;
      const std::string dotted =
          chain_head_spelling(*arg_base, *arg_tails, &head_tails);
      if (head_tails == 1) {
        spelling = dotted;
      }
    }
  }
  if (spelling.empty() ||
      macro_names.find(spelling) == macro_names.end()) {
    return std::nullopt;
  }
  return UseTrigger{spelling, expr.span};
}

// `name"""…"""` — the string-tag trigger (§8.5): a bare call whose single
// argument is a block string literal ADJACENT to the tag head. The parser
// already produces this shape (an identifier juxtaposed with a text block is
// a bare call); adjacency — no whitespace between the identifier and the
// `"""` opener — is what makes it a tag invocation, so `sql """…"""` with a
// space stays an ordinary (non-tag) call. The macro receives the literal
// re-kinded as `Ast.StringTemplate`: the same parts model (post-dedent static
// chunks plus unevaluated interpolant ASTs), a distinct kind because a
// template handed to a macro is data, not a Str expression.
struct StringTagCall {
  std::string name;
  const ast::Expr *literal = nullptr;
  lexer::Span span;
};

std::optional<StringTagCall>
match_string_tag(const ast::Expr &expr,
                 const std::set<std::string> &string_tag_names) {
  if (string_tag_names.empty() || expr.kind != "AstPostfixChain") {
    return std::nullopt;
  }
  const ast::Expr *base = node_field(expr, "base");
  const ast::ListField *tails = list_field(expr, "tails");
  if (base == nullptr || base->kind != "AstName" || tails == nullptr ||
      tails->values.empty() || tails->values.size() > 2) {
    return std::nullopt;
  }
  std::size_t index = 0;
  const std::string spelling = chain_head_spelling(*base, *tails, &index);
  if (spelling.empty() ||
      string_tag_names.find(spelling) == string_tag_names.end()) {
    return std::nullopt;
  }
  if (tails->values.size() != index + 1 || !tails->values[index] ||
      tails->values[index]->kind != "AstTailCall") {
    return std::nullopt;
  }
  const ast::Expr &call_tail = *tails->values[index];
  const std::string *style = string_field(call_tail, "call_style");
  const ast::ListField *args = list_field(call_tail, "args");
  if (style == nullptr || *style != "bare" || args == nullptr ||
      args->values.size() != 1 || !args->values[0] ||
      args->values[0]->kind != "AstStringLiteral") {
    return std::nullopt;
  }
  const ast::Expr &literal = *args->values[0];
  const std::string *quote_kind = string_field(literal, "quote_kind");
  if (quote_kind == nullptr || *quote_kind != "block") {
    return std::nullopt;
  }
  // The tag head ends at the base name or, for a dotted head, at the dot
  // member; the opener must touch it (whitespace makes an ordinary call).
  const lexer::Span &head_span =
      index == 1 ? tails->values[0]->span : base->span;
  if (literal.span.start.offset != head_span.end.offset) {
    return std::nullopt; // whitespace before the opener: not a tag
  }
  return StringTagCall{spelling, &literal, expr.span};
}

// Wrap an AST subtree as an `Ast` value, carrying the module source so the
// macro body can recover the argument's verbatim text via `.source`.
runtime::Value make_ast_value(const ast::Expr &node,
                              std::shared_ptr<const std::string> source) {
  auto root = std::shared_ptr<const ast::Expr>(ast::clone_expr(node));
  auto value = std::make_shared<runtime::RuntimeAstNode>();
  value->root = root;
  value->node = root.get();
  value->source = std::move(source);
  return runtime::Value::ast_node(value);
}

// Declarations an annotation stack can attach to (§8 attachment rule). Macro
// defs are excluded: they are compile-time only and dropped before runtime.
bool is_declaration(const ast::Expr &expr) {
  return !is_macro_def(expr) &&
         (expr.kind == "AstDefStmt" || expr.kind == "AstClauseDef" ||
          expr.kind == "AstClassDef" || expr.kind == "AstClassMethodDef" ||
          expr.kind == "AstPropDef" || expr.kind == "AstClassPropDef");
}

// Statement/declaration kinds that stand alone in a body list; anything else
// a macro returns into statement position gets an AstExprStmt wrapper.
bool is_statement_kind(const std::string &kind) {
  return kind == "AstExprStmt" || kind == "AstDefStmt" ||
         kind == "AstClauseDef" || kind == "AstClassDef" ||
         kind == "AstClassMethodDef" || kind == "AstPropDef" ||
         kind == "AstClassPropDef" || kind == "AstReturn";
}

// List fields that hold sibling statements/declarations — the positions where
// a macro may expand to more than one node (§8 expansion cardinality).
bool is_statement_list(const std::string &field_name) {
  if (field_name == "body") {
    return true;
  }
  const std::string suffix = "_body";
  return field_name.size() > suffix.size() &&
         field_name.compare(field_name.size() - suffix.size(), suffix.size(),
                            suffix) == 0;
}

// §8 attachment rule: annotations bind to the declaration below only when the
// lines touch (a blank line — or anything else — between them breaks the run).
bool lines_adjacent(const ast::Expr &above, const ast::Expr &below) {
  return below.span.start.line == above.span.end.line + 1;
}

class Expander {
public:
  Expander(bytecode::BcModule macro_module, MacroNameTable macro_names,
           std::map<std::string, int> annotation_arity,
           std::set<std::string> string_tag_names,
           std::shared_ptr<const std::string> source)
      : macro_module_(std::move(macro_module)),
        macro_names_(std::move(macro_names)),
        annotation_arity_(std::move(annotation_arity)),
        string_tag_names_(std::move(string_tag_names)),
        source_(std::move(source)) {}

  ExpandResult result() const { return result_; }

  // Expression-position walk: a macro call here must expand to exactly one
  // expression (§8 cardinality); statement lists hand off to
  // expand_statements, where multi-node expansion and annotations live.
  void expand(std::unique_ptr<ast::Expr> &slot, int depth) {
    if (!result_.ok || !slot) {
      return;
    }
    if (std::optional<StringTagCall> tag =
            match_string_tag(*slot, string_tag_names_)) {
      // Hand-off (§8.5 / multiline design §7): the literal re-kinded as
      // Ast.StringTemplate becomes the tag macro's single argument; the
      // macro must return exactly one expression Ast.
      std::unique_ptr<ast::Expr> template_node = ast::clone_expr(*tag->literal);
      template_node->kind = "AstStringTemplate";
      std::vector<runtime::Value> args;
      args.push_back(make_ast_value(*template_node, source_));
      const std::optional<runtime::Value> value =
          execute_macro(tag->name, tag->span, std::move(args), {}, depth);
      if (!value.has_value()) {
        return;
      }
      splice_expression_result(slot, tag->name, tag->span, *value, depth);
      return;
    }
    if (std::optional<MacroCall> call = match_macro_call(*slot, macro_names_)) {
      if (!check_call_surface(*call)) {
        return;
      }
      const std::optional<runtime::Value> value = run_macro(*call, depth);
      if (!value.has_value()) {
        return;
      }
      splice_expression_result(slot, call->name, call->span, *value, depth);
      return;
    }
    for (ast::NodeField &field : slot->node_fields) {
      expand(field.value, depth);
    }
    for (ast::ListField &field : slot->list_fields) {
      if (is_statement_list(field.name)) {
        expand_statements(field.values, depth, slot.get());
      } else {
        for (std::unique_ptr<ast::Expr> &child : field.values) {
          expand(child, depth);
        }
      }
    }
  }

  // Statement-list walk: recognizes annotation runs (§8.3), `use` injection
  // triggers (§8.4, `enclosing` is the owning declaration node),
  // statement-position macro calls with multi-node results (AstBlock body or
  // List[Ast] splice as siblings), and macro defs (left for the driver to
  // drop).
  void expand_statements(std::vector<std::unique_ptr<ast::Expr>> &list,
                         int depth, const ast::Expr *enclosing = nullptr) {
    if (!result_.ok) {
      return;
    }
    if (depth >= kExpansionDepthLimit) {
      fail("AMB_MACRO_EXPANSION_LIMIT: macro expansion exceeded depth limit");
      return;
    }
    std::vector<std::unique_ptr<ast::Expr>> out;
    out.reserve(list.size());
    for (std::size_t i = 0; i < list.size() && result_.ok;) {
      // Annotation run: contiguous annotation-shaped macro calls attached to
      // the adjacent declaration below, composed in source order.
      const std::size_t run = annotation_run_at(list, i);
      if (run > 0) {
        // Backtrace frame for the run: the top (first-applied) annotation.
        const std::optional<MacroCall> top_call =
            match_macro_call(*statement_call_expr(list[i].get()), macro_names_);
        std::vector<std::unique_ptr<ast::Expr>> results =
            apply_annotation_stack(list, i, run, depth);
        if (!result_.ok) {
          return;
        }
        const ExpansionScope scope(*this, top_call->name, top_call->span);
        expand_statements(results, depth + 1, enclosing);
        for (std::unique_ptr<ast::Expr> &node : results) {
          out.push_back(std::move(node));
        }
        i += run + 1;
        continue;
      }
      std::unique_ptr<ast::Expr> &elem = list[i];
      if (elem && is_macro_def(*elem)) {
        out.push_back(std::move(elem));
        ++i;
        continue;
      }
      // `use m`: run the injection macro with the enclosing class/mixin
      // declaration and splice the returned members in place.
      if (const ast::Expr *call_expr = statement_call_expr(elem.get())) {
        if (std::optional<UseTrigger> trigger =
                match_use_trigger(*call_expr, macro_names_)) {
          if (string_tag_names_.count(trigger->name) != 0) {
            fail("macro `" + trigger->name +
                 "` is a string_tag macro and cannot be used with `use`" +
                 at_location(trigger->span));
            return;
          }
          if (enclosing == nullptr ||
              (enclosing->kind != "AstClassDef" &&
               enclosing->kind != "AstMixinDef")) {
            fail("`use " + trigger->name +
                 "` is only supported inside a class or mixin body (v1)" +
                 at_location(trigger->span));
            return;
          }
          const MacroCall call{trigger->name, nullptr, nullptr, trigger->span};
          const std::optional<runtime::Value> value =
              run_macro(call, depth, enclosing);
          if (!value.has_value()) {
            return;
          }
          std::vector<std::unique_ptr<ast::Expr>> results;
          if (!splice_statement_result(*value, call.name, call.span,
                                       &results)) {
            return;
          }
          {
            const ExpansionScope scope(*this, call.name, call.span);
            expand_statements(results, depth + 1, enclosing);
          }
          for (std::unique_ptr<ast::Expr> &node : results) {
            out.push_back(std::move(node));
          }
          ++i;
          continue;
        }
      }
      if (const ast::Expr *call_expr = statement_call_expr(elem.get())) {
        // A statement-position tag invocation is handled by the expression
        // walk below (it expands to one expression); it must not be
        // classified as an ordinary macro call here.
        std::optional<MacroCall> call =
            match_string_tag(*call_expr, string_tag_names_).has_value()
                ? std::optional<MacroCall>{}
                : match_macro_call(*call_expr, macro_names_);
        if (call.has_value()) {
          if (!check_call_surface(*call)) {
            return;
          }
          // An annotation-shaped call (declared arity = args + 1) that did not
          // form a run has no declaration under it.
          if (annotation_shape_arity(*call)) {
            fail("AMB_MACRO_DANGLING_ANNOTATION: annotation macro `" +
                 call->name +
                 "` has no declaration immediately below it (a blank line or "
                 "intervening statement breaks the annotation run)" +
                 at_location(call->span));
            return;
          }
          const std::optional<runtime::Value> value = run_macro(*call, depth);
          if (!value.has_value()) {
            return;
          }
          std::vector<std::unique_ptr<ast::Expr>> results;
          if (!splice_statement_result(*value, call->name, call->span,
                                       &results)) {
            return;
          }
          {
            const ExpansionScope scope(*this, call->name, call->span);
            expand_statements(results, depth + 1, enclosing);
          }
          for (std::unique_ptr<ast::Expr> &node : results) {
            out.push_back(std::move(node));
          }
          ++i;
          continue;
        }
      }
      expand(elem, depth);
      out.push_back(std::move(elem));
      ++i;
    }
    if (result_.ok) {
      list = std::move(out);
    }
  }

private:
  // One frame of the expansion backtrace (§12): a macro whose spliced output
  // is currently being re-expanded. Diagnostics raised while walking that
  // output chain through here, innermost expansion first.
  struct ExpansionFrame {
    std::string name;
    lexer::Span span;
  };

  class ExpansionScope {
  public:
    ExpansionScope(Expander &expander, const std::string &name,
                   const lexer::Span &span)
        : expander_(expander) {
      expander_.expansion_stack_.push_back(ExpansionFrame{name, span});
    }
    ~ExpansionScope() { expander_.expansion_stack_.pop_back(); }
    ExpansionScope(const ExpansionScope &) = delete;
    ExpansionScope &operator=(const ExpansionScope &) = delete;

  private:
    Expander &expander_;
  };

  // "expanded from macro `x` (at …)" lines, innermost expansion first. A
  // runaway recursion would produce a depth-limit chain of up to 128 frames;
  // show the closest few and summarize the rest.
  std::string expansion_backtrace() const {
    if (expansion_stack_.empty()) {
      return {};
    }
    constexpr std::size_t kMaxShownFrames = 6;
    std::string out;
    std::size_t shown = 0;
    for (auto it = expansion_stack_.rbegin(); it != expansion_stack_.rend();
         ++it) {
      if (shown == kMaxShownFrames) {
        out += "\n  … (" +
               std::to_string(expansion_stack_.size() - shown) +
               " more expansion frames)";
        break;
      }
      out += "\n  expanded from macro `" + it->name + "`" +
             at_location(it->span);
      ++shown;
    }
    return out;
  }

  void fail(const std::string &message) {
    if (result_.ok) {
      result_.ok = false;
      result_.error = message + expansion_backtrace();
    }
  }

  std::string fresh_mark() { return "h" + std::to_string(++hygiene_counter_); }

  // The macro-call expression carried by a statement slot (directly or under
  // an AstExprStmt wrapper).
  static const ast::Expr *statement_call_expr(const ast::Expr *elem) {
    if (elem == nullptr) {
      return nullptr;
    }
    if (elem->kind == "AstExprStmt") {
      return node_field(*elem, "expr");
    }
    return elem;
  }

  // Annotation classification (v1): a call is annotation-shaped when the
  // macro's declared arity is exactly one more than the arguments passed —
  // the extra slot receives the annotated declaration's Ast. Macros with
  // rest/keyword/defaulted params are not annotation-capable in v1, and only
  // plain positional call arguments count (a keyword, spread, or block-pass
  // argument makes the +1 arity test meaningless for a positional-only
  // macro).
  bool annotation_shape_arity(const MacroCall &call) const {
    if (call.block != nullptr) {
      return false;
    }
    const auto it = annotation_arity_.find(call.name);
    if (it == annotation_arity_.end() || it->second < 0) {
      return false;
    }
    std::size_t arg_count = 0;
    if (call.call_tail != nullptr) {
      if (const ast::ListField *args = list_field(*call.call_tail, "args")) {
        for (const std::unique_ptr<ast::Expr> &arg : args->values) {
          if (!arg) {
            continue;
          }
          if (arg->kind == "AstKeywordArg" ||
              arg->kind == "AstKeywordSpreadArg" ||
              arg->kind == "AstSpreadArg" || arg->kind == "AstBlockPass") {
            return false;
          }
          ++arg_count;
        }
      }
    }
    return static_cast<std::size_t>(it->second) == arg_count + 1;
  }

  // Length of the annotation run starting at `i`: one or more line-adjacent
  // annotation-shaped statements followed by an adjacent declaration. Returns
  // 0 when there is no properly attached run (a lone annotation-shaped call
  // then reports AMB_MACRO_DANGLING_ANNOTATION from the statement path).
  std::size_t
  annotation_run_at(const std::vector<std::unique_ptr<ast::Expr>> &list,
                    std::size_t i) const {
    std::size_t end = i;
    while (end < list.size() && list[end]) {
      const ast::Expr *call_expr = statement_call_expr(list[end].get());
      if (call_expr == nullptr) {
        break;
      }
      const std::optional<MacroCall> call =
          match_macro_call(*call_expr, macro_names_);
      if (!call.has_value() || !annotation_shape_arity(*call)) {
        break;
      }
      if (end > i && !lines_adjacent(*list[end - 1], *list[end])) {
        break;
      }
      ++end;
    }
    if (end == i) {
      return 0;
    }
    if (end < list.size() && list[end] && is_declaration(*list[end]) &&
        lines_adjacent(*list[end - 1], *list[end])) {
      return end - i;
    }
    return 0;
  }

  // Apply a stack of annotations to the declaration below them, top-to-bottom
  // (source order, §8): each macro receives its own arguments plus the current
  // declaration Ast (as the final argument) and returns the replacement. Only
  // the last annotation may expand to multiple declarations.
  std::vector<std::unique_ptr<ast::Expr>>
  apply_annotation_stack(std::vector<std::unique_ptr<ast::Expr>> &list,
                         std::size_t i, std::size_t run, int depth) {
    std::unique_ptr<ast::Expr> current = std::move(list[i + run]);
    std::vector<std::unique_ptr<ast::Expr>> results;
    for (std::size_t k = 0; k < run && result_.ok; ++k) {
      const ast::Expr *call_expr = statement_call_expr(list[i + k].get());
      const std::optional<MacroCall> call =
          match_macro_call(*call_expr, macro_names_);
      const std::optional<runtime::Value> value =
          run_macro(*call, depth, current.get());
      if (!value.has_value()) {
        return {};
      }
      if (value->is_ast_node()) {
        const std::shared_ptr<runtime::RuntimeAstNode> node =
            value->as_ast_node();
        if (node == nullptr || node->node == nullptr) {
          fail("macro `" + call->name + "` returned a null Ast value");
          return {};
        }
        if (node->node->kind != "AstBlock") {
          current = ast::clone_expr(*node->node);
          apply_hygiene(*current, fresh_mark(), call->span);
          continue;
        }
      }
      // Multi-declaration result: legal only for the last annotation in the
      // stack (there is no single declaration left for the next one to see).
      if (k + 1 != run) {
        fail("annotation macro `" + call->name +
             "` expanded to multiple declarations but is not the last "
             "annotation on its declaration" + at_location(call->span));
        return {};
      }
      if (!splice_statement_result(*value, call->name, call->span,
                                   &results)) {
        return {};
      }
      current.reset();
    }
    if (current) {
      results.push_back(std::move(current));
    }
    return results;
  }

  // Run one macro at expansion time. `extra_decl` is the annotated
  // declaration appended as the trailing argument for attribute macros.
  // A string_tag macro is only invocable through its tag surface; reaching it
  // via an ordinary call channel (spaced literal, paren call, block suffix)
  // is a located diagnostic rather than a silent wrong-channel expansion.
  bool check_call_surface(const MacroCall &call) {
    if (string_tag_names_.find(call.name) == string_tag_names_.end()) {
      return true;
    }
    fail("macro `" + call.name + "` is a string_tag macro; invoke it as `" +
         call.name +
         "\"\"\"…\"\"\"` with the text-block opener adjacent to the tag" +
         at_location(call.span));
    return false;
  }

  // Expression-position result splice (§8 cardinality): exactly one
  // expression Ast, hygiene-marked, then re-expanded for the fixpoint.
  void splice_expression_result(std::unique_ptr<ast::Expr> &slot,
                                const std::string &name,
                                const lexer::Span &span,
                                const runtime::Value &value, int depth) {
    // A `%`-template returns List[Ast]; in expression position it is usable
    // only when it emitted exactly one node.
    runtime::Value unwrapped = value;
    if (value.is_list()) {
      const auto list = value.as_list();
      if (list == nullptr || list->items.size() != 1U) {
        fail("macro `" + name + "` emitted " +
             std::to_string(list == nullptr ? 0 : list->items.size()) +
             " nodes; expression position needs exactly one" +
             at_location(span));
        return;
      }
      unwrapped = list->items[0];
    }
    if (!unwrapped.is_ast_node()) {
      fail("macro `" + name +
           "` must return exactly one expression Ast in expression position" +
           at_location(span));
      return;
    }
    const std::shared_ptr<runtime::RuntimeAstNode> node =
        unwrapped.as_ast_node();
    if (node == nullptr || node->node == nullptr) {
      fail("macro `" + name + "` returned a null Ast value");
      return;
    }
    if (node->node->kind == "AstBlock") {
      fail("macro `" + name +
           "` expanded to a statement block in expression position; a "
           "multi-statement macro can only be used as a statement" +
           at_location(span));
      return;
    }
    slot = ast::clone_expr(*node->node);
    apply_hygiene(*slot, fresh_mark(), span);
    const ExpansionScope scope(*this, name, span);
    expand(slot, depth + 1);
  }

  // One call-site argument routed into the macro ABI (§8 call channels):
  // positionals carry the argument Ast; `name: expr` keywords carry the value
  // Ast under the keyword; `*literal` / `**literal` spreads splice at compile
  // time (macro arguments are unevaluated ASTs, so only literal element packs
  // have knowable elements); `&name` routes the named local's Ast into the
  // block channel.
  bool append_macro_argument(
      const MacroCall &call, const ast::Expr &arg,
      std::vector<runtime::Value> *arg_values,
      std::vector<std::pair<std::string, runtime::Value>> *kw_values,
      const ast::Expr **block_pass) {
    if (arg.kind == "AstKeywordArg") {
      const std::string *name = string_field(arg, "name");
      const ast::Expr *value = node_field(arg, "value");
      if (name == nullptr || value == nullptr) {
        fail("macro `" + call.name + "` received a malformed keyword argument" +
             at_location(call.span));
        return false;
      }
      kw_values->emplace_back(*name, make_ast_value(*value, source_));
      return true;
    }
    if (arg.kind == "AstSpreadArg") {
      const ast::Expr *operand = node_field(arg, "expr");
      if (operand == nullptr || (operand->kind != "AstListLiteral" &&
                                 operand->kind != "AstTupleLiteral")) {
        fail("macro `" + call.name +
             "` was called with a `*` spread whose operand is not a list or "
             "tuple literal; macro arguments are unevaluated ASTs, so only "
             "literal elements can be spliced at compile time" +
             at_location(call.span));
        return false;
      }
      if (const ast::ListField *elements = list_field(*operand, "elements")) {
        for (const std::unique_ptr<ast::Expr> &element : elements->values) {
          if (!element) {
            continue;
          }
          if (element->kind == "AstArraySpread" ||
              element->kind == "AstSetSpread" ||
              element->kind == "AstArrayElement" ||
              element->kind == "AstSetElement") {
            fail("macro `" + call.name +
                 "` was called with a `*` spread whose literal has a nested "
                 "spread or conditional element, which cannot be expanded at "
                 "compile time" + at_location(call.span));
            return false;
          }
          arg_values->push_back(make_ast_value(*element, source_));
        }
      }
      return true;
    }
    if (arg.kind == "AstKeywordSpreadArg") {
      const ast::Expr *operand = node_field(arg, "expr");
      if (operand == nullptr || operand->kind != "AstMapLiteral") {
        fail("macro `" + call.name +
             "` was called with a `**` spread whose operand is not a map "
             "literal; macro arguments are unevaluated ASTs, so only literal "
             "entries can be spliced at compile time" + at_location(call.span));
        return false;
      }
      if (const ast::ListField *entries = list_field(*operand, "entries")) {
        for (const std::unique_ptr<ast::Expr> &entry : entries->values) {
          if (!entry) {
            continue;
          }
          const std::string *key_kind =
              entry->kind == "AstMapEntry" ? string_field(*entry, "key_kind")
                                           : nullptr;
          const std::string *key = string_field(*entry, "key");
          const ast::Expr *value = node_field(*entry, "value");
          if (key_kind == nullptr || *key_kind != "symbol" || key == nullptr ||
              value == nullptr ||
              node_field(*entry, "condition") != nullptr) {
            fail("macro `" + call.name +
                 "` was called with a `**` spread entry that is not a plain "
                 "symbol-keyed literal entry, which cannot be expanded at "
                 "compile time" + at_location(call.span));
            return false;
          }
          kw_values->emplace_back(*key, make_ast_value(*value, source_));
        }
      }
      return true;
    }
    if (arg.kind == "AstBlockPass") {
      const ast::Expr *value = node_field(arg, "value");
      if (value == nullptr || *block_pass != nullptr) {
        fail("macro `" + call.name +
             "` received a malformed `&name` block-pass argument" +
             at_location(call.span));
        return false;
      }
      *block_pass = value;
      return true;
    }
    arg_values->push_back(make_ast_value(arg, source_));
    return true;
  }

  std::optional<runtime::Value> run_macro(const MacroCall &call, int depth,
                                          const ast::Expr *extra_decl = nullptr) {
    std::vector<runtime::Value> arg_values;
    std::vector<std::pair<std::string, runtime::Value>> kw_values;
    const ast::Expr *block_pass = nullptr;
    if (call.call_tail != nullptr) {
      if (const ast::ListField *args = list_field(*call.call_tail, "args")) {
        for (const std::unique_ptr<ast::Expr> &arg : args->values) {
          if (!arg) {
            continue;
          }
          if (!append_macro_argument(call, *arg, &arg_values, &kw_values,
                                     &block_pass)) {
            return std::nullopt;
          }
        }
      }
    }
    // Block channel (§8 macro ABI, v1 spelling): the unevaluated block of a
    // block-suffix trigger — or the named local of a `&name` block-pass —
    // arrives as the trailing Ast argument (an AstBlock or an AstName; the
    // macro can distinguish them by `.kind`).
    if (call.block != nullptr && block_pass != nullptr) {
      fail("macro `" + call.name +
           "` received both a block suffix and a `&name` block-pass argument" +
           at_location(call.span));
      return std::nullopt;
    }
    if (call.block != nullptr) {
      arg_values.push_back(make_ast_value(*call.block, source_));
    } else if (block_pass != nullptr) {
      arg_values.push_back(make_ast_value(*block_pass, source_));
    }
    if (extra_decl != nullptr) {
      arg_values.push_back(make_ast_value(*extra_decl, source_));
    }
    return execute_macro(call.name, call.span, std::move(arg_values),
                         std::move(kw_values), depth);
  }

  // The symbol id for a keyword name in the macro module's symbol table,
  // interning it when absent (a `**kwargs`-collected extra keyword may not
  // appear anywhere in the compiled macro module).
  std::uint32_t intern_macro_symbol(const std::string &text) {
    for (std::uint32_t i = 0; i < macro_module_.symbols.size(); ++i) {
      if (macro_module_.symbols[i] == text) {
        return i;
      }
    }
    macro_module_.symbols.push_back(text);
    return static_cast<std::uint32_t>(macro_module_.symbols.size() - 1U);
  }

  // Shared execution path for every trigger surface: depth check, method
  // lookup, sandboxed run, fault mapping.
  std::optional<runtime::Value>
  execute_macro(const std::string &name, const lexer::Span &span,
                std::vector<runtime::Value> arg_values,
                std::vector<std::pair<std::string, runtime::Value>> kw_values,
                int depth) {
    if (depth >= kExpansionDepthLimit) {
      fail("AMB_MACRO_EXPANSION_LIMIT: macro expansion exceeded depth limit at `" +
           name + "`" + at_location(span));
      return std::nullopt;
    }
    const MacroCall call{name, nullptr, nullptr, span};

    const auto selector = macro_names_.find(call.name);
    const bytecode::BcMethod *method =
        selector != macro_names_.end()
            ? method_by_name(macro_module_, selector->second)
            : nullptr;
    if (method == nullptr) {
      fail("macro `" + call.name + "` has no compiled definition");
      return std::nullopt;
    }
    std::vector<std::pair<std::uint32_t, runtime::Value>> kw_args;
    kw_args.reserve(kw_values.size());
    for (std::pair<std::string, runtime::Value> &kw : kw_values) {
      kw_args.emplace_back(intern_macro_symbol(kw.first),
                           std::move(kw.second));
    }
    // Sandbox (§10): the expander VM runs with an armed capability gate,
    // nothing granted, and a step budget — macro IO and macro loops are
    // build diagnostics, not compile-time authority or hangs.
    runtime::RuntimeVmExecutionContext context;
    context.world_options = &sandbox_world_options();
    context.capabilities = &sandbox_no_capabilities();
    context.step_budget = kExpansionStepBudget;
    const runtime::ExecutionResult exec =
        runtime::execute_runtime_vm(macro_module_, std::move(context),
                                    method->entry_code_id, arg_values, kw_args,
                                    runtime::Value::null(),
                                    runtime::Value::null());
    if (exec.fault.has_value()) {
      if (exec.fault->error_name == "BudgetError") {
        fail("AMB_MACRO_BUDGET: macro `" + call.name +
             "` exceeded the compile-time step budget during expansion (" +
             std::to_string(kExpansionStepBudget) +
             " steps); macros must terminate" + at_location(call.span));
      } else if (exec.fault->error_name == "CapabilityError") {
        fail("AMB_MACRO_CAPABILITY: macro `" + call.name +
             "` attempted IO during expansion; the macro expander runs with "
             "no capabilities (" + exec.fault->message + ")" +
             at_location(call.span));
      } else {
        fail("macro `" + call.name + "` raised during expansion: " +
             exec.fault->error_name + ": " + exec.fault->message +
             at_location(call.span));
      }
      return std::nullopt;
    }
    return exec.value;
  }

  // Turn a macro's statement-position result into sibling statements: a
  // single Ast (statement-wrapped if needed), an AstBlock's statements, or a
  // List[Ast] of statements/declarations. All nodes from one execution share
  // one hygiene mark (a `tmp` in two emitted statements is the same `tmp`).
  bool splice_statement_result(const runtime::Value &value,
                               const std::string &name,
                               const lexer::Span &call_span,
                               std::vector<std::unique_ptr<ast::Expr>> *out) {
    const std::string mark = fresh_mark();
    if (value.is_ast_node()) {
      const std::shared_ptr<runtime::RuntimeAstNode> node = value.as_ast_node();
      if (node == nullptr || node->node == nullptr) {
        fail("macro `" + name + "` returned a null Ast value");
        return false;
      }
      flatten_statement(*node->node, mark, call_span, out);
      return result_.ok;
    }
    if (value.is_list()) {
      for (const runtime::Value &item : value.as_list()->items) {
        if (!item.is_ast_node()) {
          fail("macro `" + name +
               "` returned a List with a non-Ast element at statement "
               "position");
          return false;
        }
        const std::shared_ptr<runtime::RuntimeAstNode> node =
            item.as_ast_node();
        if (node == nullptr || node->node == nullptr) {
          fail("macro `" + name + "` returned a null Ast value");
          return false;
        }
        flatten_statement(*node->node, mark, call_span, out);
        if (!result_.ok) {
          return false;
        }
      }
      return true;
    }
    fail("macro `" + name +
         "` must return an Ast value (or a List of Ast) at statement "
         "position");
    return false;
  }

  // Splice one result node as statements: AstBlock bodies flatten into their
  // statements (recursively, so an unquoted block inside an emitted block
  // flattens too); expressions get an AstExprStmt wrapper.
  void flatten_statement(const ast::Expr &node, const std::string &mark,
                         const lexer::Span &call_span,
                         std::vector<std::unique_ptr<ast::Expr>> *out) {
    if (node.kind == "AstBlock") {
      if (const ast::ListField *body = list_field(node, "body")) {
        for (const std::unique_ptr<ast::Expr> &stmt : body->values) {
          if (stmt) {
            flatten_statement(*stmt, mark, call_span, out);
          }
        }
      }
      return;
    }
    if (node.kind == "AstExprStmt") {
      const ast::Expr *inner = node_field(node, "expr");
      if (inner != nullptr && inner->kind == "AstBlock") {
        flatten_statement(*inner, mark, call_span, out);
        return;
      }
    }
    std::unique_ptr<ast::Expr> copy = ast::clone_expr(node);
    apply_hygiene(*copy, mark, call_span);
    if (!is_statement_kind(copy->kind)) {
      auto stmt = ast::make_expr("AstExprStmt", copy->span);
      stmt->node_field("expr", std::move(copy));
      copy = std::move(stmt);
    }
    out->push_back(std::move(copy));
  }

  // Rewrite quote-introduced (hygienic-marked) identifiers to carry `mark` as
  // their syntax context, and drop the transient marker. Spliced/unquoted
  // subtrees carry no marker and are left untouched.
  void apply_hygiene(ast::Expr &node, const std::string &mark,
                     const lexer::Span &call_span) {
    // Macrotrace v1: macro-built nodes carry no source span (empty file);
    // retarget them to the invocation site so binder/runtime diagnostics in
    // generated code point at the macro call, not "(line 1)". Spliced caller
    // subtrees keep their own spans.
    if (node.span.file.empty()) {
      node.span = call_span;
    }
    if (node.kind == "AstName") {
      bool hygienic = false;
      node.bool_fields.erase(
          std::remove_if(node.bool_fields.begin(), node.bool_fields.end(),
                         [&](const ast::BoolField &field) {
                           if (field.name == "hygienic") {
                             hygienic = hygienic || field.value;
                             return true;
                           }
                           return false;
                         }),
          node.bool_fields.end());
      if (hygienic) {
        node.string_field("syntax_context", mark);
      }
    }
    for (ast::NodeField &field : node.node_fields) {
      if (field.value) {
        apply_hygiene(*field.value, mark, call_span);
      }
    }
    for (ast::ListField &field : node.list_fields) {
      for (std::unique_ptr<ast::Expr> &child : field.values) {
        if (child) {
          apply_hygiene(*child, mark, call_span);
        }
      }
    }
  }

  bytecode::BcModule macro_module_;
  // Invocation spelling (plain or dotted) -> compiled selector name.
  MacroNameTable macro_names_;
  // Declared fixed arity per macro (annotation eligibility); -1 when the
  // macro has rest/keyword/defaulted params and cannot be an annotation (v1).
  std::map<std::string, int> annotation_arity_;
  // Macros declared `string_tag` — invocable only via `name"""…"""`.
  std::set<std::string> string_tag_names_;
  std::shared_ptr<const std::string> source_;
  ExpandResult result_;
  int hygiene_counter_ = 0;
  // Macros whose spliced output is being re-expanded, outermost first (§12
  // expansion backtraces).
  std::vector<ExpansionFrame> expansion_stack_;
};

// Declared arity for annotation classification: the count of plain positional
// params, or -1 when any param is rest/keyword/defaulted (those shapes make
// the +1 arity test ambiguous, so such macros are not annotation-capable v1).
int annotation_arity_of(const ast::Expr &def) {
  const ast::Expr *signature = node_field(def, "signature");
  if (signature == nullptr) {
    return -1;
  }
  const ast::ListField *params = list_field(*signature, "params");
  if (params == nullptr) {
    return 0;
  }
  int arity = 0;
  for (const std::unique_ptr<ast::Expr> &param : params->values) {
    if (!param) {
      continue;
    }
    const std::string *kind = string_field(*param, "param_kind");
    if (kind == nullptr || *kind != "positional" ||
        node_field(*param, "default_expr") != nullptr) {
      return -1;
    }
    ++arity;
  }
  return arity;
}

// Compile the macro definitions as ordinary functions on their own module, so
// their bodies can run on the expander VM. `is_macro` is stripped (the macro
// module treats them as plain functions returning Ast) and their quote bodies
// are lowered to `Ast.node(...)` builder calls.
bool compile_macro_module(const std::vector<const ast::Expr *> &macro_defs,
                          const std::string &module_name,
                          bytecode::BcModule *out, std::string *error) {
  std::vector<std::unique_ptr<ast::Expr>> items;
  for (const ast::Expr *def : macro_defs) {
    std::unique_ptr<ast::Expr> copy = ast::clone_expr(*def);
    copy->bool_fields.erase(
        std::remove_if(copy->bool_fields.begin(), copy->bool_fields.end(),
                       [](const ast::BoolField &field) {
                         return field.name == "is_macro" ||
                                field.name == "is_string_tag";
                       }),
        copy->bool_fields.end());
    wrap_template_body(*copy);
    items.push_back(std::move(copy));
  }
  ast::expand_quotes(items);

  binder::BindResult bind = binder::bind_module(items, module_name);
  if (!bind.ok()) {
    *error = "macro definitions failed to bind";
    return false;
  }
  hir::Program program = hir::lower_module(items, module_name, bind.graph);
  bytecode::EmitResult emit = bytecode::emit_program(program, module_name);
  if (!emit.ok()) {
    *error = "macro definitions failed to compile";
    return false;
  }
  const std::vector<std::uint8_t> bytes =
      bytecode::serialize_module(emit.module);
  bytecode::DecodeResult decode = bytecode::deserialize_module(bytes);
  if (!decode.ok()) {
    *error = "macro module failed verification";
    return false;
  }
  *out = std::move(decode.module);
  return true;
}

// True when any node in the tree is a name reference to `name` — used after
// expansion to decide whether a macro-providing module alias still has
// runtime uses (if not, its import statement is compile-time only and is
// dropped).
bool tree_references_name(const ast::Expr &node, const std::string &name) {
  if (node.kind == "AstName") {
    const std::string *value = string_field(node, "name");
    if (value != nullptr && *value == name) {
      return true;
    }
  }
  for (const ast::NodeField &field : node.node_fields) {
    if (field.value && tree_references_name(*field.value, name)) {
      return true;
    }
  }
  for (const ast::ListField &field : node.list_fields) {
    for (const std::unique_ptr<ast::Expr> &child : field.values) {
      if (child && tree_references_name(*child, name)) {
        return true;
      }
    }
  }
  return false;
}

// Resolve the importer's `from <module> import name [as alias]` items against
// the staged provider table (§11). Every match yields an owned clone of the
// provider's macro def, renamed to the importer's local alias, ready to join
// the macro module alongside local defs.
std::vector<std::unique_ptr<ast::Expr>>
imported_macro_defs(const std::vector<std::unique_ptr<ast::Expr>> &items,
                    const MacroProviderMap &providers) {
  std::vector<std::unique_ptr<ast::Expr>> imported;
  for (const std::unique_ptr<ast::Expr> &item : items) {
    if (!item || item->kind != "AstImportStmt") {
      continue;
    }
    const std::string *import_kind = string_field(*item, "import_kind");
    const std::string *module_path = string_field(*item, "module_path");
    if (import_kind == nullptr || *import_kind != "from" ||
        module_path == nullptr) {
      continue;
    }
    const auto provider = providers.find(*module_path);
    if (provider == providers.end()) {
      continue;
    }
    const ast::ListField *names = list_field(*item, "names");
    if (names == nullptr) {
      continue;
    }
    for (const std::unique_ptr<ast::Expr> &name : names->values) {
      if (!name || name->kind != "AstImportName") {
        continue;
      }
      const std::string *source_name = string_field(*name, "source_name");
      const std::string *local_name = string_field(*name, "local_name");
      if (source_name == nullptr || local_name == nullptr) {
        continue;
      }
      for (const MacroExport &entry : provider->second) {
        if (entry.public_name != *source_name || entry.def == nullptr) {
          continue;
        }
        std::unique_ptr<ast::Expr> def = ast::clone_expr(*entry.def);
        for (ast::StringField &field : def->string_fields) {
          if (field.name == "name") {
            field.value = *local_name;
          }
        }
        imported.push_back(std::move(def));
        break;
      }
    }
  }
  return imported;
}

} // namespace

std::vector<MacroExport>
collect_macro_exports(const std::vector<std::unique_ptr<ast::Expr>> &items) {
  std::vector<MacroExport> exports;
  for (const std::unique_ptr<ast::Expr> &item : items) {
    if (!item || item->kind != "AstExportStmt") {
      continue;
    }
    const ast::ListField *export_items = list_field(*item, "items");
    if (export_items == nullptr) {
      continue;
    }
    for (const std::unique_ptr<ast::Expr> &entry : export_items->values) {
      if (!entry || entry->kind != "AstExportItem") {
        continue;
      }
      bool is_macro = false;
      for (const ast::BoolField &field : entry->bool_fields) {
        if (field.name == "is_macro") {
          is_macro = field.value;
        }
      }
      if (!is_macro) {
        continue;
      }
      const std::string *local_name = string_field(*entry, "local_name");
      const std::string *public_name = string_field(*entry, "public_name");
      if (local_name == nullptr || public_name == nullptr) {
        continue;
      }
      for (const std::unique_ptr<ast::Expr> &def : items) {
        if (def && is_macro_def(*def) &&
            string_field(*def, "name") != nullptr &&
            *string_field(*def, "name") == *local_name) {
          exports.push_back(
              MacroExport{*public_name, ast::clone_expr(*def)});
          break;
        }
      }
    }
  }
  return exports;
}

std::string serialize_macro_exports(const std::vector<MacroExport> &exports,
                                    const std::string &module_source) {
  std::vector<const MacroExport *> valid;
  for (const MacroExport &entry : exports) {
    if (entry.def != nullptr) {
      valid.push_back(&entry);
    }
  }
  std::string out = "amber.macro.exports.v1\n";
  out += std::to_string(valid.size()) + "\n";
  for (const MacroExport *entry : valid) {
    const lexer::Span &span = entry->def->span;
    const std::size_t begin =
        std::min<std::size_t>(span.start.offset, module_source.size());
    const std::size_t end = std::min<std::size_t>(
        std::max<std::size_t>(span.end.offset, begin), module_source.size());
    const std::string slice = module_source.substr(begin, end - begin);
    out += entry->public_name + "\t" +
           (is_string_tag_def(*entry->def) ? "string_tag" : "call") + "\t" +
           std::to_string(span.start.line) + "\t" +
           std::to_string(slice.size()) + "\n";
    out += slice;
    out += "\n";
  }
  return out;
}

namespace {

// Shift every span in the tree down by `delta` lines, so a definition
// re-parsed from its artifact source slice reports diagnostics at its
// original provider-source lines.
void shift_expr_lines(ast::Expr &node, int delta) {
  if (node.span.start.line > 0) {
    node.span.start.line += delta;
  }
  if (node.span.end.line > 0) {
    node.span.end.line += delta;
  }
  for (ast::NodeField &field : node.node_fields) {
    if (field.value) {
      shift_expr_lines(*field.value, delta);
    }
  }
  for (ast::ListField &field : node.list_fields) {
    for (std::unique_ptr<ast::Expr> &child : field.values) {
      if (child) {
        shift_expr_lines(*child, delta);
      }
    }
  }
}

// One header line of the macro-section payload: `name \t kind \t line \t len`.
bool read_payload_line(const std::string &payload, std::size_t *cursor,
                       std::string *line) {
  if (*cursor >= payload.size()) {
    return false;
  }
  const std::size_t newline = payload.find('\n', *cursor);
  if (newline == std::string::npos) {
    return false;
  }
  *line = payload.substr(*cursor, newline - *cursor);
  *cursor = newline + 1U;
  return true;
}

std::vector<std::string> split_fields(const std::string &line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1U;
  }
  return fields;
}

bool parse_payload_number(const std::string &text, std::size_t *out) {
  if (text.empty()) {
    return false;
  }
  std::size_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10U + static_cast<std::size_t>(c - '0');
  }
  *out = value;
  return true;
}

} // namespace

std::vector<MacroExport> parse_macro_exports(const std::string &payload,
                                             const std::string &provider_path,
                                             std::string *error) {
  const auto malformed = [&](const std::string &detail) {
    if (error != nullptr) {
      *error = "artifact macro section is malformed: " + detail;
    }
    return std::vector<MacroExport>{};
  };
  std::size_t cursor = 0;
  std::string line;
  if (!read_payload_line(payload, &cursor, &line) ||
      line != "amber.macro.exports.v1") {
    return malformed("unknown schema");
  }
  std::size_t count = 0;
  if (!read_payload_line(payload, &cursor, &line) ||
      !parse_payload_number(line, &count)) {
    return malformed("missing export count");
  }
  std::vector<MacroExport> exports;
  exports.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (!read_payload_line(payload, &cursor, &line)) {
      return malformed("truncated export header");
    }
    const std::vector<std::string> fields = split_fields(line);
    std::size_t start_line = 0;
    std::size_t length = 0;
    if (fields.size() != 4 || fields[0].empty() ||
        (fields[1] != "call" && fields[1] != "string_tag") ||
        !parse_payload_number(fields[2], &start_line) ||
        !parse_payload_number(fields[3], &length)) {
      return malformed("invalid export header");
    }
    if (cursor + length > payload.size()) {
      return malformed("truncated definition source");
    }
    const std::string snippet = payload.substr(cursor, length);
    cursor += length;
    if (cursor >= payload.size() || payload[cursor] != '\n') {
      return malformed("unterminated definition source");
    }
    ++cursor;

    // The parser is the deserializer: re-parse the persisted `macro def`
    // exactly as the source harvest would have seen it.
    lexer::Lexer snippet_lexer(snippet, provider_path);
    lexer::LexResult lex = snippet_lexer.lex();
    if (!lex.ok()) {
      return malformed("definition for `" + fields[0] +
                       "` no longer lexes");
    }
    parser::Parser snippet_parser(lex.tokens);
    parser::ParseModuleResult mod = snippet_parser.parse_module_unit();
    if (!mod.ok()) {
      return malformed("definition for `" + fields[0] +
                       "` no longer parses");
    }
    std::unique_ptr<ast::Expr> def;
    for (std::unique_ptr<ast::Expr> &item : mod.items) {
      if (item && is_macro_def(*item)) {
        if (def != nullptr) {
          return malformed("definition for `" + fields[0] +
                           "` contains more than one macro def");
        }
        def = std::move(item);
      }
    }
    if (def == nullptr) {
      return malformed("definition for `" + fields[0] +
                       "` is not a macro def");
    }
    const bool is_tag = is_string_tag_def(*def);
    if (is_tag != (fields[1] == "string_tag")) {
      return malformed("definition for `" + fields[0] +
                       "` does not match its declared surface kind");
    }
    if (start_line > 0) {
      shift_expr_lines(*def, static_cast<int>(start_line) - 1);
    }
    exports.push_back(
        MacroExport{fields[0], std::shared_ptr<const ast::Expr>(
                                   std::move(def))});
  }
  if (cursor != payload.size()) {
    return malformed("trailing bytes after the export table");
  }
  return exports;
}

ExpandResult expand_macros(std::vector<std::unique_ptr<ast::Expr>> &items,
                           const std::string &module_name,
                           const std::string &source) {
  static const MacroProviderMap kNoProviders;
  return expand_macros(items, module_name, source, kNoProviders);
}

ExpandResult expand_macros(std::vector<std::unique_ptr<ast::Expr>> &items,
                           const std::string &module_name,
                           const std::string &source,
                           const MacroProviderMap &providers) {
  std::vector<const ast::Expr *> macro_defs;
  MacroNameTable macro_names;
  std::map<std::string, int> annotation_arity;
  // string_tag macros are invocable only via `name"""…"""` and never
  // annotation-shaped, whatever their declared arity.
  std::set<std::string> string_tag_names;
  const auto register_macro = [&](const std::string &spelling,
                                  const std::string &selector,
                                  const ast::Expr &def) {
    macro_names[spelling] = selector;
    annotation_arity[spelling] =
        is_string_tag_def(def) ? -1 : annotation_arity_of(def);
    if (is_string_tag_def(def)) {
      string_tag_names.insert(spelling);
    }
  };
  for (const std::unique_ptr<ast::Expr> &item : items) {
    if (item && is_macro_def(*item)) {
      macro_defs.push_back(item.get());
      if (const std::string *name = string_field(*item, "name")) {
        register_macro(*name, *name, *item);
      }
    }
  }
  // Imported macros (cloned under their local aliases) join the compile-time
  // namespace next to local defs; `imported` owns them for this call.
  const std::vector<std::unique_ptr<ast::Expr>> imported =
      imported_macro_defs(items, providers);
  for (const std::unique_ptr<ast::Expr> &def : imported) {
    const std::string *name = string_field(*def, "name");
    if (name == nullptr) {
      continue;
    }
    if (macro_names.find(*name) != macro_names.end()) {
      return ExpandResult{false,
                          "macro import `" + *name +
                              "` collides with a macro definition of the same "
                              "name in this module"};
    }
    macro_defs.push_back(def.get());
    register_macro(*name, *name, *def);
  }
  // Module-alias imports (§11): `import m as pg` binds every macro export of
  // a staged provider under the dotted spelling `pg.name`. The cloned
  // definition compiles under a mangled selector (dotted spellings are not
  // identifiers); `alias_imported` owns the clones for this call.
  std::vector<std::unique_ptr<ast::Expr>> alias_imported;
  std::set<std::string> macro_bound_aliases;
  for (const std::unique_ptr<ast::Expr> &item : items) {
    if (!item || item->kind != "AstImportStmt") {
      continue;
    }
    const std::string *import_kind = string_field(*item, "import_kind");
    const std::string *module_path = string_field(*item, "module_path");
    const std::string *alias = string_field(*item, "alias");
    if (import_kind == nullptr || *import_kind != "module" ||
        module_path == nullptr || alias == nullptr || alias->empty()) {
      continue;
    }
    const auto provider = providers.find(*module_path);
    if (provider == providers.end()) {
      continue;
    }
    if (!provider->second.empty()) {
      macro_bound_aliases.insert(*alias);
    }
    for (const MacroExport &entry : provider->second) {
      if (entry.def == nullptr) {
        continue;
      }
      const std::string spelling = *alias + "." + entry.public_name;
      if (macro_names.find(spelling) != macro_names.end()) {
        return ExpandResult{false,
                            "macro import `" + spelling +
                                "` is bound more than once through "
                                "module-alias imports in this module"};
      }
      const std::string selector =
          "__macro_alias__" + *alias + "__" + entry.public_name;
      std::unique_ptr<ast::Expr> def = ast::clone_expr(*entry.def);
      for (ast::StringField &field : def->string_fields) {
        if (field.name == "name") {
          field.value = selector;
        }
      }
      register_macro(spelling, selector, *def);
      macro_defs.push_back(def.get());
      alias_imported.push_back(std::move(def));
    }
  }
  if (macro_defs.empty()) {
    return ExpandResult{};
  }

  bytecode::BcModule macro_module;
  std::string error;
  if (!compile_macro_module(macro_defs, module_name + ".macros", &macro_module,
                            &error)) {
    return ExpandResult{false, error};
  }

  Expander expander(std::move(macro_module), std::move(macro_names),
                    std::move(annotation_arity), std::move(string_tag_names),
                    std::make_shared<const std::string>(source));
  // The module item list is itself a statement/declaration context, so
  // annotation runs and multi-node expansions apply at top level.
  expander.expand_statements(items, 0);
  const ExpandResult result = expander.result();
  if (!result.ok) {
    return result;
  }

  // Macro definitions are compile-time only; drop them from the runtime
  // module, together with the `export macro` items that published them (the
  // compile-time export table was already harvested; the runtime module must
  // not export names it no longer defines). A macro-marked export with no
  // matching definition is left in place so the binder reports it.
  std::set<std::string> local_macro_names;
  for (const std::unique_ptr<ast::Expr> &item : items) {
    if (item && is_macro_def(*item)) {
      if (const std::string *name = string_field(*item, "name")) {
        local_macro_names.insert(*name);
      }
    }
  }
  // Consumed macro imports are compile-time bindings, not runtime import
  // cells: strip every from-import name that matched a provider macro export
  // (same criteria as imported_macro_defs), so the runtime module does not
  // import a name the provider no longer exports.
  for (std::unique_ptr<ast::Expr> &item : items) {
    if (!item || item->kind != "AstImportStmt") {
      continue;
    }
    const std::string *import_kind = string_field(*item, "import_kind");
    const std::string *module_path = string_field(*item, "module_path");
    if (import_kind == nullptr || *import_kind != "from" ||
        module_path == nullptr) {
      continue;
    }
    const auto provider = providers.find(*module_path);
    if (provider == providers.end()) {
      continue;
    }
    ast::ListField *names = mutable_list_field(*item, "names");
    if (names == nullptr) {
      continue;
    }
    names->values.erase(
        std::remove_if(names->values.begin(), names->values.end(),
                       [&](const std::unique_ptr<ast::Expr> &name) {
                         if (!name || name->kind != "AstImportName") {
                           return false;
                         }
                         const std::string *source_name =
                             string_field(*name, "source_name");
                         if (source_name == nullptr) {
                           return false;
                         }
                         for (const MacroExport &entry : provider->second) {
                           if (entry.public_name == *source_name) {
                             return true;
                           }
                         }
                         return false;
                       }),
        names->values.end());
  }
  // A macro-providing module alias whose name is no longer referenced after
  // expansion was a compile-time-only import: every dotted use was a macro
  // trigger, so the runtime module keeps no alias binding (and stays eligible
  // for build lanes that do not support runtime module aliases). An alias
  // with surviving references is a runtime import as before.
  for (std::unique_ptr<ast::Expr> &item : items) {
    if (!item || item->kind != "AstImportStmt") {
      continue;
    }
    const std::string *import_kind = string_field(*item, "import_kind");
    const std::string *alias = string_field(*item, "alias");
    if (import_kind == nullptr || *import_kind != "module" ||
        alias == nullptr || macro_bound_aliases.count(*alias) == 0) {
      continue;
    }
    bool referenced = false;
    for (const std::unique_ptr<ast::Expr> &other : items) {
      if (other && other.get() != item.get() &&
          tree_references_name(*other, *alias)) {
        referenced = true;
        break;
      }
    }
    if (!referenced) {
      item.reset();
    }
  }
  items.erase(std::remove_if(items.begin(), items.end(),
                             [](const std::unique_ptr<ast::Expr> &item) {
                               return item == nullptr;
                             }),
              items.end());
  for (std::unique_ptr<ast::Expr> &item : items) {
    if (!item || item->kind != "AstExportStmt") {
      continue;
    }
    ast::ListField *export_items = mutable_list_field(*item, "items");
    if (export_items == nullptr) {
      continue;
    }
    export_items->values.erase(
        std::remove_if(
            export_items->values.begin(), export_items->values.end(),
            [&](const std::unique_ptr<ast::Expr> &entry) {
              if (!entry || entry->kind != "AstExportItem") {
                return false;
              }
              bool is_macro = false;
              for (const ast::BoolField &field : entry->bool_fields) {
                if (field.name == "is_macro") {
                  is_macro = field.value;
                }
              }
              const std::string *local = string_field(*entry, "local_name");
              return is_macro && local != nullptr &&
                     local_macro_names.count(*local) != 0;
            }),
        export_items->values.end());
  }
  items.erase(std::remove_if(items.begin(), items.end(),
                             [](const std::unique_ptr<ast::Expr> &item) {
                               if (!item) {
                                 return false;
                               }
                               if (is_macro_def(*item)) {
                                 return true;
                               }
                               // Export/from-import statements emptied by the
                               // strips above carry nothing for the runtime.
                               if (item->kind == "AstExportStmt") {
                                 const ast::ListField *entries =
                                     list_field(*item, "items");
                                 return entries != nullptr &&
                                        entries->values.empty();
                               }
                               if (item->kind == "AstImportStmt") {
                                 const std::string *kind =
                                     string_field(*item, "import_kind");
                                 const ast::ListField *names =
                                     list_field(*item, "names");
                                 return kind != nullptr && *kind == "from" &&
                                        names != nullptr &&
                                        names->values.empty();
                               }
                               return false;
                             }),
              items.end());
  return ExpandResult{};
}

} // namespace amber::macros
