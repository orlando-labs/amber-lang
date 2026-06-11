#include "optimizer/mir.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace amber::mir {

namespace {

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

std::string string_field(const ast::Expr &expr, const std::string &name) {
  for (const ast::StringField &field : expr.string_fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  return "";
}

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (unsigned char ch : value) {
    switch (ch) {
    case '\\':
      out << "\\\\";
      break;
    case '"':
      out << "\\\"";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (ch < 0x20) {
        out << "\\u00";
        const char *hex = "0123456789abcdef";
        out << hex[(ch >> 4U) & 0x0fU] << hex[ch & 0x0fU];
      } else {
        out << static_cast<char>(ch);
      }
      break;
    }
  }
  return out.str();
}

void append_position_json(std::ostringstream &out,
                          const lexer::Position &position) {
  out << "{\"line\":" << position.line << ",\"col\":" << position.col
      << ",\"offset\":" << position.offset << "}";
}

void append_span_json(std::ostringstream &out, const lexer::Span &span) {
  out << "{\"file\":\"" << json_escape(span.file) << "\",\"start\":";
  append_position_json(out, span.start);
  out << ",\"end\":";
  append_position_json(out, span.end);
  out << "}";
}

Attribute attr(std::string key, std::string value) {
  return {std::move(key), std::move(value)};
}

bool is_value_name(const std::string &value) {
  return value.size() > 2U && value[0] == '%' && value[1] == 'v';
}

bool block_has_id(const Function &function, const std::string &id) {
  for (const Block &block : function.blocks) {
    if (block.id == id) {
      return true;
    }
  }
  return false;
}

bool local_has_slot(const Function &function, const std::string &slot) {
  for (const Local &local : function.locals) {
    if (local.slot == slot) {
      return true;
    }
  }
  return false;
}

bool capture_has_slot(const Function &function, const std::string &slot) {
  for (const Capture &capture : function.captures) {
    if (capture.slot == slot) {
      return true;
    }
  }
  return false;
}

class Lowerer {
public:
  Module lower(const hir::Program &program, const std::string &module_name) {
    module_.module_name = module_name;
    for (const hir::Procedure &procedure : program.procedures) {
      lower_function(procedure);
    }
    return std::move(module_);
  }

private:
  struct LoopContext {
    std::string exit_block;
  };

  Module module_;
  Function *function_ = nullptr;
  std::size_t current_block_ = 0;
  std::uint32_t next_value_ = 0;
  std::uint32_t next_block_ = 0;
  std::vector<LoopContext> loops_;

  void lower_function(const hir::Procedure &procedure) {
    module_.functions.push_back({});
    function_ = &module_.functions.back();
    current_block_ = 0;
    next_value_ = 0;
    next_block_ = 0;
    loops_.clear();

    function_->id = procedure.id;
    function_->name = procedure.name;
    function_->kind = procedure.kind;
    function_->owner = procedure.owner;
    for (const hir::ProcedureLocal &local : procedure.locals) {
      function_->locals.push_back(
          {local.slot, local.name, local.role, local.binding_kind});
    }
    for (const hir::ProcedureCapture &capture : procedure.captures) {
      function_->captures.push_back({capture.slot, capture.name,
                                     capture.source_kind, capture.source_slot,
                                     capture.source_name});
    }

    const std::size_t entry = create_block();
    function_->entry_block = block(entry).id;
    current_block_ = entry;

    if (procedure.body != nullptr && procedure.body->kind == "HSeq") {
      compile_seq(*procedure.body);
    } else if (procedure.body != nullptr) {
      compile_stmt(*procedure.body);
    }

    if (!block(current_block_).has_terminator) {
      const std::string result = emit_value("last.get", {}, {}, procedure.span);
      terminate("return", {value_operand(result)}, {}, procedure.span);
    }
  }

  std::size_t create_block() {
    Block block;
    block.id = "bb" + std::to_string(next_block_++);
    function_->blocks.push_back(std::move(block));
    return function_->blocks.size() - 1U;
  }

  Block &block(std::size_t index) { return function_->blocks[index]; }

  void ensure_open_block(const lexer::Span &span) {
    if (!block(current_block_).has_terminator) {
      return;
    }
    current_block_ = create_block();
    emit_void("unreachable.resume", {}, {}, span);
  }

  std::string next_value() { return "%v" + std::to_string(next_value_++); }

