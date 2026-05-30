#include "bytecode/emitter.h"

#include <cstdint>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amber::bytecode {

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

bool bool_field(const ast::Expr &expr, const std::string &name) {
  for (const ast::BoolField &field : expr.bool_fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  return false;
}

bool same_span(const lexer::Span &left, const lexer::Span &right) {
  return left.file == right.file && left.start.line == right.start.line &&
         left.start.col == right.start.col &&
         left.start.offset == right.start.offset &&
         left.end.line == right.end.line && left.end.col == right.end.col &&
         left.end.offset == right.end.offset;
}

std::uint32_t parse_slot(const std::string &slot, char prefix) {
  if (slot.size() < 2 || slot.front() != prefix) {
    return 0;
  }
  return static_cast<std::uint32_t>(std::stoul(slot.substr(1)));
}

std::uint32_t parse_u32_string(const std::string &value,
                               std::uint32_t fallback = 0) {
  if (value.empty()) {
    return fallback;
  }
  return static_cast<std::uint32_t>(std::stoul(value));
}

bool has_non_empty_list_field(const ast::Expr &expr, const std::string &name) {
  const ast::ListField *field = list_field(expr, name);
  return field != nullptr && !field->values.empty();
}

bool is_empty_seq_expr(const ast::Expr *expr) {
  if (expr == nullptr || expr->kind != "HSeq") {
    return false;
  }
  const ast::ListField *items = list_field(*expr, "items");
  return items == nullptr || items->values.empty();
}

std::vector<std::string> named_entries(const ast::Expr &expr,
                                       const std::string &field_name) {
  std::vector<std::string> names;
  const ast::ListField *field = list_field(expr, field_name);
  if (field == nullptr) {
    return names;
  }
  for (const std::unique_ptr<ast::Expr> &value : field->values) {
    names.push_back(string_field(*value, "name"));
  }
  return names;
}

std::string json_number_key(double value) {
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
}

std::string unquote_string_literal(const std::string &value) {
  if (value.size() >= 2U) {
    const char quote = value.front();
    if ((quote == '"' || quote == '\'') && value.back() == quote) {
      std::string out;
      for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size() - 1U) {
          out.push_back(value[i]);
          continue;
        }
        const char escaped = value[++i];
        switch (escaped) {
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '"':
          out.push_back('"');
          break;
        case '#':
          out.push_back('#');
          break;
        case 'u': {
          if (i + 1 < value.size() - 1U && value[i + 1] == '{') {
            i += 2;
            std::uint32_t codepoint = 0;
            while (i < value.size() - 1U && value[i] != '}') {
              const char c = value[i++];
              std::uint32_t digit = 0;
              if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
              } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
              } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
              } else {
                digit = 0;
              }
              codepoint = (codepoint << 4U) | digit;
            }
            if (i < value.size() - 1U && value[i] == '}') {
              if (codepoint <= 0x7FU) {
                out.push_back(static_cast<char>(codepoint));
              } else if (codepoint <= 0x7FFU) {
                out.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
                out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
              } else if (codepoint <= 0xFFFFU) {
                out.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
                out.push_back(
                    static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
              } else {
                out.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
                out.push_back(
                    static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
                out.push_back(
                    static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
              }
              break;
            }
          }
          out.push_back('u');
          break;
        }
        default:
          out.push_back(escaped);
          break;
        }
      }
      return out;
    }
  }
  return value;
}

std::string remove_numeric_separators(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    if (c != '_') {
      out.push_back(c);
    }
  }
  return out;
}

std::int64_t parse_integer_literal(const std::string &value) {
  std::string text = remove_numeric_separators(value);
  int base = 10;
  std::size_t start = 0;
  if (text.size() > 2U && text[0] == '0') {
    const char prefix = text[1] >= 'A' && text[1] <= 'Z'
                            ? static_cast<char>(text[1] - 'A' + 'a')
                            : text[1];
    if (prefix == 'x' || prefix == 'b' || prefix == 'o') {
      base = prefix == 'x' ? 16 : (prefix == 'b' ? 2 : 8);
      start = 2;
    }
  }
  return std::stoll(text.substr(start), nullptr, base);
}

double parse_float_literal(const std::string &value) {
  return std::stod(remove_numeric_separators(value));
}

enum class CaptureSourceKind : std::uint32_t { Local = 0, Capture = 1 };

constexpr std::int64_t kPatternSeqModeDirect = 0;
constexpr std::int64_t kPatternSeqModeDeconstruct = 1;
constexpr std::int64_t kPatternFailModeSoft = 0;
constexpr std::int64_t kPatternFailModeMatchError = 1;

struct ProcedureMethodInfo {
  std::uint32_t method_index = 0;
  std::string target_kind;
};

class Emitter;

class CodeEmitter {
public:
  CodeEmitter(Emitter *owner, const hir::Procedure *procedure,
              std::uint32_t code_id, const ast::Expr *body_override = nullptr,
              std::optional<CodeKind> code_kind_override = std::nullopt);

  BcCode emit();
  BcCode emit_clause_pattern_probe(const ast::Expr &clause);

private:
  struct LoopContext {
    std::vector<std::size_t> break_jump_indices;
  };

  struct PatchRef {
    std::size_t instruction_index = 0;
    std::size_t operand_index = 0;
  };

  struct CompiledMapEntry {
    std::uint32_t symbol_id = 0;
    std::uint32_t value_reg = 0;
  };

  std::uint32_t alloc_temp();
  std::uint32_t current_pc() const;
  std::size_t emit_instruction(Opcode opcode,
                               std::vector<InstructionOperand> operands,
                               const lexer::Span &span);
  void patch_operand(std::size_t instruction_index, std::size_t operand_index,
                     std::int64_t value, bool signed_immediate = false);
  void record_line(const lexer::Span &span);
  void compile_seq(const ast::Expr &seq);
  void compile_stmt(const ast::Expr &stmt);
  std::uint32_t compile_expr(const ast::Expr &expr);
  std::uint32_t compile_clause_subject(const ast::Expr &clause);
  void compile_param_pattern_prologues();
  std::uint32_t compile_if(const ast::Expr &expr);
  std::uint32_t compile_logical(const ast::Expr &expr);
  std::uint32_t compile_loop(const ast::Expr &expr);
  std::uint32_t compile_match_dispatch(const ast::Expr &expr);
  std::uint32_t compile_clause_dispatch(const ast::Expr &expr);
  std::uint32_t compile_pattern_assign(const ast::Expr &expr);
  std::uint32_t compile_send_like(const ast::Expr &expr, Opcode opcode);
  std::uint32_t compile_closure(const ast::Expr &expr,
                                std::optional<std::uint32_t> target_reg = {});
  std::uint32_t compile_lookup_like(const ast::Expr &expr,
                                    const std::string &name);
  std::uint32_t compile_const(const ast::Expr &expr);
  std::uint32_t compile_sequence_literal(const ast::Expr &expr, Opcode opcode);
  void emit_make_sequence_from_regs(std::uint32_t dst,
                                    const std::vector<std::uint32_t> &regs,
                                    Opcode opcode, const lexer::Span &span);
  void compile_sequence_literal_suffix(
      const ast::ListField *elements, std::size_t index,
      std::vector<std::uint32_t> prefix_regs, std::uint32_t dst, Opcode opcode,
      const lexer::Span &span);
  std::uint32_t compile_map_literal(const ast::Expr &expr);
  void emit_make_map_from_entries(
      std::uint32_t dst, const std::vector<CompiledMapEntry> &entries,
      const lexer::Span &span);
  void compile_map_literal_suffix(const ast::ListField *entries,
                                  std::size_t index,
                                  std::vector<CompiledMapEntry> prefix_entries,
                                  std::uint32_t dst,
                                  const lexer::Span &span);
  std::uint32_t compile_cond_source(const ast::Expr &cond, Opcode *jump_opcode,
                                    bool *jump_to_then_branch);
  std::uint32_t block_reg_operand(const ast::Expr &expr);
  std::uint32_t emit_call_site(std::uint32_t pc, const std::string &symbol_name,
                               std::uint32_t flags = 0);
  std::uint32_t emit_ivar_site(std::uint32_t pc, const std::string &name,
                               std::uint32_t flags = 0);
  std::uint32_t emit_simple_send(const lexer::Span &span,
                                 std::uint32_t receiver,
                                 const std::string &selector,
                                 const std::vector<std::uint32_t> &pos_args);
  std::uint32_t emit_simple_call(const lexer::Span &span, std::uint32_t callee,
                                 const std::vector<std::uint32_t> &pos_args);
  std::uint32_t emit_load_constant_reg(std::uint32_t constant_id,
                                       const lexer::Span &span);
  void emit_type_error(const lexer::Span &span);
  std::optional<std::uint32_t>
  local_slot_for_binding(const std::string &name,
                         const lexer::Span &span) const;
  std::optional<std::uint32_t>
  local_slot_for_reference(const std::string &name) const;
  std::optional<std::pair<std::uint32_t, std::uint32_t>>
  binding_commit_range(const ast::Expr &match_program) const;
  void add_fail_patch(std::vector<PatchRef> *fail_patches,
                      std::size_t instruction_index, std::size_t operand_index);
  void patch_fail_patches(const std::vector<PatchRef> &fail_patches,
                          std::uint32_t target_pc);
  void emit_fail_jump(std::vector<PatchRef> *fail_patches,
                      const lexer::Span &span);
  void compile_pattern_node(const ast::Expr &node, std::uint32_t value_reg,
                            std::vector<PatchRef> *fail_patches);
  void diag(const lexer::Span &span, const std::string &code,
            const std::string &message);

  Emitter *owner_;
  const hir::Procedure *procedure_;
  const ast::Expr *body_override_ = nullptr;
  BcCode code_;
  std::uint32_t next_temp_ = 0;
  std::vector<LoopContext> loops_;
};

class Emitter {
public:
  Emitter(const hir::Program *program, std::string module_name)
      : program_(program), module_name_(std::move(module_name)) {
    module_.format_version = {1, 0};
    module_.language_version = {1, 0};
  }

  EmitResult emit() {
    if (program_ == nullptr || program_->root == nullptr ||
        program_->root->kind != "HModule") {
      diag({}, "BC2002", "bytecode emitter expects HModule root");
      return {std::move(module_), std::move(diagnostics_)};
    }

    for (std::size_t i = 0; i < program_->procedures.size(); ++i) {
      const hir::Procedure &procedure = program_->procedures[i];
      procedures_by_id_.emplace(procedure.id, &procedure);
      code_id_by_procedure_.emplace(procedure.id,
                                    static_cast<std::uint32_t>(i + 1U));
    }
    next_code_id_ =
        static_cast<std::uint32_t>(program_->procedures.size() + 1U);

    const ast::Expr &root = *program_->root;
    gather_dependencies(root);
    assign_exports(root);
    compile_procedures();
    build_items(root);
    build_exports(root);
    build_init(root);
    return {std::move(module_), std::move(diagnostics_)};
  }