  std::string emit_value(const std::string &op, std::vector<Operand> operands,
                         std::vector<Attribute> attrs,
                         const lexer::Span &span) {
    ensure_open_block(span);
    Instruction instruction;
    instruction.result = next_value();
    instruction.op = op;
    instruction.operands = std::move(operands);
    instruction.attrs = std::move(attrs);
    instruction.span = span;
    const std::string result = instruction.result;
    block(current_block_).instructions.push_back(std::move(instruction));
    return result;
  }

  void emit_void(const std::string &op, std::vector<Operand> operands,
                 std::vector<Attribute> attrs, const lexer::Span &span) {
    ensure_open_block(span);
    Instruction instruction;
    instruction.op = op;
    instruction.operands = std::move(operands);
    instruction.attrs = std::move(attrs);
    instruction.span = span;
    block(current_block_).instructions.push_back(std::move(instruction));
  }

  void terminate(const std::string &op, std::vector<Operand> operands,
                 std::vector<std::string> targets, const lexer::Span &span) {
    Terminator terminator;
    terminator.op = op;
    terminator.operands = std::move(operands);
    terminator.targets = std::move(targets);
    terminator.span = span;
    block(current_block_).terminator = std::move(terminator);
    block(current_block_).has_terminator = true;
  }

  void compile_seq(const ast::Expr &seq) {
    const ast::ListField *items = list_field(seq, "items");
    if (items == nullptr) {
      return;
    }
    for (const std::unique_ptr<ast::Expr> &item : items->values) {
      compile_stmt(*item);
    }
  }

  void compile_stmt(const ast::Expr &stmt) {
    if (stmt.kind == "HSeq") {
      compile_seq(stmt);
      return;
    }
    if (stmt.kind == "HLastSet") {
      const ast::Expr *expr = node_field(stmt, "expr");
      if (expr == nullptr) {
        return;
      }
      const std::string value = compile_expr(*expr);
      emit_void("last.set", {value_operand(value)}, {}, stmt.span);
      return;
    }
    if (stmt.kind == "HMethod" || stmt.kind == "HClass" ||
        stmt.kind == "HMixin" || stmt.kind == "HInclude" ||
        stmt.kind == "HExtend") {
      emit_void("declare." + stmt.kind.substr(1), {},
                {attr("name", string_field(stmt, "name"))}, stmt.span);
      return;
    }
    const std::string value = compile_expr(stmt);
    emit_void("last.set", {value_operand(value)}, {}, stmt.span);
  }

  std::string compile_expr(const ast::Expr &expr) {
    if (expr.kind == "HConst") {
      return emit_value("const", {},
                        {attr("token", string_field(expr, "token")),
                         attr("value", string_field(expr, "value"))},
                        expr.span);
    }
    if (expr.kind == "HLoadLocal") {
      return emit_value("local.load",
                        {local_operand(string_field(expr, "slot"))}, {},
                        expr.span);
    }
    if (expr.kind == "HStoreLocal") {
      const ast::Expr *value = node_field(expr, "expr");
      const std::string compiled = value == nullptr
                                       ? emit_value("undef", {}, {}, expr.span)
                                       : compile_expr(*value);
      emit_void(
          "local.store",
          {local_operand(string_field(expr, "slot")), value_operand(compiled)},
          {}, expr.span);
      return compiled;
    }
    if (expr.kind == "HLastGet") {
      return emit_value("last.get", {}, {}, expr.span);
    }
    if (expr.kind == "HLoadCapture") {
      return emit_value("capture.load",
                        {capture_operand(string_field(expr, "slot"))}, {},
                        expr.span);
    }
    if (expr.kind == "HStoreCapture") {
      const ast::Expr *value = node_field(expr, "expr");
      const std::string compiled = value == nullptr
                                       ? emit_value("undef", {}, {}, expr.span)
                                       : compile_expr(*value);
      emit_void("capture.store",
                {capture_operand(string_field(expr, "slot")),
                 value_operand(compiled)},
                {}, expr.span);
      return compiled;
    }
    if (expr.kind == "HLoadName") {
      return emit_value("name.lookup", {},
                        {attr("name", string_field(expr, "name"))}, expr.span);
    }
    if (expr.kind == "HLoadConst") {
      return emit_value("const.lookup", {},
                        {attr("path", string_field(expr, "path"))}, expr.span);
    }
    if (expr.kind == "HLoadIvar" || expr.kind == "HLoadCvar") {
      const std::string self = emit_value("self", {}, {}, expr.span);
      return emit_value(
          expr.kind == "HLoadIvar" ? "ivar.load" : "cvar.load",
          {value_operand(self), symbol_operand(string_field(expr, "name"))}, {},
          expr.span);
    }
    if (expr.kind == "HStoreIvar" || expr.kind == "HStoreCvar") {
      const std::string self = emit_value("self", {}, {}, expr.span);
      const ast::Expr *value = node_field(expr, "expr");
      const std::string compiled = value == nullptr
                                       ? emit_value("undef", {}, {}, expr.span)
                                       : compile_expr(*value);
      emit_void(expr.kind == "HStoreIvar" ? "ivar.store" : "cvar.store",
                {value_operand(self),
                 symbol_operand(string_field(expr, "name")),
                 value_operand(compiled)},
                {}, expr.span);
      return compiled;
    }
    if (expr.kind == "HSend") {
      return compile_send(expr, "send");
    }
    if (expr.kind == "HSendDyn") {
      return compile_send(expr, "send.dynamic");
    }
    if (expr.kind == "HCall") {
      return compile_send(expr, "call");
    }
    if (expr.kind == "HIndex") {
      const ast::Expr *receiver = node_field(expr, "receiver");
      const ast::Expr *index_expr = node_field(expr, "index_expr");
      const std::string receiver_value =
          receiver == nullptr ? emit_value("undef", {}, {}, expr.span)
                              : compile_expr(*receiver);
      const std::string index_value =
          index_expr == nullptr ? emit_value("undef", {}, {}, expr.span)
                                : compile_expr(*index_expr);
      return emit_value(
          "send",
          {value_operand(receiver_value), symbol_operand("[]"),
           value_operand(index_value)},
          {attr("selector", "[]"), attr("pos_args", "1"), attr("kw_args", "0")},
          expr.span);
    }
    if (expr.kind == "HKeywordArg") {
      const ast::Expr *value = node_field(expr, "value");
      const std::string compiled = value == nullptr
                                       ? emit_value("undef", {}, {}, expr.span)
                                       : compile_expr(*value);
      return emit_value("keyword.arg", {value_operand(compiled)},
                        {attr("name", string_field(expr, "name"))}, expr.span);
    }
    if (expr.kind == "HClosure") {
      std::vector<Operand> operands{
          procedure_operand(string_field(expr, "procedure"))};
      if (const ast::ListField *captures = list_field(expr, "captures")) {
        for (const std::unique_ptr<ast::Expr> &capture : captures->values) {
          const std::string source_kind = string_field(*capture, "source_kind");
          const std::string source_slot = string_field(*capture, "source_slot");
          operands.push_back(source_kind == "capture"
                                 ? capture_operand(source_slot)
                                 : local_operand(source_slot));
        }
      }
      return emit_value("closure.make", std::move(operands), {}, expr.span);
    }
    if (expr.kind == "HIf") {
      return compile_if(expr);
    }
    if (expr.kind == "HLoop") {
      return compile_loop(expr);
    }
    if (expr.kind == "HMatchDispatch") {
      const ast::Expr *scrutinee = node_field(expr, "scrutinee");
      const std::string value = scrutinee == nullptr
                                    ? emit_value("undef", {}, {}, expr.span)
                                    : compile_expr(*scrutinee);
      const ast::ListField *arms = list_field(expr, "arms");
      return emit_value(
          "match.dispatch", {value_operand(value)},
          {attr("arms",
                std::to_string(arms == nullptr ? 0U : arms->values.size())),
           attr("fail_mode", string_field(expr, "fail_mode"))},
          expr.span);
    }
    if (expr.kind == "HPatternAssign") {
      const ast::Expr *value = node_field(expr, "value");
      const std::string compiled = value == nullptr
                                       ? emit_value("undef", {}, {}, expr.span)
                                       : compile_expr(*value);
      return emit_value("pattern.assign", {value_operand(compiled)},
                        {attr("fail_mode", string_field(expr, "fail_mode"))},
                        expr.span);
    }
    if (expr.kind == "HBreak") {
      if (const ast::Expr *value = node_field(expr, "value")) {
        const std::string compiled = compile_expr(*value);
        emit_void("last.set", {value_operand(compiled)}, {}, expr.span);
      }
      if (!loops_.empty()) {
        terminate("jump", {}, {loops_.back().exit_block}, expr.span);
      }
      return emit_value("undef", {}, {}, expr.span);
    }
    if (expr.kind == "HReturn") {
      const ast::Expr *value = node_field(expr, "value");
      const std::string compiled = value == nullptr
                                       ? emit_value("undef", {}, {}, expr.span)
                                       : compile_expr(*value);
      terminate("return", {value_operand(compiled)}, {}, expr.span);
      return emit_value("undef", {}, {}, expr.span);
    }
    if (expr.kind == "HIsNull") {
      const ast::Expr *inner = node_field(expr, "expr");
      const std::string value = inner == nullptr
                                    ? emit_value("undef", {}, {}, expr.span)
                                    : compile_expr(*inner);
      return emit_value("is_null", {value_operand(value)}, {}, expr.span);
    }
    return emit_value("unsupported", {}, {attr("hir_kind", expr.kind)},
                      expr.span);
  }