  std::uint32_t intern_string(const std::string &value) {
    const auto found = string_ids_.find(value);
    if (found != string_ids_.end()) {
      return found->second;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(module_.strings.size());
    module_.strings.push_back(value);
    string_ids_.emplace(value, id);
    return id;
  }

  std::uint32_t intern_symbol(const std::string &value) {
    const auto found = symbol_ids_.find(value);
    if (found != symbol_ids_.end()) {
      return found->second;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(module_.symbols.size());
    module_.symbols.push_back(value);
    symbol_ids_.emplace(value, id);
    return id;
  }

  std::uint32_t intern_constant(const Constant &constant) {
    std::ostringstream key;
    key << static_cast<int>(constant.kind) << ":";
    switch (constant.kind) {
    case ConstantKind::Null:
      break;
    case ConstantKind::Bool:
      key << (constant.bool_value ? "1" : "0");
      break;
    case ConstantKind::Integer:
      key << constant.int_value;
      break;
    case ConstantKind::Float:
      key << json_number_key(constant.float_value);
      break;
    case ConstantKind::SymbolRef:
    case ConstantKind::StringRef:
    case ConstantKind::CodeRef:
      key << constant.ref_id;
      break;
    case ConstantKind::KeySet:
    case ConstantKind::Path:
      for (std::uint32_t item : constant.items) {
        key << item << ",";
      }
      break;
    }
    const auto found = const_ids_.find(key.str());
    if (found != const_ids_.end()) {
      return found->second;
    }
    const std::uint32_t id =
        static_cast<std::uint32_t>(module_.const_pool.size());
    module_.const_pool.push_back(constant);
    const_ids_.emplace(key.str(), id);
    return id;
  }

  std::uint32_t intern_empty_keyset() {
    Constant constant;
    constant.kind = ConstantKind::KeySet;
    return intern_constant(constant);
  }

  std::uint32_t intern_keyset(const std::vector<std::string> &keys) {
    Constant constant;
    constant.kind = ConstantKind::KeySet;
    for (const std::string &key : keys) {
      constant.items.push_back(intern_symbol(key));
    }
    return intern_constant(constant);
  }

  std::uint32_t intern_signature_blob(const ast::Expr *signature) {
    Constant constant;
    constant.kind = ConstantKind::Path;
    if (signature != nullptr) {
      if (const ast::ListField *params = list_field(*signature, "params")) {
        for (const std::unique_ptr<ast::Expr> &param : params->values) {
          const std::string name = string_field(*param, "external_name").empty()
                                       ? string_field(*param, "local_name")
                                       : string_field(*param, "external_name");
          constant.items.push_back(intern_symbol(name));
        }
      }
    }
    return intern_constant(constant);
  }

  std::uint32_t intern_type_hook(const std::string &mode,
                                 const std::string &name,
                                 const std::string &type_expr) {
    Constant constant;
    constant.kind = ConstantKind::StringRef;
    constant.ref_id = intern_string(mode + ":" + name + ":" + type_expr);
    return intern_constant(constant);
  }

  std::uint32_t intern_path_ref(const std::string &name) {
    Constant constant;
    constant.kind = ConstantKind::Path;
    std::size_t start = 0;
    while (start <= name.size()) {
      const std::size_t end = name.find('.', start);
      const std::string segment = end == std::string::npos
                                      ? name.substr(start)
                                      : name.substr(start, end - start);
      if (!segment.empty()) {
        constant.items.push_back(intern_symbol(segment));
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1U;
    }
    if (constant.items.empty()) {
      constant.items.push_back(intern_symbol(name));
    }
    return intern_constant(constant);
  }

  std::uint32_t intern_lookup_path(const std::string &name) {
    return intern_path_ref(name);
  }

  std::uint32_t intern_literal_constant(const std::string &token,
                                        const std::string &value) {
    if (token == "KEYWORD_NULL") {
      Constant constant;
      constant.kind = ConstantKind::Null;
      return intern_constant(constant);
    }
    if (token == "KEYWORD_TRUE" || token == "KEYWORD_FALSE") {
      Constant constant;
      constant.kind = ConstantKind::Bool;
      constant.bool_value = token == "KEYWORD_TRUE";
      return intern_constant(constant);
    }
    if (token == "INTEGER") {
      Constant constant;
      constant.kind = ConstantKind::Integer;
      constant.int_value = parse_integer_literal(value);
      return intern_constant(constant);
    }
    if (token == "FLOAT") {
      Constant constant;
      constant.kind = ConstantKind::Float;
      constant.float_value = parse_float_literal(value);
      return intern_constant(constant);
    }
    if (token == "STRING") {
      Constant constant;
      constant.kind = ConstantKind::StringRef;
      constant.ref_id = intern_string(unquote_string_literal(value));
      return intern_constant(constant);
    }
    if (token == "SYMBOL") {
      Constant constant;
      constant.kind = ConstantKind::SymbolRef;
      constant.ref_id = intern_symbol(value);
      return intern_constant(constant);
    }
    Constant constant;
    constant.kind = ConstantKind::StringRef;
    constant.ref_id = intern_string(value);
    return intern_constant(constant);
  }

  const hir::Procedure *procedure_by_id(const std::string &id) const {
    const auto found = procedures_by_id_.find(id);
    return found == procedures_by_id_.end() ? nullptr : found->second;
  }

  std::optional<std::uint32_t>
  code_id_for_procedure(const std::string &id) const {
    const auto found = code_id_by_procedure_.find(id);
    if (found == code_id_by_procedure_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  void diag(const lexer::Span &span, const std::string &code,
            const std::string &message) {
    diagnostics_.push_back({code, "error", "bytecode", message, span});
  }

  BcModule *module() { return &module_; }

private:
  std::uint32_t allocate_code_id() { return next_code_id_++; }

  void append_local_debug(const hir::Procedure &procedure, const BcCode &code) {
    for (const hir::ProcedureLocal &local : procedure.locals) {
      module_.local_debug.push_back(
          {code.code_id, parse_slot(local.slot, 'l'), intern_string(local.name),
           0, static_cast<std::uint32_t>(code.instructions.size())});
    }
  }

  std::uint32_t emit_embedded_code(const hir::Procedure &procedure,
                                   const ast::Expr &body, CodeKind kind) {
    const std::uint32_t code_id = allocate_code_id();
    CodeEmitter emitter(this, &procedure, code_id, &body, kind);
    BcCode code = emitter.emit();
    append_local_debug(procedure, code);
    module_.code_objects.push_back(std::move(code));
    return code_id;
  }

  std::uint32_t emit_clause_pattern_code(const hir::Procedure &procedure,
                                         const ast::Expr &clause) {
    const std::uint32_t code_id = allocate_code_id();
    CodeEmitter emitter(this, &procedure, code_id);
    BcCode code = emitter.emit_clause_pattern_probe(clause);
    append_local_debug(procedure, code);
    module_.code_objects.push_back(std::move(code));
    return code_id;
  }

  std::uint32_t register_pattern_program(const ast::Expr &compiled_pattern) {
    std::uint32_t binding_count = 0;
    std::uint32_t flags = 0;
    if (const ast::Expr *match_program =
            node_field(compiled_pattern, "match_program")) {
      if (const ast::ListField *binding_order =
              list_field(*match_program, "binding_order")) {
        binding_count =
            static_cast<std::uint32_t>(binding_order->values.size());
      }
      if (bool_field(*match_program, "requires_commit")) {
        flags |= 1U;
      }
    }

    const std::uint32_t pattern_id =
        static_cast<std::uint32_t>(module_.pattern_programs.size() + 1U);
    module_.pattern_programs.push_back({pattern_id, binding_count, flags});
    return pattern_id;
  }

  std::vector<ClauseEntry>
  build_clause_entries(const ast::Expr &item, const hir::Procedure &procedure) {
    std::vector<ClauseEntry> clauses;
    const ast::ListField *clause_list = list_field(item, "clauses");
    if (clause_list == nullptr) {
      return clauses;
    }

    for (const std::unique_ptr<ast::Expr> &clause : clause_list->values) {
      const ast::Expr *compiled_pattern =
          node_field(*clause, "compiled_pattern");
      const ast::Expr *body = node_field(*clause, "body");
      if (compiled_pattern == nullptr || body == nullptr) {
        diag(clause->span, "BC2001",
             "clause method is missing compiled pattern or body");
        continue;
      }

      std::unique_ptr<ast::Expr> synthetic_guard;
      const ast::Expr *guard = node_field(*clause, "guard");
      if (guard == nullptr) {
        synthetic_guard = std::make_unique<ast::Expr>("HConst", clause->span);
        synthetic_guard->string_fields.push_back({"token", "KEYWORD_TRUE"});
        synthetic_guard->string_fields.push_back({"value", "true"});
        guard = synthetic_guard.get();
      }

      clauses.push_back({register_pattern_program(*compiled_pattern),
                         emit_clause_pattern_code(procedure, *clause),
                         emit_embedded_code(procedure, *guard, CodeKind::Block),
                         emit_embedded_code(procedure, *body, CodeKind::Block),
                         0});
    }
    return clauses;
  }

  void gather_dependencies(const ast::Expr &root) {
    const ast::ListField *imports = list_field(root, "imports");
    if (imports == nullptr) {
      return;
    }
    std::map<std::string, bool> seen;
    for (const std::unique_ptr<ast::Expr> &item : imports->values) {
      const std::string module_id = string_field(*item, "module_id");
      if (module_id.empty() || seen[module_id]) {
        continue;
      }
      seen[module_id] = true;
      DepEntry dep;
      dep.module_name_str_id = intern_string(module_id);
      dep.required_format = {1, 0};
      dep.min_language_version = {1, 0};
      module_.dependencies.push_back(dep);
    }
  }

  void assign_exports(const ast::Expr &root) {
    const ast::ListField *items = list_field(root, "items");
    if (items == nullptr) {
      return;
    }
    for (const std::unique_ptr<ast::Expr> &item : items->values) {
      if (item->kind == "HMethod") {
        declared_exports_.emplace(string_field(*item, "name"),
                                  ProcedureMethodInfo{0, "method"});
      } else if (item->kind == "HClass" || item->kind == "HMixin") {
        declared_classes_.emplace(string_field(*item, "name"), 0);
      }
    }
  }

  void compile_procedures() {
    for (const hir::Procedure &procedure : program_->procedures) {
      const std::uint32_t code_id = code_id_by_procedure_.at(procedure.id);
      CodeEmitter emitter(this, &procedure, code_id);
      BcCode code = emitter.emit();
      append_local_debug(procedure, code);
      module_.code_objects.push_back(std::move(code));
    }
  }

  void append_path_refs(const ast::Expr &item, const char *field_name,
                        std::vector<std::uint32_t> &out) {
    const ast::ListField *paths = list_field(item, field_name);
    if (paths == nullptr) {
      return;
    }
    for (const std::unique_ptr<ast::Expr> &path : paths->values) {
      out.push_back(intern_path_ref(string_field(*path, "path")));
    }
  }

  BcClass build_class_like(const ast::Expr &item, bool is_mixin) {
    const std::string class_name = string_field(item, "name");
    const std::uint32_t class_index =
        static_cast<std::uint32_t>(module_.classes.size());
    declared_classes_[class_name] = class_index;

    BcClass klass;
    klass.class_name_sym_id = intern_symbol(class_name);
    klass.ivar_schema_id = intern_empty_keyset();
    klass.method_range_start =
        static_cast<std::uint32_t>(module_.methods.size());
    if (is_mixin) {
      klass.flags |= kClassFlagMixin;
    }

    const std::string superclass = string_field(item, "superclass");
    if (!superclass.empty()) {
      klass.has_superclass_ref = true;
      klass.superclass_ref = intern_path_ref(superclass);
    }

    const ast::ListField *body = list_field(item, "body");
    if (body != nullptr) {
      for (const std::unique_ptr<ast::Expr> &member : body->values) {
        if (member->kind == "HMethod") {
          module_.methods.push_back(build_method(*member, class_index));
        } else if (member->kind == "HInclude") {
          append_path_refs(*member, "paths", klass.direct_include_refs);
        } else if (member->kind == "HExtend" && !is_mixin) {
          append_path_refs(*member, "paths", klass.direct_extend_refs);
        } else {
          diag(member->span, "BC2002",
               "unsupported class or mixin body item in bytecode emitter");
        }
      }
    }

    klass.method_range_count =
        static_cast<std::uint32_t>(module_.methods.size()) -
        klass.method_range_start;
    return klass;
  }

  void build_items(const ast::Expr &root) {
    const ast::ListField *items = list_field(root, "items");
    if (items == nullptr) {
      return;
    }

    for (const std::unique_ptr<ast::Expr> &item : items->values) {
      if (item->kind == "HMethod") {
        const std::uint32_t method_index =
            static_cast<std::uint32_t>(module_.methods.size());
        BcMethod method = build_method(*item, std::nullopt);
        declared_exports_[string_field(*item, "name")] = {method_index,
                                                          "method"};
        module_.methods.push_back(std::move(method));
        continue;
      }

      if (item->kind == "HClass") {
        module_.classes.push_back(build_class_like(*item, false));
        continue;
      }

      if (item->kind == "HMixin") {
        module_.classes.push_back(build_class_like(*item, true));
        continue;
      }

      diag(item->span, "BC2002", "unsupported top-level HIR item");
    }
  }

  BcMethod build_method(const ast::Expr &item,
                        std::optional<std::uint32_t> owner_class) {
    BcMethod method;
    method.selector_sym_id = intern_symbol(string_field(item, "name"));
    method.owner_dispatch_ref = owner_class.value_or(0U);
    method.signature_blob_id =
        intern_signature_blob(node_field(item, "signature"));

    const ast::ListField *auto_assign = list_field(item, "auto_assign");
    if (auto_assign != nullptr) {
      for (const std::unique_ptr<ast::Expr> &entry : auto_assign->values) {
        method.auto_assign_desc.push_back(
            {intern_string(string_field(*entry, "local_name")),
             intern_string(string_field(*entry, "target")), 0});
      }
    }

    const std::string procedure_id = string_field(item, "procedure");
    const hir::Procedure *procedure = procedure_by_id(procedure_id);
    const ast::Expr *signature =
        procedure != nullptr && procedure->signature != nullptr
            ? procedure->signature.get()
            : node_field(item, "signature");
    if (procedure == nullptr) {
      diag(item.span, "BC2003", "missing procedure for emitted method");
      method.entry_code_id = 0;
    } else if (has_non_empty_list_field(item, "clauses") ||
               node_field(item, "else_body") != nullptr) {
      const ast::Expr *else_body = node_field(item, "else_body");
      if (else_body == nullptr) {
        diag(item.span, "BC2001", "clause-style method is missing else_body");
        method.entry_code_id = 0;
      } else {
        method.entry_code_id =
            emit_embedded_code(*procedure, *else_body, CodeKind::Method);
        method.clause_table = build_clause_entries(item, *procedure);
      }
    } else {
      const std::optional<std::uint32_t> code_id =
          code_id_for_procedure(procedure_id);
      if (!code_id.has_value()) {
        diag(item.span, "BC2003", "missing procedure for emitted method");
        method.entry_code_id = 0;
      } else {
        method.entry_code_id = *code_id;
      }
    }

    if (procedure != nullptr && signature != nullptr) {
      if (const ast::ListField *params = list_field(*signature, "params")) {
        for (const std::unique_ptr<ast::Expr> &param : params->values) {
          MethodParamEntry entry;
          entry.external_name_sym_id =
              intern_symbol(string_field(*param, "external_name").empty()
                                ? string_field(*param, "local_name")
                                : string_field(*param, "external_name"));
          entry.local_name_str_id =
              intern_string(string_field(*param, "local_name"));
          entry.flags = 0;
          if (string_field(*param, "kind") == "keyword") {
            entry.flags |= kMethodParamFlagKeyword;
          }
          if (!bool_field(*param, "has_default")) {
            method.params.push_back(entry);
            const std::string type_expr = string_field(*param, "type_expr");
            if (!type_expr.empty()) {
              method.type_hook_ids.push_back(intern_type_hook(
                  "parameter", string_field(*param, "local_name"), type_expr));
            }
            continue;
          }
          entry.flags |= kMethodParamFlagHasDefault;
          method.params.push_back(entry);
          const std::string type_expr = string_field(*param, "type_expr");
          if (!type_expr.empty()) {
            method.type_hook_ids.push_back(intern_type_hook(
                "parameter", string_field(*param, "local_name"), type_expr));
          }
          const ast::Expr *default_expr = node_field(*param, "default_expr");
          if (default_expr == nullptr) {
            diag(param->span, "BC2001",
                 "defaulted param is missing lowered default_expr");
            continue;
          }
          method.default_thunk_ids.push_back(emit_embedded_code(
              *procedure, *default_expr, CodeKind::DefaultThunk));
        }
      }
      const std::string return_type_expr =
          string_field(*signature, "return_type_expr");
      if (!return_type_expr.empty()) {
        method.type_hook_ids.push_back(intern_type_hook(
            "return", string_field(item, "name"), return_type_expr));
      }
    }

    const std::string dispatch_side = string_field(item, "dispatch_side");
    if (dispatch_side == "instance") {
      method.flags = 1;
    } else if (dispatch_side == "class") {
      method.flags = 2;
    } else {
      method.flags = 0;
    }
    return method;
  }

  void build_exports(const ast::Expr &root) {
    const ast::ListField *exports = list_field(root, "exports");
    if (exports == nullptr) {
      return;
    }
    for (const std::unique_ptr<ast::Expr> &item : exports->values) {
      const std::string local_name = string_field(*item, "local_name");
      const std::string public_name = string_field(*item, "public_name");
      const auto method_it = declared_exports_.find(local_name);
      if (method_it != declared_exports_.end()) {
        module_.exports.push_back({intern_symbol(public_name),
                                   intern_string(method_it->second.target_kind),
                                   method_it->second.method_index, 1});
        continue;
      }
      const auto class_it = declared_classes_.find(local_name);
      if (class_it != declared_classes_.end()) {
        module_.exports.push_back({intern_symbol(public_name),
                                   intern_string("class"), class_it->second,
                                   1});
        continue;
      }
      diag(item->span, "BC2002",
           "unsupported export target in bytecode emitter");
    }
  }

  void build_init(const ast::Expr &root) {
    const std::string procedure_id = string_field(root, "init");
    const std::optional<std::uint32_t> code_id =
        code_id_for_procedure(procedure_id);
    if (!code_id.has_value()) {
      diag(root.span, "BC2003", "missing module init procedure");
      return;
    }
    module_.init = {true, *code_id, 0};
  }

  const hir::Program *program_;
  std::string module_name_;
  BcModule module_;
  std::vector<lexer::Diagnostic> diagnostics_;
  std::unordered_map<std::string, std::uint32_t> string_ids_;
  std::unordered_map<std::string, std::uint32_t> symbol_ids_;
  std::unordered_map<std::string, std::uint32_t> const_ids_;
  std::unordered_map<std::string, const hir::Procedure *> procedures_by_id_;
  std::unordered_map<std::string, std::uint32_t> code_id_by_procedure_;
  std::unordered_map<std::string, ProcedureMethodInfo> declared_exports_;
  std::unordered_map<std::string, std::uint32_t> declared_classes_;
  std::uint32_t next_code_id_ = 1;

  friend class CodeEmitter;
};

CodeEmitter::CodeEmitter(Emitter *owner, const hir::Procedure *procedure,
                         std::uint32_t code_id, const ast::Expr *body_override,
                         std::optional<CodeKind> code_kind_override)
    : owner_(owner), procedure_(procedure), body_override_(body_override) {
  code_.code_id = code_id;
  if (code_kind_override.has_value()) {
    code_.kind = *code_kind_override;
  } else if (procedure_->kind == "module_init") {
    code_.kind = CodeKind::Module;
  } else if (procedure_->kind == "closure") {
    code_.kind = CodeKind::Block;
  } else {
    code_.kind = CodeKind::Method;
  }

  for (const hir::ProcedureLocal &local : procedure_->locals) {
    code_.local_layout.push_back({parse_slot(local.slot, 'l'),
                                  owner_->intern_string(local.name),
                                  owner_->intern_string(local.role),
                                  owner_->intern_string(local.binding_kind)});
  }
  for (const hir::ProcedureCapture &capture : procedure_->captures) {
    code_.capture_layout.push_back(
        {parse_slot(capture.slot, 'u'), owner_->intern_string(capture.name),
         owner_->intern_string(capture.source_kind),
         owner_->intern_string(capture.source_name)});
  }

  next_temp_ = static_cast<std::uint32_t>(procedure_->locals.size());
}

BcCode CodeEmitter::emit() {
  compile_param_pattern_prologues();
  const ast::Expr *body =
      body_override_ != nullptr ? body_override_ : procedure_->body.get();
  if (body != nullptr && body->kind == "HSeq") {
    compile_seq(*body);
  } else if (body != nullptr) {
    compile_stmt(*body);
  }

  emit_instruction(Opcode::CloseUpvalues, {{0, false}}, procedure_->span);
  const std::uint32_t last_reg = alloc_temp();
  emit_instruction(Opcode::GetLast, {{last_reg, false}}, procedure_->span);
  emit_instruction(Opcode::Return, {{last_reg, false}}, procedure_->span);

  code_.reg_count = next_temp_;
  return code_;
}

BcCode CodeEmitter::emit_clause_pattern_probe(const ast::Expr &clause) {
  const ast::Expr *compiled_pattern = node_field(clause, "compiled_pattern");
  const ast::Expr *match_program =
      compiled_pattern == nullptr
          ? nullptr
          : node_field(*compiled_pattern, "match_program");
  const ast::Expr *root =
      match_program == nullptr ? nullptr : node_field(*match_program, "root");
  if (compiled_pattern == nullptr || match_program == nullptr ||
      root == nullptr) {
    diag(clause.span, "BC2001",
         "clause pattern probe is missing compiled_pattern or match_program");
  } else {
    std::vector<PatchRef> fail_patches;
    const std::uint32_t subject_reg = compile_clause_subject(clause);
    compile_pattern_node(*root, subject_reg, &fail_patches);
    if (bool_field(*match_program, "requires_commit")) {
      const auto commit = binding_commit_range(*match_program);
      if (!commit.has_value()) {
        diag(clause.span, "BC2001",
             "clause bindings are not representable as one contiguous commit "
             "range");
      } else if (commit->second != 0U) {
        emit_instruction(Opcode::PCommit,
                         {{commit->first, false}, {commit->second, false}},
                         clause.span);
      }
    }

    const std::uint32_t success_reg = alloc_temp();
    emit_instruction(Opcode::LoadBool, {{success_reg, false}, {1, false}},
                     clause.span);
    emit_instruction(Opcode::Return, {{success_reg, false}}, clause.span);

    patch_fail_patches(fail_patches, current_pc());
  }

  emit_instruction(Opcode::PFail, {{kPatternFailModeSoft, false}}, clause.span);
  const std::uint32_t fail_reg = alloc_temp();
  emit_instruction(Opcode::LoadBool, {{fail_reg, false}, {0, false}},
                   clause.span);
  emit_instruction(Opcode::Return, {{fail_reg, false}}, clause.span);

  code_.reg_count = next_temp_;
  return code_;
}

std::uint32_t CodeEmitter::alloc_temp() { return next_temp_++; }

std::uint32_t CodeEmitter::compile_clause_subject(const ast::Expr &clause) {
  if (procedure_ == nullptr || procedure_->signature == nullptr) {
    diag(clause.span, "BC2003",
         "clause pattern probe is missing procedure signature");
    const std::uint32_t dst = alloc_temp();
    emit_instruction(Opcode::LoadNull, {{dst, false}}, clause.span);
    return dst;
  }

  const ast::ListField *params = list_field(*procedure_->signature, "params");
  const std::string subject_kind = string_field(clause, "subject_kind");
  if (subject_kind == "single_positional") {
    if (params != nullptr) {
      for (const std::unique_ptr<ast::Expr> &param : params->values) {
        if (string_field(*param, "kind") == "keyword") {
          continue;
        }
        const std::optional<std::uint32_t> slot =
            local_slot_for_reference(string_field(*param, "local_name"));
        if (slot.has_value()) {
          return *slot;
        }
      }
    }
    diag(clause.span, "BC2001",
         "single_positional clause is missing positional parameter slot");
    const std::uint32_t dst = alloc_temp();
    emit_instruction(Opcode::LoadNull, {{dst, false}}, clause.span);
    return dst;
  }

  if (subject_kind == "named_args_map") {
    const std::uint32_t dst = alloc_temp();
    std::vector<InstructionOperand> operands;
    operands.push_back({dst, false});
    std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;
    if (params != nullptr) {
      for (const std::unique_ptr<ast::Expr> &param : params->values) {
        if (string_field(*param, "kind") != "keyword") {
          continue;
        }
        const std::optional<std::uint32_t> slot =
            local_slot_for_reference(string_field(*param, "local_name"));
        if (!slot.has_value()) {
          diag(param->span, "BC2001",
               "named_args_map clause is missing keyword parameter slot");
          continue;
        }
        const std::string key = string_field(*param, "external_name").empty()
                                    ? string_field(*param, "local_name")
                                    : string_field(*param, "external_name");
        entries.push_back({owner_->intern_symbol(key), *slot});
      }
    }
    operands.push_back({static_cast<std::int64_t>(entries.size()), false});
    for (const auto &[symbol_id, reg] : entries) {
      operands.push_back({symbol_id, false});
      operands.push_back({reg, false});
    }
    emit_instruction(Opcode::MakeMap, std::move(operands), clause.span);
    return dst;
  }

  std::vector<std::uint32_t> positional_slots;
  if (params != nullptr) {
    for (const std::unique_ptr<ast::Expr> &param : params->values) {
      if (string_field(*param, "kind") == "keyword") {
        continue;
      }
      const std::optional<std::uint32_t> slot =
          local_slot_for_reference(string_field(*param, "local_name"));
      if (!slot.has_value()) {
        diag(param->span, "BC2001",
             "positional_tuple clause is missing positional parameter slot");
        continue;
      }
      positional_slots.push_back(*slot);
    }
  }

  std::vector<std::uint32_t> temp_slots;
  temp_slots.reserve(positional_slots.size());
  for (std::uint32_t slot : positional_slots) {
    const std::uint32_t temp = alloc_temp();
    emit_instruction(Opcode::Move, {{temp, false}, {slot, false}}, clause.span);
    temp_slots.push_back(temp);
  }

  const std::uint32_t dst = alloc_temp();
  emit_instruction(Opcode::MakeTuple,
                   {{dst, false},
                    {temp_slots.empty() ? 0 : temp_slots.front(), false},
                    {static_cast<std::int64_t>(temp_slots.size()), false}},
                   clause.span);
  return dst;
}

void CodeEmitter::compile_param_pattern_prologues() {
  if (procedure_ == nullptr || procedure_->param_patterns.empty()) {
    return;
  }

  std::vector<PatchRef> fail_patches;
  for (const std::unique_ptr<ast::Expr> &param_pattern :
       procedure_->param_patterns) {
    if (param_pattern == nullptr) {
      continue;
    }
    const ast::Expr *compiled_pattern =
        node_field(*param_pattern, "compiled_pattern");
    if (compiled_pattern == nullptr) {
      diag(param_pattern->span, "BC2001",
           "block param pattern is missing compiled_pattern");
      emit_fail_jump(&fail_patches, param_pattern->span);
      continue;
    }
    const ast::Expr *match_program =
        node_field(*compiled_pattern, "match_program");
    if (match_program == nullptr) {
      diag(param_pattern->span, "BC2001",
           "block param pattern is missing match_program");
      emit_fail_jump(&fail_patches, param_pattern->span);
      continue;
    }
    const ast::Expr *root = node_field(*match_program, "root");
    if (root == nullptr) {
      diag(param_pattern->span, "BC2001",
           "block param pattern is missing match root");
      emit_fail_jump(&fail_patches, param_pattern->span);
      continue;
    }
    const std::optional<std::uint32_t> source_slot =
        local_slot_for_reference(string_field(*param_pattern, "param_slot"));
    if (!source_slot.has_value()) {
      diag(param_pattern->span, "BC2001", "block param source slot is missing");
      emit_fail_jump(&fail_patches, param_pattern->span);
      continue;
    }

    compile_pattern_node(*root, *source_slot, &fail_patches);
    if (bool_field(*match_program, "requires_commit")) {
      const auto commit = binding_commit_range(*match_program);
      if (!commit.has_value()) {
        diag(param_pattern->span, "BC2001",
             "block param pattern bindings are not representable as one "
             "contiguous commit range");
        emit_fail_jump(&fail_patches, param_pattern->span);
        continue;
      }
      emit_instruction(Opcode::PCommit,
                       {{commit->first, false}, {commit->second, false}},
                       param_pattern->span);
    }
  }

  if (!fail_patches.empty()) {
    const std::uint32_t fail_pc = current_pc();
    patch_fail_patches(fail_patches, fail_pc);
    emit_instruction(Opcode::PFail, {{kPatternFailModeMatchError, false}},
                     procedure_->span);
  }
}

std::uint32_t CodeEmitter::current_pc() const {
  return static_cast<std::uint32_t>(code_.instructions.size());
}

std::size_t
CodeEmitter::emit_instruction(Opcode opcode,
                              std::vector<InstructionOperand> operands,
                              const lexer::Span &span) {
  const std::size_t pc = code_.instructions.size();
  code_.instructions.push_back({opcode, std::move(operands)});
  code_.source_spans.push_back({static_cast<std::uint32_t>(pc),
                                static_cast<std::uint32_t>(pc + 1U), span});
  record_line(span);
  return pc;
}

void CodeEmitter::patch_operand(std::size_t instruction_index,
                                std::size_t operand_index, std::int64_t value,
                                bool signed_immediate) {
  if (instruction_index >= code_.instructions.size() ||
      operand_index >= code_.instructions[instruction_index].operands.size()) {
    return;
  }
  code_.instructions[instruction_index].operands[operand_index].value = value;
  code_.instructions[instruction_index]
      .operands[operand_index]
      .signed_immediate = signed_immediate;
}

void CodeEmitter::record_line(const lexer::Span &span) {
  owner_->module()->line_table.push_back(
      {code_.code_id, current_pc() - 1U,
       static_cast<std::uint32_t>(span.start.line)});
}

void CodeEmitter::diag(const lexer::Span &span, const std::string &code,
                       const std::string &message) {
  owner_->diag(span, code, message);
}

void CodeEmitter::compile_seq(const ast::Expr &seq) {
  const ast::ListField *items = list_field(seq, "items");
  if (items == nullptr) {
    return;
  }
  for (const std::unique_ptr<ast::Expr> &item : items->values) {
    compile_stmt(*item);
  }
}

void CodeEmitter::compile_stmt(const ast::Expr &stmt) {
  if (stmt.kind == "HSeq") {
    compile_seq(stmt);
    return;
  }
  if (stmt.kind == "HLastSet") {
    const ast::Expr *expr = node_field(stmt, "expr");
    if (expr == nullptr) {
      diag(stmt.span, "BC2001", "HLastSet is missing expr");
      return;
    }
    const std::uint32_t reg = compile_expr(*expr);
    emit_instruction(Opcode::SetLast, {{reg, false}}, stmt.span);
    return;
  }

  const std::uint32_t reg = compile_expr(stmt);
  emit_instruction(Opcode::SetLast, {{reg, false}}, stmt.span);
}

std::uint32_t CodeEmitter::compile_const(const ast::Expr &expr) {
  const std::string token = string_field(expr, "token");
  const std::string value = string_field(expr, "value");
  const std::uint32_t dst = alloc_temp();
  if (token == "KEYWORD_NULL") {
    emit_instruction(Opcode::LoadNull, {{dst, false}}, expr.span);
    return dst;
  }
  if (token == "KEYWORD_TRUE" || token == "KEYWORD_FALSE") {
    emit_instruction(Opcode::LoadBool,
                     {{dst, false}, {token == "KEYWORD_TRUE" ? 1 : 0, false}},
                     expr.span);
    return dst;
  }
  const std::uint32_t constant_id =
      owner_->intern_literal_constant(token, value);
  emit_instruction(Opcode::LoadK, {{dst, false}, {constant_id, false}},
                   expr.span);
  return dst;
}

std::uint32_t CodeEmitter::compile_sequence_literal(const ast::Expr &expr,
                                                    Opcode opcode) {
  const ast::ListField *elements = list_field(expr, "elements");
  bool has_conditional = false;
  if (elements != nullptr) {
    for (const std::unique_ptr<ast::Expr> &element : elements->values) {
      if (element != nullptr && element->kind == "HConditionalElement") {
        has_conditional = true;
        break;
      }
    }
  }
  if (has_conditional) {
    const std::uint32_t dst = alloc_temp();
    compile_sequence_literal_suffix(elements, 0, {}, dst, opcode, expr.span);
    return dst;
  }

  const std::uint32_t count =
      elements == nullptr ? 0U
                          : static_cast<std::uint32_t>(elements->values.size());
  const std::uint32_t dst = alloc_temp();
  std::uint32_t first_reg = 0;
  if (count != 0U) {
    first_reg = alloc_temp();
    for (std::uint32_t i = 1; i < count; ++i) {
      alloc_temp();
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint32_t src = compile_expr(*elements->values[i]);
      const std::uint32_t target = first_reg + i;
      if (src != target) {
        emit_instruction(Opcode::Move, {{target, false}, {src, false}},
                         elements->values[i]->span);
      }
    }
  }
  emit_instruction(opcode, {{dst, false}, {first_reg, false}, {count, false}},
                   expr.span);
  return dst;
}

void CodeEmitter::emit_make_sequence_from_regs(
    std::uint32_t dst, const std::vector<std::uint32_t> &regs, Opcode opcode,
    const lexer::Span &span) {
  std::uint32_t first_reg = 0;
  if (!regs.empty()) {
    first_reg = alloc_temp();
    for (std::size_t i = 1; i < regs.size(); ++i) {
      alloc_temp();
    }
    for (std::size_t i = 0; i < regs.size(); ++i) {
      const std::uint32_t target = first_reg + static_cast<std::uint32_t>(i);
      if (regs[i] != target) {
        emit_instruction(Opcode::Move, {{target, false}, {regs[i], false}},
                         span);
      }
    }
  }
  emit_instruction(
      opcode,
      {{dst, false}, {first_reg, false},
       {static_cast<std::int64_t>(regs.size()), false}},
      span);
}

void CodeEmitter::compile_sequence_literal_suffix(
    const ast::ListField *elements, std::size_t index,
    std::vector<std::uint32_t> prefix_regs, std::uint32_t dst, Opcode opcode,
    const lexer::Span &span) {
  if (elements == nullptr || index >= elements->values.size()) {
    emit_make_sequence_from_regs(dst, prefix_regs, opcode, span);
    return;
  }

  const ast::Expr &element = *elements->values[index];
  if (element.kind != "HConditionalElement") {
    prefix_regs.push_back(compile_expr(element));
    compile_sequence_literal_suffix(elements, index + 1, std::move(prefix_regs),
                                    dst, opcode, span);
    return;
  }

  const ast::Expr *condition = node_field(element, "condition");
  const ast::Expr *value = node_field(element, "value");
  if (condition == nullptr || value == nullptr) {
    diag(element.span, "BC2001", "invalid conditional collection element");
    compile_sequence_literal_suffix(elements, index + 1, std::move(prefix_regs),
                                    dst, opcode, span);
    return;
  }

  const std::uint32_t condition_reg = compile_expr(*condition);
  const Opcode skip_opcode =
      string_field(element, "condition_kind") == "unless" ? Opcode::JumpIfTrue
                                                          : Opcode::JumpIfFalse;
  const std::size_t jump_skip = emit_instruction(
      skip_opcode, {{condition_reg, false}, {-1, true}}, condition->span);

  std::vector<std::uint32_t> with_value = prefix_regs;
  with_value.push_back(compile_expr(*value));
  compile_sequence_literal_suffix(elements, index + 1, std::move(with_value),
                                  dst, opcode, span);
  const std::size_t jump_end =
      emit_instruction(Opcode::Jump, {{-1, true}}, element.span);
  patch_operand(jump_skip, 1, current_pc(), false);
  compile_sequence_literal_suffix(elements, index + 1, std::move(prefix_regs),
                                  dst, opcode, span);
  patch_operand(jump_end, 0, current_pc(), false);
}

std::uint32_t CodeEmitter::compile_map_literal(const ast::Expr &expr) {
  const ast::ListField *entries = list_field(expr, "entries");
  const std::uint32_t dst = alloc_temp();
  bool has_conditional = false;
  if (entries != nullptr) {
    for (const std::unique_ptr<ast::Expr> &entry : entries->values) {
      if (entry != nullptr && node_field(*entry, "condition") != nullptr) {
        has_conditional = true;
        break;
      }
    }
  }
  if (has_conditional) {
    compile_map_literal_suffix(entries, 0, {}, dst, expr.span);
    return dst;
  }

  std::vector<CompiledMapEntry> compiled_entries;

  if (entries != nullptr) {
    for (const std::unique_ptr<ast::Expr> &entry : entries->values) {
      const ast::Expr *value = node_field(*entry, "value");
      if (entry->kind != "HMapEntry" || value == nullptr) {
        diag(entry->span, "BC2001", "invalid map literal entry");
        continue;
      }
      const std::string key =
          string_field(*entry, "key_kind") == "string"
              ? unquote_string_literal(string_field(*entry, "key"))
              : string_field(*entry, "key");
      compiled_entries.push_back(
          CompiledMapEntry{owner_->intern_symbol(key), compile_expr(*value)});
    }
  }

  emit_make_map_from_entries(dst, compiled_entries, expr.span);
  return dst;
}

void CodeEmitter::emit_make_map_from_entries(
    std::uint32_t dst, const std::vector<CompiledMapEntry> &entries,
    const lexer::Span &span) {
  std::vector<InstructionOperand> operands;
  operands.push_back({dst, false});
  operands.push_back({static_cast<std::int64_t>(entries.size()), false});
  for (const CompiledMapEntry &entry : entries) {
    operands.push_back({entry.symbol_id, false});
    operands.push_back({entry.value_reg, false});
  }

  emit_instruction(Opcode::MakeMap, std::move(operands), span);
}

void CodeEmitter::compile_map_literal_suffix(
    const ast::ListField *entries, std::size_t index,
    std::vector<CompiledMapEntry> prefix_entries, std::uint32_t dst,
    const lexer::Span &span) {
  if (entries == nullptr || index >= entries->values.size()) {
    emit_make_map_from_entries(dst, prefix_entries, span);
    return;
  }

  const ast::Expr &entry = *entries->values[index];
  const ast::Expr *value = node_field(entry, "value");
  if (entry.kind != "HMapEntry" || value == nullptr) {
    diag(entry.span, "BC2001", "invalid map literal entry");
    compile_map_literal_suffix(entries, index + 1, std::move(prefix_entries),
                               dst, span);
    return;
  }
  const std::string key =
      string_field(entry, "key_kind") == "string"
          ? unquote_string_literal(string_field(entry, "key"))
          : string_field(entry, "key");
  const std::uint32_t symbol_id = owner_->intern_symbol(key);
  const ast::Expr *condition = node_field(entry, "condition");
  if (condition == nullptr) {
    prefix_entries.push_back(
        CompiledMapEntry{symbol_id, compile_expr(*value)});
    compile_map_literal_suffix(entries, index + 1, std::move(prefix_entries),
                               dst, span);
    return;
  }

  const std::uint32_t condition_reg = compile_expr(*condition);
  const Opcode skip_opcode =
      string_field(entry, "condition_kind") == "unless" ? Opcode::JumpIfTrue
                                                        : Opcode::JumpIfFalse;
  const std::size_t jump_skip = emit_instruction(
      skip_opcode, {{condition_reg, false}, {-1, true}}, condition->span);

  std::vector<CompiledMapEntry> with_entry = prefix_entries;
  with_entry.push_back(CompiledMapEntry{symbol_id, compile_expr(*value)});
  compile_map_literal_suffix(entries, index + 1, std::move(with_entry), dst,
                             span);
  const std::size_t jump_end =
      emit_instruction(Opcode::Jump, {{-1, true}}, entry.span);
  patch_operand(jump_skip, 1, current_pc(), false);
  compile_map_literal_suffix(entries, index + 1, std::move(prefix_entries), dst,
                             span);
  patch_operand(jump_end, 0, current_pc(), false);
}

std::uint32_t CodeEmitter::compile_lookup_like(const ast::Expr &expr,
                                               const std::string &name) {
  const std::uint32_t dst = alloc_temp();
  emit_instruction(Opcode::LookupConst,
                   {{dst, false}, {owner_->intern_lookup_path(name), false}},
                   expr.span);
  return dst;
}

std::uint32_t CodeEmitter::emit_call_site(std::uint32_t pc,
                                          const std::string &symbol_name,
                                          std::uint32_t flags) {
  const std::uint32_t site_id =
      static_cast<std::uint32_t>(code_.call_site_table.size());
  code_.call_site_table.push_back(
      {pc, site_id, owner_->intern_symbol(symbol_name), flags});
  return site_id;
}

std::uint32_t CodeEmitter::emit_ivar_site(std::uint32_t pc,
                                          const std::string &name,
                                          std::uint32_t flags) {
  const std::uint32_t site_id =
      static_cast<std::uint32_t>(code_.ivar_site_table.size());
  code_.ivar_site_table.push_back(
      {pc, site_id, owner_->intern_symbol(name), flags});
  return site_id;
}

std::uint32_t
CodeEmitter::emit_simple_send(const lexer::Span &span, std::uint32_t receiver,
                              const std::string &selector,
                              const std::vector<std::uint32_t> &pos_args) {
  std::vector<InstructionOperand> operands;
  const std::uint32_t dst = alloc_temp();
  operands.push_back({dst, false});
  operands.push_back({receiver, false});
  operands.push_back({owner_->intern_symbol(selector), false});
  operands.push_back({static_cast<std::int64_t>(pos_args.size()), false});
  for (std::uint32_t reg : pos_args) {
    operands.push_back({reg, false});
  }
  operands.push_back({0, false});
  operands.push_back({-1, true});
  const std::uint32_t site_id = emit_call_site(current_pc(), selector);
  operands.push_back({site_id, false});
  emit_instruction(Opcode::Send, std::move(operands), span);
  return dst;
}

std::uint32_t
CodeEmitter::emit_simple_call(const lexer::Span &span, std::uint32_t callee,
                              const std::vector<std::uint32_t> &pos_args) {
  std::vector<InstructionOperand> operands;
  const std::uint32_t dst = alloc_temp();
  operands.push_back({dst, false});
  operands.push_back({callee, false});
  operands.push_back({static_cast<std::int64_t>(pos_args.size()), false});
  for (std::uint32_t reg : pos_args) {
    operands.push_back({reg, false});
  }
  operands.push_back({0, false});
  operands.push_back({-1, true});
  const std::uint32_t site_id = emit_call_site(current_pc(), "<call>");
  operands.push_back({site_id, false});
  emit_instruction(Opcode::Call, std::move(operands), span);
  return dst;
}

std::uint32_t CodeEmitter::emit_load_constant_reg(std::uint32_t constant_id,
                                                  const lexer::Span &span) {
  const std::uint32_t dst = alloc_temp();
  emit_instruction(Opcode::LoadK, {{dst, false}, {constant_id, false}}, span);
  return dst;
}

void CodeEmitter::emit_type_error(const lexer::Span &span) {
  const std::uint32_t class_reg = alloc_temp();
  emit_instruction(
      Opcode::LookupConst,
      {{class_reg, false}, {owner_->intern_lookup_path("TypeError"), false}},
      span);
  const std::uint32_t error_reg = emit_simple_call(span, class_reg, {});
  emit_instruction(Opcode::Raise, {{error_reg, false}}, span);
}

std::optional<std::uint32_t>
CodeEmitter::local_slot_for_binding(const std::string &name,
                                    const lexer::Span &span) const {
  for (const hir::ProcedureLocal &local : procedure_->locals) {
    if (local.name == name && same_span(local.span, span)) {
      return parse_slot(local.slot, 'l');
    }
  }
  for (const hir::ProcedureLocal &local : procedure_->locals) {
    if (local.name == name && local.role == "pattern") {
      return parse_slot(local.slot, 'l');
    }
  }
  for (const hir::ProcedureLocal &local : procedure_->locals) {
    if (local.name == name) {
      return parse_slot(local.slot, 'l');
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t>
CodeEmitter::local_slot_for_reference(const std::string &name) const {
  for (const hir::ProcedureLocal &local : procedure_->locals) {
    if (local.name == name) {
      return parse_slot(local.slot, 'l');
    }
  }
  return std::nullopt;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>>
CodeEmitter::binding_commit_range(const ast::Expr &match_program) const {
  const ast::ListField *binding_order =
      list_field(match_program, "binding_order");
  if (binding_order == nullptr || binding_order->values.empty()) {
    return std::pair<std::uint32_t, std::uint32_t>{0, 0};
  }

  std::vector<std::uint32_t> slots;
  slots.reserve(binding_order->values.size());
  for (const std::unique_ptr<ast::Expr> &entry : binding_order->values) {
    const std::optional<std::uint32_t> slot =
        local_slot_for_binding(string_field(*entry, "name"), entry->span);
    if (!slot.has_value()) {
      return std::nullopt;
    }
    slots.push_back(*slot);
  }

  std::uint32_t min_slot = slots.front();
  std::uint32_t max_slot = slots.front();
  for (std::uint32_t slot : slots) {
    if (slot < min_slot) {
      min_slot = slot;
    }
    if (slot > max_slot) {
      max_slot = slot;
    }
  }
  if (max_slot - min_slot + 1U != slots.size()) {
    return std::nullopt;
  }
  return std::pair<std::uint32_t, std::uint32_t>{
      min_slot, static_cast<std::uint32_t>(slots.size())};
}

void CodeEmitter::add_fail_patch(std::vector<PatchRef> *fail_patches,
                                 std::size_t instruction_index,
                                 std::size_t operand_index) {
  if (fail_patches == nullptr) {
    return;
  }
  fail_patches->push_back({instruction_index, operand_index});
}

void CodeEmitter::patch_fail_patches(const std::vector<PatchRef> &fail_patches,
                                     std::uint32_t target_pc) {
  for (const PatchRef &patch : fail_patches) {
    patch_operand(patch.instruction_index, patch.operand_index, target_pc,
                  false);
  }
}

void CodeEmitter::emit_fail_jump(std::vector<PatchRef> *fail_patches,
                                 const lexer::Span &span) {
  const std::size_t jump = emit_instruction(Opcode::Jump, {{-1, true}}, span);
  add_fail_patch(fail_patches, jump, 0);
}

void CodeEmitter::compile_pattern_node(const ast::Expr &node,
                                       std::uint32_t value_reg,
                                       std::vector<PatchRef> *fail_patches) {
  if (node.kind == "PWildcard") {
    return;
  }
  if (node.kind == "PBind") {
    const std::optional<std::uint32_t> slot =
        local_slot_for_binding(string_field(node, "name"), node.span);
    if (!slot.has_value()) {
      diag(node.span, "BC2001", "pattern binding slot is missing");
      emit_fail_jump(fail_patches, node.span);
      return;
    }
    emit_instruction(Opcode::PBind, {{*slot, false}, {value_reg, false}},
                     node.span);
    return;
  }
  if (node.kind == "PPin") {
    const std::optional<std::uint32_t> slot =
        local_slot_for_reference(string_field(node, "name"));
    if (!slot.has_value()) {
      diag(node.span, "BC2001", "pin binding slot is missing");
      emit_fail_jump(fail_patches, node.span);
      return;
    }
    const std::size_t insn = emit_instruction(
        Opcode::PCheckPin, {{value_reg, false}, {*slot, false}, {-1, true}},
        node.span);
    add_fail_patch(fail_patches, insn, 2);
    return;
  }
  if (node.kind == "PLiteral") {
    const std::uint32_t const_id = owner_->intern_literal_constant(
        string_field(node, "token"), string_field(node, "value"));
    const std::size_t insn = emit_instruction(
        Opcode::PCheckEq, {{value_reg, false}, {const_id, false}, {-1, true}},
        node.span);
    add_fail_patch(fail_patches, insn, 2);
    return;
  }
  if (node.kind == "PConstMatch") {
    const std::uint32_t matcher_reg =
        compile_lookup_like(node, string_field(node, "path"));
    const std::size_t insn = emit_instruction(
        Opcode::PTripleEq,
        {{matcher_reg, false}, {value_reg, false}, {-1, true}}, node.span);
    add_fail_patch(fail_patches, insn, 2);
    return;
  }
  if (node.kind == "PMatcherExpr") {
    const ast::Expr *matcher_expr = node_field(node, "matcher_expr");
    if (matcher_expr == nullptr) {
      diag(node.span, "BC2001", "matcher-expression pattern is missing expr");
      emit_fail_jump(fail_patches, node.span);
      return;
    }
    const std::uint32_t matcher_reg = compile_expr(*matcher_expr);
    const std::size_t insn = emit_instruction(
        Opcode::PTripleEq,
        {{matcher_reg, false}, {value_reg, false}, {-1, true}}, node.span);
    add_fail_patch(fail_patches, insn, 2);
    return;
  }
  if (node.kind == "PDynamic") {
    const ast::Expr *matcher_expr = node_field(node, "matcher_expr");
    if (matcher_expr == nullptr) {
      diag(node.span, "BC2001", "dynamic pattern is missing matcher expr");
      emit_fail_jump(fail_patches, node.span);
      return;
    }

    const std::uint32_t matcher_reg = compile_expr(*matcher_expr);
    const std::uint32_t result_reg =
        emit_simple_send(node.span, matcher_reg, "match", {value_reg});
    const std::uint32_t success_reg =
        emit_simple_send(node.span, result_reg, "success", {});
    const std::size_t jump_failure = emit_instruction(
        Opcode::JumpIfFalse, {{success_reg, false}, {-1, true}}, node.span);

    if (const ast::Expr *export_map = node_field(node, "export_map_program")) {
      const std::uint32_t bindings_reg =
          emit_simple_send(node.span, result_reg, "bindings", {});
      compile_pattern_node(*export_map, bindings_reg, fail_patches);
    } else {
      const std::uint32_t bindings_reg =
          emit_simple_send(node.span, result_reg, "bindings", {});
      const std::uint32_t empty_reg =
          emit_simple_send(node.span, bindings_reg, "empty?", {});
      const std::size_t jump_empty = emit_instruction(
          Opcode::JumpIfTrue, {{empty_reg, false}, {-1, true}}, node.span);
      emit_type_error(node.span);
      patch_operand(jump_empty, 1, current_pc(), false);
    }

    const std::size_t jump_end =
        emit_instruction(Opcode::Jump, {{-1, true}}, node.span);
    patch_operand(jump_failure, 1, current_pc(), false);

    const std::uint32_t failure_bindings_reg =
        emit_simple_send(node.span, result_reg, "bindings", {});
    const std::uint32_t failure_empty_reg =
        emit_simple_send(node.span, failure_bindings_reg, "empty?", {});
    const std::size_t jump_fail =
        emit_instruction(Opcode::JumpIfTrue,
                         {{failure_empty_reg, false}, {-1, true}}, node.span);
    emit_type_error(node.span);
    patch_operand(jump_fail, 1, current_pc(), false);
    emit_fail_jump(fail_patches, node.span);
    patch_operand(jump_end, 0, current_pc(), false);
    return;
  }
  if (node.kind == "PAs") {
    const ast::Expr *inner = node_field(node, "inner");
    if (inner == nullptr) {
      diag(node.span, "BC2001", "PAs is missing inner pattern");
      emit_fail_jump(fail_patches, node.span);
      return;
    }
    compile_pattern_node(*inner, value_reg, fail_patches);
    const std::optional<std::uint32_t> slot =
        local_slot_for_binding(string_field(node, "bind_name"), node.span);
    if (!slot.has_value()) {
      diag(node.span, "BC2001", "PAs binding slot is missing");
      emit_fail_jump(fail_patches, node.span);
      return;
    }
    emit_instruction(Opcode::PBind, {{*slot, false}, {value_reg, false}},
                     node.span);
    return;
  }
  if (node.kind == "POr") {
    const ast::ListField *alternatives = list_field(node, "alternatives");
    if (alternatives == nullptr || alternatives->values.empty()) {
      diag(node.span, "BC2001", "POr is missing alternatives");
      emit_fail_jump(fail_patches, node.span);
      return;
    }
    std::vector<std::size_t> success_jumps;
    for (std::size_t i = 0; i < alternatives->values.size(); ++i) {
      std::vector<PatchRef> alt_fail_patches;
      compile_pattern_node(*alternatives->values[i], value_reg,
                           i + 1U == alternatives->values.size()
                               ? fail_patches
                               : &alt_fail_patches);
      if (i + 1U != alternatives->values.size()) {
        success_jumps.push_back(emit_instruction(
            Opcode::Jump, {{-1, true}}, alternatives->values[i]->span));
        patch_fail_patches(alt_fail_patches, current_pc());
      }
    }
    for (std::size_t jump : success_jumps) {
      patch_operand(jump, 0, current_pc(), false);
    }
    return;
  }
  if (node.kind == "PSeqTuple" || node.kind == "PSeqList") {
    const std::uint32_t seq_reg = alloc_temp();
    const std::size_t prep = emit_instruction(Opcode::PPrepSeq,
                                              {{seq_reg, false},
                                               {value_reg, false},
                                               {kPatternSeqModeDirect, false},
                                               {-1, true}},
                                              node.span);
    add_fail_patch(fail_patches, prep, 3);

    const std::uint32_t min_arity =
        parse_u32_string(string_field(node, "min_arity"));
    const std::size_t len_check = emit_instruction(
        bool_field(node, "exact_arity") ? Opcode::PCheckLenEq
                                        : Opcode::PCheckLenGte,
        {{seq_reg, false}, {min_arity, false}, {-1, true}}, node.span);
    add_fail_patch(fail_patches, len_check, 2);

    if (const ast::ListField *items = list_field(node, "items")) {
      for (const std::unique_ptr<ast::Expr> &item : items->values) {
        const ast::Expr *pattern = node_field(*item, "pattern");
        if (pattern == nullptr) {
          diag(item->span, "BC2001", "sequence item is missing pattern");
          emit_fail_jump(fail_patches, item->span);
          continue;
        }
        const std::uint32_t item_reg = alloc_temp();
        emit_instruction(
            Opcode::PGetIndex,
            {{item_reg, false},
             {seq_reg, false},
             {parse_u32_string(string_field(*item, "index")), false}},
            item->span);
        compile_pattern_node(*pattern, item_reg, fail_patches);
      }
    }
    if (bool_field(node, "capture_rest")) {
      const std::optional<std::uint32_t> slot =
          local_slot_for_binding(string_field(node, "rest_binding"), node.span);
      if (!slot.has_value()) {
        diag(node.span, "BC2001", "sequence rest binding slot is missing");
        emit_fail_jump(fail_patches, node.span);
      } else {
        emit_instruction(Opcode::PBind, {{*slot, false}, {seq_reg, false}},
                         node.span);
      }
    }
    return;
  }
  if (node.kind == "PMap") {
    const std::vector<std::string> keys = named_entries(node, "requested_keys");
    const std::uint32_t keyset_id = owner_->intern_keyset(keys);
    const std::uint32_t map_reg = alloc_temp();
    const std::size_t prep =
        emit_instruction(Opcode::PPrepMap,
                         {{map_reg, false},
                          {value_reg, false},
                          {keyset_id, false},
                          {bool_field(node, "needs_full_map") ? 1 : 0, false},
                          {-1, true}},
                         node.span);
    add_fail_patch(fail_patches, prep, 4);

    if (const ast::ListField *fields = list_field(node, "fields")) {
      for (const std::unique_ptr<ast::Expr> &field : fields->values) {
        const std::uint32_t key_id =
            owner_->intern_symbol(string_field(*field, "name"));
        const std::size_t has_key = emit_instruction(
            Opcode::PHasKey, {{map_reg, false}, {key_id, false}, {-1, true}},
            field->span);
        add_fail_patch(fail_patches, has_key, 2);
        const ast::Expr *pattern = node_field(*field, "pattern");
        if (pattern == nullptr) {
          diag(field->span, "BC2001", "map field is missing pattern");
          emit_fail_jump(fail_patches, field->span);
          continue;
        }
        const std::uint32_t field_reg = alloc_temp();
        emit_instruction(
            Opcode::PGetKey,
            {{field_reg, false}, {map_reg, false}, {key_id, false}},
            field->span);
        compile_pattern_node(*pattern, field_reg, fail_patches);
      }
    }
    if (bool_field(node, "capture_rest")) {
      const std::optional<std::uint32_t> slot =
          local_slot_for_binding(string_field(node, "rest_binding"), node.span);
      if (!slot.has_value()) {
        diag(node.span, "BC2001", "map rest binding slot is missing");
        emit_fail_jump(fail_patches, node.span);
      } else {
        emit_instruction(Opcode::PBind, {{*slot, false}, {map_reg, false}},
                         node.span);
      }
    }
    return;
  }
  if (node.kind == "PHead") {
    const std::uint32_t matcher_reg =
        compile_lookup_like(node, string_field(node, "head"));
    const std::size_t head_check = emit_instruction(
        Opcode::PTripleEq,
        {{matcher_reg, false}, {value_reg, false}, {-1, true}}, node.span);
    add_fail_patch(fail_patches, head_check, 2);

    const std::string mode = string_field(node, "destructure_mode");
    if (mode == "POSITIONAL" || mode == "MIXED") {
      const std::uint32_t seq_source =
          emit_simple_send(node.span, value_reg, "deconstruct", {});
      const std::uint32_t seq_reg = alloc_temp();
      const std::size_t prep =
          emit_instruction(Opcode::PPrepSeq,
                           {{seq_reg, false},
                            {seq_source, false},
                            {kPatternSeqModeDeconstruct, false},
                            {-1, true}},
                           node.span);
      add_fail_patch(fail_patches, prep, 3);

      const ast::ListField *pos_args = list_field(node, "pos_args");
      const std::uint32_t count =
          pos_args == nullptr
              ? 0U
              : static_cast<std::uint32_t>(pos_args->values.size());
      const std::size_t len_check = emit_instruction(
          Opcode::PCheckLenEq, {{seq_reg, false}, {count, false}, {-1, true}},
          node.span);
      add_fail_patch(fail_patches, len_check, 2);

      if (pos_args != nullptr) {
        for (const std::unique_ptr<ast::Expr> &arg : pos_args->values) {
          const ast::Expr *pattern = node_field(*arg, "pattern");
          if (pattern == nullptr) {
            diag(arg->span, "BC2001",
                 "head positional item is missing pattern");
            emit_fail_jump(fail_patches, arg->span);
            continue;
          }
          const std::uint32_t item_reg = alloc_temp();
          emit_instruction(
              Opcode::PGetIndex,
              {{item_reg, false},
               {seq_reg, false},
               {parse_u32_string(string_field(*arg, "index")), false}},
              arg->span);
          compile_pattern_node(*pattern, item_reg, fail_patches);
        }
      }
    }
    if (mode == "KEYS" || mode == "MIXED") {
      const std::vector<std::string> keys =
          named_entries(node, "requested_keys");
      const std::uint32_t keyset_id = owner_->intern_keyset(keys);
      const std::uint32_t keyset_reg =
          emit_load_constant_reg(keyset_id, node.span);
      const std::uint32_t map_source = emit_simple_send(
          node.span, value_reg, "deconstruct_keys", {keyset_reg});
      const std::uint32_t map_reg = alloc_temp();
      const std::size_t prep = emit_instruction(Opcode::PPrepMap,
                                                {{map_reg, false},
                                                 {map_source, false},
                                                 {keyset_id, false},
                                                 {0, false},
                                                 {-1, true}},
                                                node.span);
      add_fail_patch(fail_patches, prep, 4);

      if (const ast::ListField *fields = list_field(node, "kw_fields")) {
        for (const std::unique_ptr<ast::Expr> &field : fields->values) {
          const std::uint32_t key_id =
              owner_->intern_symbol(string_field(*field, "name"));
          const std::size_t has_key = emit_instruction(
              Opcode::PHasKey, {{map_reg, false}, {key_id, false}, {-1, true}},
              field->span);
          add_fail_patch(fail_patches, has_key, 2);
          const ast::Expr *pattern = node_field(*field, "pattern");
          if (pattern == nullptr) {
            diag(field->span, "BC2001", "head key field is missing pattern");
            emit_fail_jump(fail_patches, field->span);
            continue;
          }
          const std::uint32_t field_reg = alloc_temp();
          emit_instruction(
              Opcode::PGetKey,
              {{field_reg, false}, {map_reg, false}, {key_id, false}},
              field->span);
          compile_pattern_node(*pattern, field_reg, fail_patches);
        }
      }
    }
    return;
  }

  diag(node.span, "BC2007",
       "unsupported match-program node in bytecode emitter");
  emit_fail_jump(fail_patches, node.span);
}

std::uint32_t CodeEmitter::block_reg_operand(const ast::Expr &expr) {
  const ast::Expr *block = node_field(expr, "block");
  if (block == nullptr) {
    return static_cast<std::uint32_t>(-1);
  }
  return compile_expr(*block);
}

std::uint32_t CodeEmitter::compile_send_like(const ast::Expr &expr,
                                             Opcode opcode) {
  std::vector<InstructionOperand> operands;
  const std::uint32_t dst = alloc_temp();
  operands.push_back({dst, false});

  if (opcode == Opcode::Send) {
    const ast::Expr *receiver = node_field(expr, "receiver");
    if (receiver == nullptr) {
      diag(expr.span, "BC2001", "HSend is missing receiver");
      return dst;
    }
    const std::uint32_t recv_reg = compile_expr(*receiver);
    operands.push_back({recv_reg, false});
    operands.push_back(
        {owner_->intern_symbol(string_field(expr, "selector")), false});
  } else if (opcode == Opcode::SendDyn) {
    const ast::Expr *receiver = node_field(expr, "receiver");
    const ast::Expr *selector_expr = node_field(expr, "selector_expr");
    if (receiver == nullptr || selector_expr == nullptr) {
      diag(expr.span, "BC2001", "HSendDyn is missing receiver or selector");
      return dst;
    }
    operands.push_back({compile_expr(*receiver), false});
    operands.push_back({compile_expr(*selector_expr), false});
  } else if (opcode == Opcode::Call) {
    const ast::Expr *callable = node_field(expr, "callable");
    if (callable == nullptr) {
      diag(expr.span, "BC2001", "HCall is missing callable");
      return dst;
    }
    operands.push_back({compile_expr(*callable), false});
  }

  std::vector<std::uint32_t> pos_regs;
  const ast::ListField *pos_args = list_field(expr, "pos_args");
  if (pos_args != nullptr) {
    for (const std::unique_ptr<ast::Expr> &arg : pos_args->values) {
      pos_regs.push_back(compile_expr(*arg));
    }
  }
  operands.push_back({static_cast<std::int64_t>(pos_regs.size()), false});
  for (std::uint32_t reg : pos_regs) {
    operands.push_back({reg, false});
  }

  std::vector<std::pair<std::uint32_t, std::uint32_t>> kw_regs;
  const ast::ListField *kw_args = list_field(expr, "kw_args");
  if (kw_args != nullptr) {
    for (const std::unique_ptr<ast::Expr> &arg : kw_args->values) {
      const ast::Expr *value = node_field(*arg, "value");
      if (arg->kind != "HKeywordArg" || value == nullptr) {
        diag(arg->span, "BC2001", "invalid keyword arg in bytecode emitter");
        continue;
      }
      kw_regs.push_back({owner_->intern_symbol(string_field(*arg, "name")),
                         compile_expr(*value)});
    }
  }
  operands.push_back({static_cast<std::int64_t>(kw_regs.size()), false});
  for (const auto &[symbol_id, reg] : kw_regs) {
    operands.push_back({symbol_id, false});
    operands.push_back({reg, false});
  }

  operands.push_back({block_reg_operand(expr), true});
  const std::uint32_t pc = current_pc();
  const std::string site_symbol =
      opcode == Opcode::Send
          ? string_field(expr, "selector")
          : (opcode == Opcode::SendDyn ? "<dynamic>" : "<call>");
  const std::uint32_t site_id = emit_call_site(pc, site_symbol);
  operands.push_back({site_id, false});
  emit_instruction(opcode, std::move(operands), expr.span);
  return dst;
}

std::uint32_t
CodeEmitter::compile_closure(const ast::Expr &expr,
                             std::optional<std::uint32_t> target_reg) {
  const std::string procedure_id = string_field(expr, "procedure");
  const std::optional<std::uint32_t> code_id =
      owner_->code_id_for_procedure(procedure_id);
  const std::uint32_t dst = target_reg.value_or(alloc_temp());
  if (!code_id.has_value()) {
    diag(expr.span, "BC2003", "missing closure procedure in bytecode emitter");
    return dst;
  }

  std::vector<InstructionOperand> operands;
  operands.push_back({dst, false});
  operands.push_back({*code_id, false});
  const ast::ListField *captures = list_field(expr, "captures");
  const std::size_t capture_count =
      captures == nullptr ? 0U : captures->values.size();
  operands.push_back({static_cast<std::int64_t>(capture_count), false});
  if (captures != nullptr) {
    for (const std::unique_ptr<ast::Expr> &capture : captures->values) {
      const std::string source_kind = string_field(*capture, "source_kind");
      const std::string source_slot = string_field(*capture, "source_slot");
      const CaptureSourceKind kind = source_kind == "capture"
                                         ? CaptureSourceKind::Capture
                                         : CaptureSourceKind::Local;
      operands.push_back({static_cast<std::int64_t>(kind), false});
      operands.push_back(
          {static_cast<std::int64_t>(parse_slot(
               source_slot, kind == CaptureSourceKind::Capture ? 'u' : 'l')),
           false});
    }
  }
  emit_instruction(Opcode::MakeClosure, std::move(operands), expr.span);
  return dst;
}

std::uint32_t CodeEmitter::compile_cond_source(const ast::Expr &cond,
                                               Opcode *jump_opcode,
                                               bool *jump_to_then_branch) {
  if (cond.kind == "HIsNull") {
    const ast::Expr *inner = node_field(cond, "expr");
    if (inner == nullptr) {
      diag(cond.span, "BC2001", "HIsNull is missing expr");
      *jump_opcode = Opcode::JumpIfFalse;
      *jump_to_then_branch = false;
      return alloc_temp();
    }
    *jump_opcode = Opcode::JumpIfNull;
    *jump_to_then_branch = true;
    return compile_expr(*inner);
  }
  *jump_opcode = Opcode::JumpIfFalse;
  *jump_to_then_branch = false;
  return compile_expr(cond);
}

std::uint32_t CodeEmitter::compile_if(const ast::Expr &expr) {
  const ast::Expr *cond = node_field(expr, "cond");
  const ast::Expr *then_body = node_field(expr, "then_body");
  const ast::Expr *else_body = node_field(expr, "else_body");
  const std::uint32_t dst = alloc_temp();
  if (cond == nullptr || then_body == nullptr || else_body == nullptr) {
    diag(expr.span, "BC2001", "HIf is missing child nodes");
    return dst;
  }

  Opcode cond_jump = Opcode::JumpIfFalse;
  bool jump_to_then = false;
  const std::uint32_t cond_reg =
      compile_cond_source(*cond, &cond_jump, &jump_to_then);
  if (jump_to_then) {
    const std::size_t jump_then = emit_instruction(
        cond_jump, {{cond_reg, false}, {-1, true}}, cond->span);
    compile_seq(*else_body);
    const std::size_t jump_end =
        emit_instruction(Opcode::Jump, {{-1, true}}, expr.span);
    patch_operand(jump_then, 1, current_pc(), false);
    compile_seq(*then_body);
    patch_operand(jump_end, 0, current_pc(), false);
  } else {
    const std::size_t jump_else = emit_instruction(
        cond_jump, {{cond_reg, false}, {-1, true}}, cond->span);
    compile_seq(*then_body);
    const std::size_t jump_end =
        emit_instruction(Opcode::Jump, {{-1, true}}, expr.span);
    patch_operand(jump_else, 1, current_pc(), false);
    compile_seq(*else_body);
    patch_operand(jump_end, 0, current_pc(), false);
  }
  emit_instruction(Opcode::GetLast, {{dst, false}}, expr.span);
  return dst;
}

std::uint32_t CodeEmitter::compile_logical(const ast::Expr &expr) {
  const ast::Expr *left = node_field(expr, "left");
  const ast::Expr *right = node_field(expr, "right");
  const std::uint32_t dst = alloc_temp();
  if (left == nullptr || right == nullptr) {
    diag(expr.span, "BC2001", "HLogical is missing child nodes");
    return dst;
  }

  const std::uint32_t left_reg = compile_expr(*left);
  emit_instruction(Opcode::Move, {{dst, false}, {left_reg, false}}, left->span);
  const Opcode jump_opcode = string_field(expr, "op") == "or"
                                 ? Opcode::JumpIfTrue
                                 : Opcode::JumpIfFalse;
  const std::size_t jump_end =
      emit_instruction(jump_opcode, {{left_reg, false}, {-1, true}}, expr.span);
  const std::uint32_t right_reg = compile_expr(*right);
  if (right_reg != dst) {
    emit_instruction(Opcode::Move, {{dst, false}, {right_reg, false}},
                     right->span);
  }
  patch_operand(jump_end, 1, current_pc(), false);
  return dst;
}

std::uint32_t CodeEmitter::compile_loop(const ast::Expr &expr) {
  const std::string kind = string_field(expr, "kind");
  const ast::Expr *cond = node_field(expr, "cond");
  const ast::Expr *body = node_field(expr, "body");
  const std::uint32_t dst = alloc_temp();
  if (body == nullptr) {
    diag(expr.span, "BC2001", "HLoop is missing body");
    return dst;
  }

  const std::uint32_t header_pc = current_pc();
  code_.safepoint_table.push_back({header_pc, 0});
  emit_instruction(Opcode::Safepoint, {}, expr.span);

  if (kind == "while" || kind == "until") {
    if (cond == nullptr) {
      diag(expr.span, "BC2001", "conditional loop is missing cond");
      return dst;
    }
    const std::uint32_t cond_reg = compile_expr(*cond);
    const Opcode jump_opcode =
        kind == "while" ? Opcode::JumpIfFalse : Opcode::JumpIfTrue;
    const std::size_t jump_exit = emit_instruction(
        jump_opcode, {{cond_reg, false}, {-1, true}}, cond->span);
    loops_.push_back({});
    compile_seq(*body);
    emit_instruction(Opcode::Jump, {{header_pc, false}}, expr.span);
    const std::uint32_t exit_pc = current_pc();
    patch_operand(jump_exit, 1, exit_pc, false);
    for (std::size_t patch : loops_.back().break_jump_indices) {
      patch_operand(patch, 0, exit_pc, false);
    }
    loops_.pop_back();
  } else if (kind == "loop") {
    loops_.push_back({});
    compile_seq(*body);
    emit_instruction(Opcode::Jump, {{header_pc, false}}, expr.span);
    const std::uint32_t exit_pc = current_pc();
    for (std::size_t patch : loops_.back().break_jump_indices) {
      patch_operand(patch, 0, exit_pc, false);
    }
    loops_.pop_back();
  } else if (kind == "do_while") {
    loops_.push_back({});
    compile_seq(*body);
    if (cond == nullptr) {
      diag(expr.span, "BC2001", "do_while loop is missing cond");
    } else {
      const std::uint32_t cond_reg = compile_expr(*cond);
      emit_instruction(Opcode::JumpIfTrue,
                       {{cond_reg, false}, {header_pc, false}}, cond->span);
    }
    const std::uint32_t exit_pc = current_pc();
    for (std::size_t patch : loops_.back().break_jump_indices) {
      patch_operand(patch, 0, exit_pc, false);
    }
    loops_.pop_back();
  } else {
    diag(expr.span, "BC2001", "unsupported loop kind in bytecode emitter");
  }

  emit_instruction(Opcode::GetLast, {{dst, false}}, expr.span);
  return dst;
}

std::uint32_t CodeEmitter::compile_match_dispatch(const ast::Expr &expr) {
  const ast::Expr *scrutinee = node_field(expr, "scrutinee");
  if (scrutinee == nullptr) {
    diag(expr.span, "BC2001", "HMatchDispatch is missing scrutinee");
    return alloc_temp();
  }

  const std::uint32_t scrutinee_reg = compile_expr(*scrutinee);
  const std::uint32_t dst = alloc_temp();
  std::vector<std::size_t> end_jumps;

  const ast::ListField *arms = list_field(expr, "arms");
  if (arms != nullptr) {
    for (const std::unique_ptr<ast::Expr> &arm : arms->values) {
      const ast::Expr *compiled_pattern = node_field(*arm, "compiled_pattern");
      const ast::Expr *body = node_field(*arm, "body");
      if (compiled_pattern == nullptr || body == nullptr) {
        diag(arm->span, "BC2001",
             "match arm is missing compiled pattern or body");
        continue;
      }

      owner_->register_pattern_program(*compiled_pattern);
      const ast::Expr *match_program =
          node_field(*compiled_pattern, "match_program");
      if (match_program == nullptr) {
        diag(arm->span, "BC2001", "compiled pattern is missing match_program");
        continue;
      }
      const ast::Expr *root = node_field(*match_program, "root");
      if (root == nullptr) {
        diag(arm->span, "BC2001", "match_program is missing root");
        continue;
      }

      std::vector<PatchRef> fail_patches;
      compile_pattern_node(*root, scrutinee_reg, &fail_patches);

      if (bool_field(*match_program, "requires_commit")) {
        const std::optional<std::pair<std::uint32_t, std::uint32_t>> commit =
            binding_commit_range(*match_program);
        if (!commit.has_value()) {
          diag(arm->span, "BC2001",
               "match arm bindings are not representable as one commit range");
        } else if (commit->second != 0U) {
          emit_instruction(Opcode::PCommit,
                           {{commit->first, false}, {commit->second, false}},
                           arm->span);
        }
      }

      std::vector<PatchRef> next_arm_patches = fail_patches;
      if (const ast::Expr *guard = node_field(*arm, "guard")) {
        const std::uint32_t guard_reg = compile_expr(*guard);
        const std::size_t jump_next = emit_instruction(
            Opcode::JumpIfFalse, {{guard_reg, false}, {-1, true}}, guard->span);
        next_arm_patches.push_back({jump_next, 1});
      }

      compile_seq(*body);
      emit_instruction(Opcode::GetLast, {{dst, false}}, arm->span);
      end_jumps.push_back(
          emit_instruction(Opcode::Jump, {{-1, true}}, arm->span));
      patch_fail_patches(next_arm_patches, current_pc());
    }
  }

  const ast::Expr *else_body = node_field(expr, "else_body");
  if (else_body != nullptr && !is_empty_seq_expr(else_body)) {
    compile_seq(*else_body);
    emit_instruction(Opcode::GetLast, {{dst, false}}, else_body->span);
  } else if (string_field(expr, "fail_mode") == "match_error") {
    emit_instruction(Opcode::PFail, {{kPatternFailModeMatchError, false}},
                     expr.span);
  } else {
    emit_instruction(Opcode::LoadNull, {{dst, false}}, expr.span);
    emit_instruction(Opcode::SetLast, {{dst, false}}, expr.span);
  }

  for (std::size_t jump : end_jumps) {
    patch_operand(jump, 0, current_pc(), false);
  }
  return dst;
}

std::uint32_t CodeEmitter::compile_clause_dispatch(const ast::Expr &expr) {
  const std::uint32_t dst = alloc_temp();
  std::vector<std::size_t> end_jumps;

  if (const ast::ListField *clauses = list_field(expr, "clauses")) {
    for (const std::unique_ptr<ast::Expr> &clause : clauses->values) {
      if (clause == nullptr) {
        continue;
      }
      const ast::Expr *compiled_pattern =
          node_field(*clause, "compiled_pattern");
      const ast::Expr *match_program =
          compiled_pattern == nullptr
              ? nullptr
              : node_field(*compiled_pattern, "match_program");
      const ast::Expr *root =
          match_program == nullptr ? nullptr : node_field(*match_program,
                                                          "root");
      const ast::Expr *body = node_field(*clause, "body");
      if (compiled_pattern == nullptr || match_program == nullptr ||
          root == nullptr || body == nullptr) {
        diag(clause->span, "BC2001",
             "clause dispatch is missing pattern or body");
        continue;
      }

      std::vector<PatchRef> fail_patches;
      const std::uint32_t subject_reg = compile_clause_subject(*clause);
      compile_pattern_node(*root, subject_reg, &fail_patches);

      if (bool_field(*match_program, "requires_commit")) {
        const std::optional<std::pair<std::uint32_t, std::uint32_t>> commit =
            binding_commit_range(*match_program);
        if (!commit.has_value()) {
          diag(clause->span, "BC2001",
               "clause bindings are not representable as one commit range");
        } else if (commit->second != 0U) {
          emit_instruction(Opcode::PCommit,
                           {{commit->first, false}, {commit->second, false}},
                           clause->span);
        }
      }

      std::vector<PatchRef> next_clause_patches = fail_patches;
      if (const ast::Expr *guard = node_field(*clause, "guard")) {
        const std::uint32_t guard_reg = compile_expr(*guard);
        const std::size_t jump_next =
            emit_instruction(Opcode::JumpIfFalse,
                             {{guard_reg, false}, {-1, true}}, guard->span);
        next_clause_patches.push_back({jump_next, 1});
      }

      compile_seq(*body);
      emit_instruction(Opcode::GetLast, {{dst, false}}, body->span);
      end_jumps.push_back(
          emit_instruction(Opcode::Jump, {{-1, true}}, clause->span));
      patch_fail_patches(next_clause_patches, current_pc());
    }
  }

  const ast::Expr *else_body = node_field(expr, "else_body");
  if (else_body != nullptr && !is_empty_seq_expr(else_body)) {
    compile_seq(*else_body);
    emit_instruction(Opcode::GetLast, {{dst, false}}, else_body->span);
  } else {
    emit_instruction(Opcode::LoadNull, {{dst, false}}, expr.span);
    emit_instruction(Opcode::SetLast, {{dst, false}}, expr.span);
  }

  for (std::size_t jump : end_jumps) {
    patch_operand(jump, 0, current_pc(), false);
  }
  return dst;
}

std::uint32_t CodeEmitter::compile_pattern_assign(const ast::Expr &expr) {
  const ast::Expr *compiled_pattern = node_field(expr, "compiled_pattern");
  const ast::Expr *value = node_field(expr, "value");
  if (compiled_pattern == nullptr || value == nullptr) {
    diag(expr.span, "BC2001",
         "HPatternAssign is missing compiled_pattern or value");
    return alloc_temp();
  }

  owner_->register_pattern_program(*compiled_pattern);
  const ast::Expr *match_program =
      node_field(*compiled_pattern, "match_program");
  const ast::Expr *root =
      match_program == nullptr ? nullptr : node_field(*match_program, "root");
  if (match_program == nullptr || root == nullptr) {
    diag(expr.span, "BC2001", "pattern assignment match_program is malformed");
    return alloc_temp();
  }

  const std::uint32_t rhs_reg = compile_expr(*value);
  std::vector<PatchRef> fail_patches;
  compile_pattern_node(*root, rhs_reg, &fail_patches);
  if (bool_field(*match_program, "requires_commit")) {
    const std::optional<std::pair<std::uint32_t, std::uint32_t>> commit =
        binding_commit_range(*match_program);
    if (!commit.has_value()) {
      diag(expr.span, "BC2001",
           "pattern assignment bindings are not representable as one commit "
           "range");
    } else if (commit->second != 0U) {
      emit_instruction(Opcode::PCommit,
                       {{commit->first, false}, {commit->second, false}},
                       expr.span);
    }
  }
  emit_instruction(Opcode::SetLast, {{rhs_reg, false}}, expr.span);
  const std::size_t jump_end =
      emit_instruction(Opcode::Jump, {{-1, true}}, expr.span);
  patch_fail_patches(fail_patches, current_pc());
  emit_instruction(Opcode::PFail, {{kPatternFailModeMatchError, false}},
                   expr.span);
  patch_operand(jump_end, 0, current_pc(), false);
  return rhs_reg;
}

std::uint32_t CodeEmitter::compile_expr(const ast::Expr &expr) {
  if (expr.kind == "HConst") {
    return compile_const(expr);
  }
  if (expr.kind == "HListLiteral") {
    return compile_sequence_literal(expr, Opcode::MakeList);
  }
  if (expr.kind == "HTupleLiteral") {
    return compile_sequence_literal(expr, Opcode::MakeTuple);
  }
  if (expr.kind == "HSetLiteral") {
    return compile_sequence_literal(expr, Opcode::MakeSet);
  }
  if (expr.kind == "HMapLiteral") {
    return compile_map_literal(expr);
  }
  if (expr.kind == "HLoadLocal") {
    return parse_slot(string_field(expr, "slot"), 'l');
  }
  if (expr.kind == "HLastGet") {
    const std::uint32_t dst = alloc_temp();
    emit_instruction(Opcode::GetLast, {{dst, false}}, expr.span);
    return dst;
  }
  if (expr.kind == "HLoadCapture") {
    const std::uint32_t dst = alloc_temp();
    emit_instruction(
        Opcode::LoadUpval,
        {{dst, false}, {parse_slot(string_field(expr, "slot"), 'u'), false}},
        expr.span);
    return dst;
  }
  if (expr.kind == "HLoadName") {
    return compile_lookup_like(expr, string_field(expr, "name"));
  }
  if (expr.kind == "HLoadConst") {
    return compile_lookup_like(expr, string_field(expr, "path"));
  }
  if (expr.kind == "HLoadIvar") {
    const std::uint32_t self_reg = alloc_temp();
    emit_instruction(Opcode::LoadSelf, {{self_reg, false}}, expr.span);
    const std::uint32_t dst = alloc_temp();
    const std::uint32_t site_id =
        emit_ivar_site(current_pc(), string_field(expr, "name"));
    emit_instruction(
        Opcode::LoadIvar,
        {{dst, false},
         {self_reg, false},
         {owner_->intern_symbol(string_field(expr, "name")), false},
         {site_id, false}},
        expr.span);
    return dst;
  }
  if (expr.kind == "HLoadCvar") {
    const std::uint32_t self_reg = alloc_temp();
    emit_instruction(Opcode::LoadSelf, {{self_reg, false}}, expr.span);
    const std::uint32_t dst = alloc_temp();
    emit_instruction(
        Opcode::LoadCvar,
        {{dst, false},
         {self_reg, false},
         {owner_->intern_symbol(string_field(expr, "name")), false}},
        expr.span);
    return dst;
  }
  if (expr.kind == "HStoreLocal") {
    const ast::Expr *value = node_field(expr, "expr");
    const std::uint32_t slot = parse_slot(string_field(expr, "slot"), 'l');
    if (value == nullptr) {
      diag(expr.span, "BC2001", "HStoreLocal is missing expr");
      return slot;
    }
    if (value->kind == "HClosure") {
      return compile_closure(*value, slot);
    }
    const std::uint32_t src = compile_expr(*value);
    if (src != slot) {
      emit_instruction(Opcode::Move, {{slot, false}, {src, false}}, expr.span);
    }
    return slot;
  }
  if (expr.kind == "HStoreCapture") {
    const ast::Expr *value = node_field(expr, "expr");
    const std::uint32_t slot = parse_slot(string_field(expr, "slot"), 'u');
    if (value == nullptr) {
      diag(expr.span, "BC2001", "HStoreCapture is missing expr");
      return alloc_temp();
    }
    const std::uint32_t src = compile_expr(*value);
    emit_instruction(Opcode::StoreUpval, {{slot, false}, {src, false}},
                     expr.span);
    return src;
  }
  if (expr.kind == "HStoreIvar") {
    const ast::Expr *value = node_field(expr, "expr");
    if (value == nullptr) {
      diag(expr.span, "BC2001", "HStoreIvar is missing expr");
      return alloc_temp();
    }
    const std::uint32_t self_reg = alloc_temp();
    emit_instruction(Opcode::LoadSelf, {{self_reg, false}}, expr.span);
    const std::uint32_t src = compile_expr(*value);
    const std::uint32_t site_id =
        emit_ivar_site(current_pc(), string_field(expr, "name"));
    emit_instruction(
        Opcode::StoreIvar,
        {{self_reg, false},
         {owner_->intern_symbol(string_field(expr, "name")), false},
         {src, false},
         {site_id, false}},
        expr.span);
    return src;
  }
  if (expr.kind == "HStoreCvar") {
    const ast::Expr *value = node_field(expr, "expr");
    if (value == nullptr) {
      diag(expr.span, "BC2001", "HStoreCvar is missing expr");
      return alloc_temp();
    }
    const std::uint32_t self_reg = alloc_temp();
    emit_instruction(Opcode::LoadSelf, {{self_reg, false}}, expr.span);
    const std::uint32_t src = compile_expr(*value);
    emit_instruction(
        Opcode::StoreCvar,
        {{self_reg, false},
         {owner_->intern_symbol(string_field(expr, "name")), false},
         {src, false}},
        expr.span);
    return src;
  }
  if (expr.kind == "HSend") {
    return compile_send_like(expr, Opcode::Send);
  }
  if (expr.kind == "HSendDyn") {
    return compile_send_like(expr, Opcode::SendDyn);
  }
  if (expr.kind == "HCall") {
    return compile_send_like(expr, Opcode::Call);
  }
  if (expr.kind == "HIndex") {
    const ast::Expr *receiver = node_field(expr, "receiver");
    const ast::Expr *index_expr = node_field(expr, "index_expr");
    const std::uint32_t dst = alloc_temp();
    if (receiver == nullptr || index_expr == nullptr) {
      diag(expr.span, "BC2001", "HIndex is missing receiver or index_expr");
      return dst;
    }
    std::vector<InstructionOperand> operands;
    operands.push_back({dst, false});
    operands.push_back({compile_expr(*receiver), false});
    operands.push_back({owner_->intern_symbol("[]"), false});
    operands.push_back({1, false});
    operands.push_back({compile_expr(*index_expr), false});
    operands.push_back({0, false});
    operands.push_back({-1, true});
    const std::uint32_t site_id = emit_call_site(current_pc(), "[]");
    operands.push_back({site_id, false});
    emit_instruction(Opcode::Send, std::move(operands), expr.span);
    return dst;
  }
  if (expr.kind == "HClosure") {
    return compile_closure(expr);
  }
  if (expr.kind == "HIf") {
    return compile_if(expr);
  }
  if (expr.kind == "HLogical") {
    return compile_logical(expr);
  }
  if (expr.kind == "HLoop") {
    return compile_loop(expr);
  }
  if (expr.kind == "HMatchDispatch") {
    return compile_match_dispatch(expr);
  }
  if (expr.kind == "HClauseDispatch") {
    return compile_clause_dispatch(expr);
  }
  if (expr.kind == "HPatternAssign") {
    return compile_pattern_assign(expr);
  }
  if (expr.kind == "HBreak") {
    if (loops_.empty()) {
      diag(expr.span, "BC2001", "break outside loop in bytecode emitter");
      return alloc_temp();
    }
    if (const ast::Expr *value = node_field(expr, "value")) {
      const std::uint32_t reg = compile_expr(*value);
      emit_instruction(Opcode::SetLast, {{reg, false}}, expr.span);
    }
    loops_.back().break_jump_indices.push_back(
        emit_instruction(Opcode::Jump, {{-1, true}}, expr.span));
    return alloc_temp();
  }
  if (expr.kind == "HIsNull") {
    diag(expr.span, "BC2001",
         "HIsNull should be consumed by conditional lowering before emission");
    return alloc_temp();
  }

  diag(expr.span, "BC2001", "unsupported HIR expression in bytecode emitter");
  return alloc_temp();
}

} // namespace

EmitResult emit_program(const hir::Program &program,
                        const std::string &module_name) {
  Emitter emitter(&program, module_name);
  return emitter.emit();
}

} // namespace amber::bytecode