  std::string compile_send(const ast::Expr &expr, const std::string &op) {
    std::vector<Operand> operands;
    std::vector<Attribute> attrs;

    if (op == "send") {
      const ast::Expr *receiver = node_field(expr, "receiver");
      operands.push_back(value_operand(
          receiver == nullptr ? emit_value("undef", {}, {}, expr.span)
                              : compile_expr(*receiver)));
      operands.push_back(symbol_operand(string_field(expr, "selector")));
      attrs.push_back(attr("selector", string_field(expr, "selector")));
    } else if (op == "send.dynamic") {
      const ast::Expr *receiver = node_field(expr, "receiver");
      const ast::Expr *selector = node_field(expr, "selector_expr");
      operands.push_back(value_operand(
          receiver == nullptr ? emit_value("undef", {}, {}, expr.span)
                              : compile_expr(*receiver)));
      operands.push_back(value_operand(
          selector == nullptr ? emit_value("undef", {}, {}, expr.span)
                              : compile_expr(*selector)));
    } else {
      const ast::Expr *callable = node_field(expr, "callable");
      operands.push_back(value_operand(
          callable == nullptr ? emit_value("undef", {}, {}, expr.span)
                              : compile_expr(*callable)));
    }

    std::size_t pos_count = 0;
    if (const ast::ListField *pos_args = list_field(expr, "pos_args")) {
      for (const std::unique_ptr<ast::Expr> &arg : pos_args->values) {
        operands.push_back(value_operand(compile_expr(*arg)));
        ++pos_count;
      }
    }
    std::size_t kw_count = 0;
    if (const ast::ListField *kw_args = list_field(expr, "kw_args")) {
      for (const std::unique_ptr<ast::Expr> &arg : kw_args->values) {
        const ast::Expr *value = node_field(*arg, "value");
        operands.push_back(symbol_operand(string_field(*arg, "name")));
        operands.push_back(value_operand(
            value == nullptr ? emit_value("undef", {}, {}, arg->span)
                             : compile_expr(*value)));
        ++kw_count;
      }
    }
    if (const ast::Expr *block = node_field(expr, "block")) {
      operands.push_back(value_operand(compile_expr(*block)));
      attrs.push_back(attr("has_block", "true"));
    }
    attrs.push_back(attr("pos_args", std::to_string(pos_count)));
    attrs.push_back(attr("kw_args", std::to_string(kw_count)));
    return emit_value(op, std::move(operands), std::move(attrs), expr.span);
  }

  std::string compile_if(const ast::Expr &expr) {
    const ast::Expr *cond = node_field(expr, "cond");
    const ast::Expr *then_body = node_field(expr, "then_body");
    const ast::Expr *else_body = node_field(expr, "else_body");
    const std::string cond_value = cond == nullptr
                                       ? emit_value("undef", {}, {}, expr.span)
                                       : compile_expr(*cond);

    const std::size_t then_block = create_block();
    const std::size_t else_block = create_block();
    const std::size_t merge_block = create_block();
    const std::string then_id = block(then_block).id;
    const std::string else_id = block(else_block).id;
    const std::string merge_id = block(merge_block).id;
    terminate("branch_if", {value_operand(cond_value)}, {then_id, else_id},
              expr.span);

    std::vector<Operand> phi_operands;
    current_block_ = then_block;
    if (then_body != nullptr) {
      compile_seq(*then_body);
    }
    if (!block(current_block_).has_terminator) {
      const std::string result = emit_value("last.get", {}, {}, expr.span);
      phi_operands.push_back(block_operand(block(current_block_).id));
      phi_operands.push_back(value_operand(result));
      terminate("jump", {}, {merge_id}, expr.span);
    }

    current_block_ = else_block;
    if (else_body != nullptr) {
      compile_seq(*else_body);
    }
    if (!block(current_block_).has_terminator) {
      const std::string result = emit_value("last.get", {}, {}, expr.span);
      phi_operands.push_back(block_operand(block(current_block_).id));
      phi_operands.push_back(value_operand(result));
      terminate("jump", {}, {merge_id}, expr.span);
    }

    current_block_ = merge_block;
    if (phi_operands.empty()) {
      return emit_value("undef", {}, {}, expr.span);
    }
    if (phi_operands.size() == 2U) {
      return phi_operands[1].value;
    }
    return emit_value("phi", std::move(phi_operands), {}, expr.span);
  }

  std::string compile_loop(const ast::Expr &expr) {
    const std::string kind = string_field(expr, "kind");
    const ast::Expr *cond = node_field(expr, "cond");
    const ast::Expr *body = node_field(expr, "body");
    const std::size_t header_block = create_block();
    const std::size_t body_block = create_block();
    const std::size_t exit_block = create_block();
    const std::string header_id = block(header_block).id;
    const std::string body_id = block(body_block).id;
    const std::string exit_id = block(exit_block).id;

    terminate("jump", {}, {header_id}, expr.span);

    current_block_ = header_block;
    emit_void("safepoint", {}, {}, expr.span);
    if (kind == "while" || kind == "until") {
      const std::string cond_value =
          cond == nullptr ? emit_value("undef", {}, {}, expr.span)
                          : compile_expr(*cond);
      if (kind == "while") {
        terminate("branch_if", {value_operand(cond_value)}, {body_id, exit_id},
                  expr.span);
      } else {
        terminate("branch_if", {value_operand(cond_value)}, {exit_id, body_id},
                  expr.span);
      }
    } else if (kind == "loop" || kind == "do_while") {
      terminate("jump", {}, {body_id}, expr.span);
    }

    current_block_ = body_block;
    loops_.push_back({exit_id});
    if (body != nullptr) {
      compile_seq(*body);
    }
    if (!block(current_block_).has_terminator) {
      if (kind == "do_while") {
        const std::string cond_value =
            cond == nullptr ? emit_value("undef", {}, {}, expr.span)
                            : compile_expr(*cond);
        terminate("branch_if", {value_operand(cond_value)},
                  {header_id, exit_id}, expr.span);
      } else {
        terminate("jump", {}, {header_id}, expr.span);
      }
    }
    loops_.pop_back();

    current_block_ = exit_block;
    return emit_value("last.get", {}, {}, expr.span);
  }
};

void append_operand_json(std::ostringstream &out, const Operand &operand) {
  out << "{\"kind\":\"" << json_escape(operand.kind) << "\",\"value\":\""
      << json_escape(operand.value) << "\"}";
}

void append_attrs_json(std::ostringstream &out,
                       const std::vector<Attribute> &attrs) {
  out << "[";
  for (std::size_t i = 0; i < attrs.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "{\"key\":\"" << json_escape(attrs[i].key) << "\",\"value\":\""
        << json_escape(attrs[i].value) << "\"}";
  }
  out << "]";
}

void append_instruction_json(std::ostringstream &out,
                             const Instruction &instruction) {
  out << "{\"op\":\"" << json_escape(instruction.op) << "\",\"result\":";
  if (instruction.result.empty()) {
    out << "null";
  } else {
    out << "\"" << json_escape(instruction.result) << "\"";
  }
  out << ",\"operands\":[";
  for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    append_operand_json(out, instruction.operands[i]);
  }
  out << "],\"attrs\":";
  append_attrs_json(out, instruction.attrs);
  out << ",\"span\":";
  append_span_json(out, instruction.span);
  out << "}";
}

void append_terminator_json(std::ostringstream &out,
                            const Terminator &terminator) {
  out << "{\"op\":\"" << json_escape(terminator.op) << "\",\"operands\":[";
  for (std::size_t i = 0; i < terminator.operands.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    append_operand_json(out, terminator.operands[i]);
  }
  out << "],\"targets\":[";
  for (std::size_t i = 0; i < terminator.targets.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\"" << json_escape(terminator.targets[i]) << "\"";
  }
  out << "],\"span\":";
  append_span_json(out, terminator.span);
  out << "}";
}

std::string attrs_to_dump(const std::vector<Attribute> &attrs) {
  std::ostringstream out;
  for (const Attribute &attribute : attrs) {
    out << " " << attribute.key << "=\"" << json_escape(attribute.value)
        << "\"";
  }
  return out.str();
}

std::string operand_to_dump(const Operand &operand) {
  if (operand.kind == "value") {
    return operand.value;
  }
  if (operand.kind == "local") {
    return "local(" + operand.value + ")";
  }
  if (operand.kind == "capture") {
    return "capture(" + operand.value + ")";
  }
  if (operand.kind == "symbol") {
    return ":" + operand.value;
  }
  if (operand.kind == "block") {
    return operand.value;
  }
  if (operand.kind == "procedure") {
    return "@" + operand.value;
  }
  if (operand.kind == "const") {
    return "const(" + operand.value + ")";
  }
  return "\"" + json_escape(operand.value) + "\"";
}

void append_error(std::vector<ValidationError> &errors, const std::string &code,
                  const std::string &message, const std::string &function_id,
                  const std::string &block_id = "") {
  errors.push_back({code, message, function_id, block_id});
}

void validate_value_operand(const Operand &operand,
                            const std::set<std::string> &defs,
                            std::vector<ValidationError> &errors,
                            const Function &function, const Block &block) {
  if (operand.kind == "value" && defs.find(operand.value) == defs.end()) {
    append_error(errors, "MIR1005", "value operand has no SSA definition",
                 function.id, block.id);
  }
}

} // namespace

Operand value_operand(std::string value) { return {"value", std::move(value)}; }

Operand local_operand(std::string slot) { return {"local", std::move(slot)}; }

Operand capture_operand(std::string slot) {
  return {"capture", std::move(slot)};
}

Operand const_operand(std::string value) { return {"const", std::move(value)}; }

Operand symbol_operand(std::string value) {
  return {"symbol", std::move(value)};
}

Operand block_operand(std::string value) { return {"block", std::move(value)}; }

Operand procedure_operand(std::string value) {
  return {"procedure", std::move(value)};
}

Operand text_operand(std::string value) { return {"text", std::move(value)}; }

Module lower_program(const hir::Program &program,
                     const std::string &module_name) {
  Lowerer lowerer;
  return lowerer.lower(program, module_name);
}

ValidationResult validate_module(const Module &module) {
  ValidationResult result;
  for (const Function &function : module.functions) {
    if (function.entry_block.empty() ||
        !block_has_id(function, function.entry_block)) {
      append_error(result.errors, "MIR1001", "function entry block is missing",
                   function.id);
    }

    std::set<std::string> block_ids;
    for (const Block &block : function.blocks) {
      if (!block_ids.insert(block.id).second) {
        append_error(result.errors, "MIR1002", "duplicate block id",
                     function.id, block.id);
      }
    }

    std::set<std::string> defs;
    for (const Block &block : function.blocks) {
      bool saw_non_phi = false;
      for (const Instruction &instruction : block.instructions) {
        if (instruction.op == "phi" && saw_non_phi) {
          append_error(result.errors, "MIR1007",
                       "phi instruction must be at the top of a block",
                       function.id, block.id);
        }
        if (instruction.op != "phi") {
          saw_non_phi = true;
        }
        if (instruction.result.empty()) {
          continue;
        }
        if (!is_value_name(instruction.result)) {
          append_error(result.errors, "MIR1006",
                       "instruction result is not an SSA value", function.id,
                       block.id);
        }
        if (!defs.insert(instruction.result).second) {
          append_error(result.errors, "MIR1004", "duplicate SSA definition",
                       function.id, block.id);
        }
      }
      if (!block.has_terminator) {
        append_error(result.errors, "MIR1003", "block has no terminator",
                     function.id, block.id);
      }
    }

    for (const Block &block : function.blocks) {
      for (const Instruction &instruction : block.instructions) {
        if (instruction.op == "phi") {
          if (instruction.operands.empty() ||
              instruction.operands.size() % 2U != 0U) {
            append_error(result.errors, "MIR1008",
                         "phi requires block/value operand pairs", function.id,
                         block.id);
          }
          for (std::size_t i = 0; i + 1U < instruction.operands.size();
               i += 2U) {
            if (instruction.operands[i].kind != "block" ||
                instruction.operands[i + 1U].kind != "value") {
              append_error(result.errors, "MIR1008",
                           "phi operand pair must be block then value",
                           function.id, block.id);
              continue;
            }
            if (block_ids.find(instruction.operands[i].value) ==
                block_ids.end()) {
              append_error(result.errors, "MIR1010",
                           "phi references an unknown predecessor block",
                           function.id, block.id);
            }
            validate_value_operand(instruction.operands[i + 1U], defs,
                                   result.errors, function, block);
          }
        }
        for (const Operand &operand : instruction.operands) {
          validate_value_operand(operand, defs, result.errors, function, block);
          if (operand.kind == "local" &&
              !local_has_slot(function, operand.value)) {
            append_error(result.errors, "MIR1011",
                         "local operand references an unknown slot",
                         function.id, block.id);
          }
          if (operand.kind == "capture" &&
              !capture_has_slot(function, operand.value)) {
            append_error(result.errors, "MIR1012",
                         "capture operand references an unknown slot",
                         function.id, block.id);
          }
        }
      }

      if (!block.has_terminator) {
        continue;
      }
      const Terminator &terminator = block.terminator;
      for (const Operand &operand : terminator.operands) {
        validate_value_operand(operand, defs, result.errors, function, block);
      }
      for (const std::string &target : terminator.targets) {
        if (block_ids.find(target) == block_ids.end()) {
          append_error(result.errors, "MIR1010",
                       "terminator references an unknown block", function.id,
                       block.id);
        }
      }
      if (terminator.op == "return") {
        if (terminator.operands.size() > 1U || !terminator.targets.empty()) {
          append_error(result.errors, "MIR1009",
                       "return terminator has invalid shape", function.id,
                       block.id);
        }
      } else if (terminator.op == "jump") {
        if (!terminator.operands.empty() || terminator.targets.size() != 1U) {
          append_error(result.errors, "MIR1009",
                       "jump terminator has invalid shape", function.id,
                       block.id);
        }
      } else if (terminator.op == "branch_if") {
        if (terminator.operands.size() != 1U ||
            terminator.targets.size() != 2U) {
          append_error(result.errors, "MIR1009",
                       "branch_if terminator has invalid shape", function.id,
                       block.id);
        }
      } else {
        append_error(result.errors, "MIR1013", "unknown terminator op",
                     function.id, block.id);
      }
    }
  }
  return result;
}

PassPipelineResult run_pass_pipeline(Module &module,
                                     const std::vector<Pass> &passes) {
  PassPipelineResult result;
  bool have_phase = false;
  std::uint32_t last_phase = 0;
  for (const Pass &pass : passes) {
    if (pass.name.empty()) {
      append_error(result.errors, "MIR2000", "pass name is empty", "");
      return result;
    }
    if (have_phase && pass.phase_order < last_phase) {
      append_error(result.errors, "MIR2001",
                   "pass phase order is not monotonic", "");
      return result;
    }
    have_phase = true;
    last_phase = pass.phase_order;

    if ((pass.invalidates & kInvalidatesSsa) != 0U &&
        pass.preserves_valid_ssa) {
      append_error(result.errors, "MIR2002",
                   "pass cannot both invalidate and preserve SSA", "");
      return result;
    }
    if (pass.requires_valid_ssa) {
      ValidationResult before = validate_module(module);
      if (!before.ok()) {
        result.errors.insert(result.errors.end(), before.errors.begin(),
                             before.errors.end());
        return result;
      }
    }
    if (pass.run) {
      pass.run(module);
    }
    PassRecord record{pass.name, pass.phase_order, pass.invalidates};
    module.pass_log.push_back(record);
    result.records.push_back(record);
    if (pass.preserves_valid_ssa) {
      ValidationResult after = validate_module(module);
      if (!after.ok()) {
        result.errors.insert(result.errors.end(), after.errors.begin(),
                             after.errors.end());
        return result;
      }
    }
  }
  return result;
}

std::string module_to_json(const Module &module,
                           const std::string &source_hash) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"format\": \"amber.mir.v1\",\n";
  if (module.module_name.empty()) {
    out << "  \"module\": null,\n";
  } else {
    out << "  \"module\": \"" << json_escape(module.module_name) << "\",\n";
  }
  out << "  \"functions\": [\n";
  for (std::size_t fn_i = 0; fn_i < module.functions.size(); ++fn_i) {
    const Function &function = module.functions[fn_i];
    out << "    {\"id\":\"" << json_escape(function.id) << "\",\"name\":\""
        << json_escape(function.name) << "\",\"kind\":\""
        << json_escape(function.kind) << "\",\"owner\":\""
        << json_escape(function.owner) << "\",\"entry\":\""
        << json_escape(function.entry_block) << "\",\"locals\":[";
    for (std::size_t i = 0; i < function.locals.size(); ++i) {
      if (i != 0U) {
        out << ",";
      }
      const Local &local = function.locals[i];
      out << "{\"slot\":\"" << json_escape(local.slot) << "\",\"name\":\""
          << json_escape(local.name) << "\",\"role\":\""
          << json_escape(local.role) << "\",\"binding_kind\":\""
          << json_escape(local.binding_kind) << "\"}";
    }
    out << "],\"captures\":[";
    for (std::size_t i = 0; i < function.captures.size(); ++i) {
      if (i != 0U) {
        out << ",";
      }
      const Capture &capture = function.captures[i];
      out << "{\"slot\":\"" << json_escape(capture.slot) << "\",\"name\":\""
          << json_escape(capture.name) << "\",\"source_kind\":\""
          << json_escape(capture.source_kind) << "\",\"source_slot\":\""
          << json_escape(capture.source_slot) << "\",\"source_name\":\""
          << json_escape(capture.source_name) << "\"}";
    }
    out << "],\"blocks\":[";
    for (std::size_t block_i = 0; block_i < function.blocks.size(); ++block_i) {
      if (block_i != 0U) {
        out << ",";
      }
      const Block &block = function.blocks[block_i];
      out << "{\"id\":\"" << json_escape(block.id) << "\",\"instructions\":[";
      for (std::size_t ins_i = 0; ins_i < block.instructions.size(); ++ins_i) {
        if (ins_i != 0U) {
          out << ",";
        }
        append_instruction_json(out, block.instructions[ins_i]);
      }
      out << "],\"terminator\":";
      if (block.has_terminator) {
        append_terminator_json(out, block.terminator);
      } else {
        out << "null";
      }
      out << "}";
    }
    out << "]}";
    if (fn_i + 1U < module.functions.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"passes\": [";
  for (std::size_t i = 0; i < module.pass_log.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "{\"name\":\"" << json_escape(module.pass_log[i].name)
        << "\",\"phase_order\":" << module.pass_log[i].phase_order
        << ",\"invalidates\":" << module.pass_log[i].invalidates << "}";
  }
  out << "],\n";
  out << "  \"source_hash\": \"sha256:" << json_escape(source_hash) << "\"\n";
  out << "}\n";
  return out.str();
}

std::string module_to_dump(const Module &module,
                           const std::string &source_hash) {
  std::ostringstream out;
  out << "amber.mir.v1 module=";
  out << (module.module_name.empty() ? "<anonymous>" : module.module_name);
  out << " source=sha256:" << source_hash << "\n";
  for (const PassRecord &record : module.pass_log) {
    out << "pass " << record.phase_order << " " << record.name
        << " invalidates=" << record.invalidates << "\n";
  }
  for (const Function &function : module.functions) {
    out << "func @" << function.id << " " << function.name
        << " kind=" << function.kind << " owner=\""
        << json_escape(function.owner) << "\" entry=" << function.entry_block
        << "\n";
    if (!function.locals.empty()) {
      out << "  locals";
      for (const Local &local : function.locals) {
        out << " " << local.slot << ":" << local.name << "/" << local.role;
      }
      out << "\n";
    }
    if (!function.captures.empty()) {
      out << "  captures";
      for (const Capture &capture : function.captures) {
        out << " " << capture.slot << ":" << capture.name << "<-"
            << capture.source_slot;
      }
      out << "\n";
    }
    for (const Block &block : function.blocks) {
      out << block.id << ":\n";
      for (const Instruction &instruction : block.instructions) {
        out << "  ";
        if (!instruction.result.empty()) {
          out << instruction.result << " = ";
        }
        out << instruction.op;
        for (const Operand &operand : instruction.operands) {
          out << " " << operand_to_dump(operand);
        }
        out << attrs_to_dump(instruction.attrs) << "\n";
      }
      out << "  ";
      if (block.has_terminator) {
        out << block.terminator.op;
        for (const Operand &operand : block.terminator.operands) {
          out << " " << operand_to_dump(operand);
        }
        for (const std::string &target : block.terminator.targets) {
          out << " " << target;
        }
      } else {
        out << "<missing-terminator>";
      }
      out << "\n";
    }
  }
  return out.str();
}

std::string
validation_errors_to_json(const std::vector<ValidationError> &errors) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"format\": \"amber.mir.validate.v1\",\n";
  out << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  out << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "{\"code\":\"" << json_escape(errors[i].code) << "\",\"message\":\""
        << json_escape(errors[i].message) << "\",\"function\":\""
        << json_escape(errors[i].function_id) << "\",\"block\":\""
        << json_escape(errors[i].block_id) << "\"}";
  }
  out << "]\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::mir
