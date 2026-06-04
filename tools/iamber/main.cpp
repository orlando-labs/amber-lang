#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/binder/binder.h"
#include "frontend/checker/checker.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/module_loader.h"
#include "runtime/vm.h"

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <curses.h>

namespace {

struct LocalView {
  std::string name;
  std::string role;
  std::string binding_kind;
  std::string value;
  bool initialized = false;
  bool watched = false;
  std::uint64_t watch_cell_id = 0;
  std::uint64_t watch_revision = 0;
};

struct CodeErrorRange {
  int start_line = 0;
  int start_column = 0;
  int end_line = 0;
  int end_column = 0;
  bool whole_line = false;
};

struct SourceErrorRange {
  std::string file;
  std::size_t start_line = 0;
  std::size_t start_column = 0;
  std::size_t end_line = 0;
  std::size_t end_column = 0;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  bool has_offsets = false;
  bool whole_line = false;
};

struct CellErrorRange {
  std::size_t cell_index = 0;
  CodeErrorRange range;
};

struct CellErrorView {
  std::string message;
  CodeErrorRange range;
  bool has_range = false;
};

struct Cell {
  std::string source;
  std::size_t cursor = 0;
  bool watch = true;
  bool dirty = true;
  bool running = false;
  bool ok = false;
  std::string result = "not evaluated";
  std::string error;
  std::vector<LocalView> locals;
  std::vector<amber::runtime::RuntimeTextOutputEvent> output_events;
  std::uint64_t watch_epoch = 0;
  std::size_t watch_event_count = 0;
  std::vector<CodeErrorRange> error_ranges;
  std::vector<CellErrorView> errors;
  std::size_t selected_error = 0;
};

struct Session {
  std::vector<Cell> cells;
  std::size_t selected = 0;
  int editor_scroll = 0;
  int cell_scroll = 0;
  int preferred_column = 0;
  bool auto_watch = true;
  std::string status = "iamber ready";
};

struct CompileResult {
  bool ok = false;
  amber::bytecode::BcModule module;
  std::string error;
  std::vector<SourceErrorRange> error_ranges;
  std::string module_name;
};

struct EvalView {
  bool ok = false;
  std::string result;
  std::string error;
  std::vector<LocalView> locals;
  std::vector<amber::runtime::RuntimeTextOutputEvent> output_events;
  std::uint64_t watch_epoch = 0;
  std::size_t watch_event_count = 0;
  std::vector<CellErrorRange> error_ranges;
};

struct CellDependencyInfo {
  std::set<std::string> reads;
  std::set<std::string> writes;
};

struct DependencyScanState {
  CellDependencyInfo *info = nullptr;
  std::set<std::string> local_bindings;
};

constexpr short kBorderEditColor = 1;
constexpr short kBorderErrorColor = 2;
constexpr short kBorderRunningColor = 3;
constexpr short kFooterKeyColor = 4;
constexpr short kFooterLabelColor = 5;
constexpr short kFooterStatusColor = 6;
constexpr short kLineNumberColor = 7;
constexpr short kErrorHighlightColor = 8;
constexpr short kSyntaxKeywordColor = 9;
constexpr short kSyntaxLiteralColor = 10;
constexpr short kSyntaxStringColor = 11;
constexpr short kSyntaxOperatorColor = 12;
constexpr short kSyntaxCommentColor = 13;
constexpr short kSyntaxSpecialColor = 14;

enum class SyntaxStyle {
  Plain,
  Keyword,
  Literal,
  String,
  Operator,
  Comment,
  Special,
};

struct SyntaxSpan {
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  SyntaxStyle style = SyntaxStyle::Plain;
};

std::string read_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open input file: " + path);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

amber::lexer::LexResult lex_source(const std::string &source,
                                   const std::string &path) {
  amber::lexer::Lexer lexer(source, path);
  return lexer.lex();
}

bool is_keyword_token(amber::lexer::TokenKind kind) {
  return static_cast<int>(kind) >=
             static_cast<int>(amber::lexer::TokenKind::KeywordAnd) &&
         static_cast<int>(kind) <=
             static_cast<int>(amber::lexer::TokenKind::KeywordWhile);
}

bool is_literal_token(amber::lexer::TokenKind kind) {
  switch (kind) {
  case amber::lexer::TokenKind::Integer:
  case amber::lexer::TokenKind::Float:
  case amber::lexer::TokenKind::KeywordFalse:
  case amber::lexer::TokenKind::KeywordNull:
  case amber::lexer::TokenKind::KeywordTrue:
    return true;
  default:
    return false;
  }
}

SyntaxStyle syntax_style_for_token(amber::lexer::TokenKind kind) {
  switch (kind) {
  case amber::lexer::TokenKind::Eof:
  case amber::lexer::TokenKind::Newline:
  case amber::lexer::TokenKind::Indent:
  case amber::lexer::TokenKind::Dedent:
  case amber::lexer::TokenKind::Identifier:
    return SyntaxStyle::Plain;
  default:
    break;
  }

  if (kind == amber::lexer::TokenKind::String) {
    return SyntaxStyle::String;
  }
  if (kind == amber::lexer::TokenKind::Placeholder ||
      kind == amber::lexer::TokenKind::LastValue) {
    return SyntaxStyle::Special;
  }
  if (is_literal_token(kind)) {
    return SyntaxStyle::Literal;
  }
  if (is_keyword_token(kind)) {
    return SyntaxStyle::Keyword;
  }
  return SyntaxStyle::Operator;
}

bool comment_starts_at_for_highlight(const std::string &source,
                                     std::size_t offset) {
  if (offset >= source.size() || source[offset] != '#') {
    return false;
  }
  if (offset == 0U) {
    return true;
  }

  std::size_t line_start = offset;
  while (line_start > 0U && source[line_start - 1U] != '\n' &&
         source[line_start - 1U] != '\r') {
    --line_start;
  }

  bool first_non_space = true;
  for (std::size_t i = line_start; i < offset; ++i) {
    if (source[i] != ' ' && source[i] != '\t') {
      first_non_space = false;
      break;
    }
  }
  if (first_non_space) {
    return true;
  }

  const char previous = source[offset - 1U];
  return previous == ' ' || previous == '\t';
}

void append_comment_syntax_spans(std::vector<SyntaxSpan> *spans,
                                 const std::string &source,
                                 std::size_t start_offset,
                                 std::size_t end_offset) {
  if (spans == nullptr || start_offset >= source.size()) {
    return;
  }
  end_offset = std::min(end_offset, source.size());
  std::size_t cursor = start_offset;
  while (cursor < end_offset) {
    if (comment_starts_at_for_highlight(source, cursor)) {
      std::size_t comment_end = cursor;
      while (comment_end < end_offset && source[comment_end] != '\n' &&
             source[comment_end] != '\r') {
        ++comment_end;
      }
      spans->push_back(SyntaxSpan{cursor, comment_end, SyntaxStyle::Comment});
      cursor = comment_end;
      continue;
    }
    ++cursor;
  }
}

std::vector<SyntaxSpan> syntax_spans_for_source(const std::string &source) {
  std::vector<SyntaxSpan> spans;
  amber::lexer::LexResult result = lex_source(source, "<iamber-highlight>");
  std::size_t previous_end = 0;
  for (const amber::lexer::Token &token : result.tokens) {
    const std::size_t start = std::min(token.span.start.offset, source.size());
    const std::size_t end = std::min(token.span.end.offset, source.size());
    if (start > previous_end) {
      append_comment_syntax_spans(&spans, source, previous_end, start);
    }
    const SyntaxStyle style = syntax_style_for_token(token.kind);
    if (style != SyntaxStyle::Plain && start < end) {
      spans.push_back(SyntaxSpan{start, end, style});
    }
    previous_end = std::max(previous_end, end);
  }
  if (previous_end < source.size()) {
    append_comment_syntax_spans(&spans, source, previous_end, source.size());
  }
  return spans;
}

std::vector<amber::lexer::Diagnostic>
effect_diagnostics_only(const amber::checker::CheckResult &result) {
  std::vector<amber::lexer::Diagnostic> diagnostics;
  for (const amber::lexer::Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.phase == "effects") {
      diagnostics.push_back(diagnostic);
    }
  }
  return diagnostics;
}

std::string diagnostics_to_summary(
    const std::vector<amber::lexer::Diagnostic> &diagnostics) {
  std::ostringstream out;
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    const amber::lexer::Diagnostic &diagnostic = diagnostics[i];
    if (i != 0U) {
      out << "\n";
    }
    out << diagnostic.code << ": " << diagnostic.message;
    if (!diagnostic.span.file.empty() && diagnostic.span.start.line > 0) {
      out << " at " << diagnostic.span.file << ":" << diagnostic.span.start.line
          << ":" << diagnostic.span.start.col;
    }
  }
  return out.str();
}

std::string verify_errors_to_summary(
    const std::vector<amber::bytecode::VerifyError> &errors) {
  std::ostringstream out;
  for (std::size_t i = 0; i < errors.size(); ++i) {
    const amber::bytecode::VerifyError &error = errors[i];
    if (i != 0U) {
      out << "\n";
    }
    out << error.code << ": " << error.message;
    if (!error.section.empty()) {
      out << " in " << error.section;
      if (error.offset != 0U) {
        out << " at offset " << error.offset;
      }
    }
  }
  return out.str();
}

std::size_t zero_based_column(std::size_t column) {
  return column == 0U ? 0U : column - 1U;
}

void normalize_source_error_range(SourceErrorRange *range) {
  if (range == nullptr || range->start_line == 0U) {
    return;
  }
  if (range->end_line == 0U || range->end_line < range->start_line) {
    range->end_line = range->start_line;
  }
  if (range->end_line == range->start_line &&
      range->end_column <= range->start_column) {
    range->end_column = range->start_column + 1U;
  }
  if (range->has_offsets && range->end_offset < range->start_offset) {
    range->end_offset = range->start_offset;
  }
}

SourceErrorRange source_error_range_from_span(const amber::lexer::Span &span) {
  SourceErrorRange range;
  range.file = span.file;
  range.start_line = span.start.line;
  range.start_column = zero_based_column(span.start.col);
  range.end_line = span.end.line == 0U ? span.start.line : span.end.line;
  range.end_column = span.end.col == 0U ? range.start_column + 1U
                                        : zero_based_column(span.end.col);
  range.start_offset = span.start.offset;
  range.end_offset = span.end.offset;
  range.has_offsets = span.start.offset != 0U || span.end.offset != 0U;
  normalize_source_error_range(&range);
  return range;
}

bool same_source_error_range(const SourceErrorRange &left,
                             const SourceErrorRange &right) {
  return left.file == right.file && left.start_line == right.start_line &&
         left.start_column == right.start_column &&
         left.end_line == right.end_line &&
         left.end_column == right.end_column &&
         left.start_offset == right.start_offset &&
         left.end_offset == right.end_offset &&
         left.whole_line == right.whole_line;
}

void push_unique_source_error_range(std::vector<SourceErrorRange> *ranges,
                                    SourceErrorRange range) {
  if (ranges == nullptr || range.start_line == 0U) {
    return;
  }
  normalize_source_error_range(&range);
  for (const SourceErrorRange &existing : *ranges) {
    if (same_source_error_range(existing, range)) {
      return;
    }
  }
  ranges->push_back(std::move(range));
}

std::vector<SourceErrorRange> source_error_ranges_from_diagnostics(
    const std::vector<amber::lexer::Diagnostic> &diagnostics) {
  std::vector<SourceErrorRange> ranges;
  for (const amber::lexer::Diagnostic &diagnostic : diagnostics) {
    push_unique_source_error_range(
        &ranges, source_error_range_from_span(diagnostic.span));
  }
  return ranges;
}

std::vector<SourceErrorRange> source_error_ranges_from_loader_result(
    const amber::runtime::RuntimeModuleLoadResult &result) {
  std::vector<SourceErrorRange> ranges;
  for (const amber::runtime::RuntimeLoaderDiagnostic &diagnostic :
       result.diagnostics) {
    const amber::runtime::RuntimeDebugLocation &location =
        diagnostic.location;
    if (location.line == 0U) {
      continue;
    }
    SourceErrorRange range;
    range.file = location.file;
    range.start_line = location.line;
    range.start_column = zero_based_column(location.column);
    range.end_line = location.line;
    range.end_column = range.start_column + 1U;
    range.whole_line = location.column == 0U;
    push_unique_source_error_range(&ranges, std::move(range));
  }
  return ranges;
}

std::string loader_result_to_summary(
    const amber::runtime::RuntimeModuleLoadResult &result) {
  std::ostringstream out;
  if (!result.error_name.empty()) {
    out << result.error_name << ": ";
  }
  out << (result.message.empty() ? "module load failed" : result.message);
  for (const amber::runtime::RuntimeLoaderDiagnostic &diagnostic :
       result.diagnostics) {
    if (diagnostic.message.empty() || diagnostic.message == result.message) {
      continue;
    }
    out << "\n";
    if (!diagnostic.error_name.empty()) {
      out << diagnostic.error_name << ": ";
    }
    out << diagnostic.message;
  }
  return out.str();
}

CompileResult compile_source_text(const std::string &source,
                                  const std::string &source_path) {
  amber::lexer::LexResult lex_result = lex_source(source, source_path);
  if (!lex_result.ok()) {
    return {false,
            {},
            diagnostics_to_summary(lex_result.diagnostics),
            source_error_ranges_from_diagnostics(lex_result.diagnostics),
            {}};
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    return {false,
            {},
            diagnostics_to_summary(parse_result.diagnostics),
            source_error_ranges_from_diagnostics(parse_result.diagnostics),
            {}};
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    return {false,
            {},
            diagnostics_to_summary(bind_result.diagnostics),
            source_error_ranges_from_diagnostics(bind_result.diagnostics),
            {}};
  }
  const std::vector<amber::lexer::Diagnostic> unresolved_name_diagnostics =
      amber::binder::unresolved_name_diagnostics(parse_result.items,
                                                 bind_result.graph);
  if (!unresolved_name_diagnostics.empty()) {
    return {false,
            {},
            diagnostics_to_summary(unresolved_name_diagnostics),
            source_error_ranges_from_diagnostics(unresolved_name_diagnostics),
            {}};
  }

  amber::checker::CheckResult check_result = amber::checker::check_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  const std::vector<amber::lexer::Diagnostic> effect_diagnostics =
      effect_diagnostics_only(check_result);
  if (!effect_diagnostics.empty()) {
    return {false,
            {},
            diagnostics_to_summary(effect_diagnostics),
            source_error_ranges_from_diagnostics(effect_diagnostics),
            {}};
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    return {false,
            {},
            diagnostics_to_summary(emit_result.diagnostics),
            source_error_ranges_from_diagnostics(emit_result.diagnostics),
            {}};
  }
  emit_result.module.effects = check_result.effect_summaries;
  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(emit_result.module);
  amber::bytecode::DecodeResult decode_result =
      amber::bytecode::deserialize_module(bytes);
  if (!decode_result.ok()) {
    return {false, {}, verify_errors_to_summary(decode_result.errors), {}, {}};
  }
  return {true, std::move(decode_result.module), {}, {},
          parse_result.module_name};
}

const amber::ast::Expr *ast_node_field(const amber::ast::Expr &expr,
                                       const std::string &name) {
  for (const amber::ast::NodeField &field : expr.node_fields) {
    if (field.name == name) {
      return field.value.get();
    }
  }
  return nullptr;
}

std::string ast_string_value(const amber::ast::Expr &expr,
                             const std::string &name) {
  for (const amber::ast::StringField &field : expr.string_fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  return {};
}

std::optional<std::string>
dependency_binding_name(const amber::ast::Expr &expr) {
  if (expr.kind == "AstName") {
    return ast_string_value(expr, "name");
  }
  if (expr.kind == "AstIvar") {
    return "@" + ast_string_value(expr, "name");
  }
  if (expr.kind == "AstCvar") {
    return "@@" + ast_string_value(expr, "name");
  }
  return std::nullopt;
}

void collect_dependency_reads(const amber::ast::Expr &expr,
                              DependencyScanState *state);

void collect_dependency_write_target(const amber::ast::Expr &expr,
                                     DependencyScanState *state) {
  if (state == nullptr || state->info == nullptr) {
    return;
  }
  if (std::optional<std::string> binding = dependency_binding_name(expr)) {
    state->info->writes.insert(*binding);
    state->local_bindings.insert(std::move(*binding));
    return;
  }
  collect_dependency_reads(expr, state);
}

bool dependency_scan_skips_body(const amber::ast::Expr &expr) {
  return expr.kind == "AstDefStmt" || expr.kind == "AstClauseDef" ||
         expr.kind == "AstClassDef" || expr.kind == "AstMixinDef" ||
         expr.kind == "AstPropDef" || expr.kind == "AstClassPropDef" ||
         expr.kind == "AstAttrDef";
}

void collect_dependency_reads(const amber::ast::Expr &expr,
                              DependencyScanState *state) {
  if (state == nullptr || state->info == nullptr ||
      dependency_scan_skips_body(expr)) {
    return;
  }
  if (std::optional<std::string> binding = dependency_binding_name(expr)) {
    if (state->local_bindings.count(*binding) == 0U) {
      state->info->reads.insert(std::move(*binding));
    }
    return;
  }
  if (expr.kind == "AstAssign") {
    const amber::ast::Expr *left = ast_node_field(expr, "left");
    const amber::ast::Expr *right = ast_node_field(expr, "right");
    if (ast_string_value(expr, "op") != "=" && left != nullptr) {
      collect_dependency_reads(*left, state);
    }
    if (right != nullptr) {
      collect_dependency_reads(*right, state);
    }
    if (left != nullptr) {
      collect_dependency_write_target(*left, state);
    }
    return;
  }
  if (expr.kind == "AstPatternAssign") {
    if (const amber::ast::Expr *right = ast_node_field(expr, "right")) {
      collect_dependency_reads(*right, state);
    }
    return;
  }

  for (const amber::ast::NodeField &field : expr.node_fields) {
    if (field.value != nullptr) {
      collect_dependency_reads(*field.value, state);
    }
  }
  for (const amber::ast::ListField &field : expr.list_fields) {
    for (const std::unique_ptr<amber::ast::Expr> &value : field.values) {
      if (value != nullptr) {
        collect_dependency_reads(*value, state);
      }
    }
  }
}

CellDependencyInfo dependency_info_for_cell(const Cell &cell) {
  CellDependencyInfo info;
  amber::lexer::LexResult lex_result = lex_source(cell.source, "<iamber-cell>");
  if (!lex_result.ok()) {
    return info;
  }
  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    return info;
  }
  DependencyScanState state;
  state.info = &info;
  for (const std::unique_ptr<amber::ast::Expr> &item : parse_result.items) {
    if (item != nullptr) {
      collect_dependency_reads(*item, &state);
    }
  }
  return info;
}

std::vector<CellDependencyInfo>
dependency_info_for_cells(const std::vector<Cell> &cells) {
  std::vector<CellDependencyInfo> infos;
  infos.reserve(cells.size());
  for (const Cell &cell : cells) {
    infos.push_back(dependency_info_for_cell(cell));
  }
  return infos;
}

std::string join_dependency_names(const std::set<std::string> &names) {
  std::ostringstream out;
  std::size_t index = 0;
  for (const std::string &name : names) {
    if (index != 0U) {
      out << ", ";
    }
    out << name;
    ++index;
  }
  return out.str();
}

std::set<std::string> read_write_overlap(const CellDependencyInfo &info) {
  std::set<std::string> overlap;
  for (const std::string &name : info.reads) {
    if (info.writes.count(name) != 0U) {
      overlap.insert(name);
    }
  }
  return overlap;
}

bool dependency_graph_reaches(
    const std::vector<std::vector<std::size_t>> &graph, std::size_t current,
    std::size_t target, std::vector<unsigned char> *visited) {
  if (current >= graph.size() || visited == nullptr) {
    return false;
  }
  if ((*visited)[current] != 0U) {
    return false;
  }
  (*visited)[current] = 1U;
  for (const std::size_t next : graph[current]) {
    if (next == target) {
      return true;
    }
    if (dependency_graph_reaches(graph, next, target, visited)) {
      return true;
    }
  }
  return false;
}

std::optional<std::string>
cyclic_watch_error_for_cell(const std::vector<Cell> &cells, std::size_t index) {
  if (index >= cells.size() || !cells[index].watch) {
    return std::nullopt;
  }
  const std::vector<CellDependencyInfo> infos = dependency_info_for_cells(cells);
  const std::set<std::string> overlap = read_write_overlap(infos[index]);
  if (!overlap.empty()) {
    std::ostringstream out;
    out << "cyclic watch dependency: cell " << (index + 1U)
        << " reads and writes " << join_dependency_names(overlap);
    return out.str();
  }

  std::vector<std::vector<std::size_t>> graph(cells.size());
  for (std::size_t reader = 0; reader < cells.size(); ++reader) {
    if (!cells[reader].watch) {
      continue;
    }
    for (std::size_t writer = 0; writer < cells.size(); ++writer) {
      if (!cells[writer].watch) {
        continue;
      }
      for (const std::string &name : infos[reader].reads) {
        if (infos[writer].writes.count(name) != 0U) {
          graph[reader].push_back(writer);
          break;
        }
      }
    }
  }

  std::vector<unsigned char> visited(cells.size(), 0U);
  if (dependency_graph_reaches(graph, index, index, &visited)) {
    std::ostringstream out;
    out << "cyclic watch dependency: cell " << (index + 1U)
        << " participates in a watch dependency cycle";
    return out.str();
  }
  return std::nullopt;
}

EvalView watch_cycle_eval_view(std::string message) {
  EvalView view;
  view.error = std::move(message);
  return view;
}

std::string session_header_source() { return "package iamber.session\n\n"; }

std::string session_source_until(const std::vector<Cell> &cells,
                                 std::size_t end_index) {
  std::ostringstream out;
  out << session_header_source();
  for (std::size_t i = 0; i <= end_index && i < cells.size(); ++i) {
    out << cells[i].source;
    if (cells[i].source.empty() || cells[i].source.back() != '\n') {
      out << "\n";
    }
    out << "\n";
  }
  return out.str();
}

struct CellSourceMap {
  std::size_t cell_index = 0;
  std::size_t start_line = 1;
  std::size_t source_line_count = 1;
  std::size_t start_offset = 0;
  std::size_t source_end_offset = 0;
};

std::size_t generated_cell_source_line_count(const std::string &source) {
  return static_cast<std::size_t>(
             std::count(source.begin(), source.end(), '\n')) +
         ((source.empty() || source.back() != '\n') ? 1U : 0U);
}

std::size_t generated_cell_source_size(const std::string &source) {
  return source.size() + ((source.empty() || source.back() != '\n') ? 1U : 0U);
}

std::string session_source_for_cell_only(const std::vector<Cell> &cells,
                                         std::size_t index) {
  std::ostringstream out;
  out << session_header_source();
  for (std::size_t i = 0; i < index && i < cells.size(); ++i) {
    const std::size_t blank_lines =
        generated_cell_source_line_count(cells[i].source) + 1U;
    for (std::size_t line = 0; line < blank_lines; ++line) {
      out << "\n";
    }
  }
  if (index < cells.size()) {
    out << cells[index].source;
    if (cells[index].source.empty() || cells[index].source.back() != '\n') {
      out << "\n";
    }
    out << "\n";
  }
  return out.str();
}

std::vector<CellSourceMap>
cell_source_maps_until(const std::vector<Cell> &cells, std::size_t end_index) {
  std::vector<CellSourceMap> maps;
  const std::string header = session_header_source();
  std::size_t line = 1U + static_cast<std::size_t>(
                              std::count(header.begin(), header.end(), '\n'));
  std::size_t offset = header.size();
  for (std::size_t i = 0; i <= end_index && i < cells.size(); ++i) {
    const std::size_t source_line_count =
        generated_cell_source_line_count(cells[i].source);
    const std::size_t source_size = generated_cell_source_size(cells[i].source);
    maps.push_back(
        {i, line, source_line_count, offset, offset + cells[i].source.size()});
    line += source_line_count + 1U;
    offset += source_size + 1U;
  }
  return maps;
}

std::pair<int, int> line_column_for_offset(const std::string &text,
                                           std::size_t offset) {
  int line = 0;
  int column = 0;
  offset = std::min(offset, text.size());
  for (std::size_t i = 0; i < offset; ++i) {
    if (text[i] == '\n') {
      ++line;
      column = 0;
    } else {
      ++column;
    }
  }
  return {line, column};
}

bool same_code_error_range(const CodeErrorRange &left,
                           const CodeErrorRange &right) {
  return left.start_line == right.start_line &&
         left.start_column == right.start_column &&
         left.end_line == right.end_line &&
         left.end_column == right.end_column &&
         left.whole_line == right.whole_line;
}

void push_unique_cell_error_range(std::vector<CellErrorRange> *ranges,
                                  CellErrorRange range) {
  if (ranges == nullptr) {
    return;
  }
  for (const CellErrorRange &existing : *ranges) {
    if (existing.cell_index == range.cell_index &&
        same_code_error_range(existing.range, range.range)) {
      return;
    }
  }
  ranges->push_back(std::move(range));
}

std::optional<CellErrorRange>
map_source_error_range_to_cell(const std::vector<Cell> &cells,
                               const CellSourceMap &map,
                               const SourceErrorRange &source_range) {
  if (map.cell_index >= cells.size()) {
    return std::nullopt;
  }

  CodeErrorRange local;
  local.whole_line = source_range.whole_line;
  const std::size_t end_line =
      map.start_line + std::max<std::size_t>(1U, map.source_line_count) - 1U;
  if (source_range.start_line >= map.start_line &&
      source_range.start_line <= end_line) {
    local.start_line =
        static_cast<int>(source_range.start_line - map.start_line);
    local.start_column = static_cast<int>(source_range.start_column);
    if (source_range.end_line >= map.start_line &&
        source_range.end_line <= end_line) {
      local.end_line = static_cast<int>(source_range.end_line - map.start_line);
      local.end_column = static_cast<int>(source_range.end_column);
    } else {
      local.end_line = static_cast<int>(map.source_line_count - 1U);
      local.end_column = 0;
      local.whole_line = true;
    }
    return CellErrorRange{map.cell_index, local};
  }

  if (source_range.has_offsets &&
      source_range.start_offset >= map.start_offset &&
      source_range.start_offset <= map.source_end_offset) {
    const std::size_t local_start_offset =
        std::min(source_range.start_offset - map.start_offset,
                 cells[map.cell_index].source.size());
    const std::size_t local_end_offset =
        source_range.end_offset >= map.start_offset
            ? std::min(source_range.end_offset - map.start_offset,
                       cells[map.cell_index].source.size())
            : local_start_offset;
    const auto [start_line, start_column] = line_column_for_offset(
        cells[map.cell_index].source, local_start_offset);
    const auto [end_line_from_offset, end_column_from_offset] =
        line_column_for_offset(cells[map.cell_index].source, local_end_offset);
    local.start_line = start_line;
    local.start_column = start_column;
    local.end_line = end_line_from_offset;
    local.end_column = end_column_from_offset;
    if (local.end_line == local.start_line &&
        local.end_column <= local.start_column) {
      local.end_column = local.start_column + 1;
    }
    return CellErrorRange{map.cell_index, local};
  }

  return std::nullopt;
}

std::vector<CellErrorRange>
map_source_error_ranges_to_cells(const std::vector<Cell> &cells,
                                 std::size_t end_index,
                                 const std::vector<SourceErrorRange> &ranges) {
  std::vector<CellErrorRange> mapped;
  const std::vector<CellSourceMap> maps =
      cell_source_maps_until(cells, end_index);
  for (const SourceErrorRange &range : ranges) {
    for (const CellSourceMap &map : maps) {
      std::optional<CellErrorRange> cell_range =
          map_source_error_range_to_cell(cells, map, range);
      if (cell_range.has_value()) {
        push_unique_cell_error_range(&mapped, std::move(*cell_range));
        break;
      }
    }
  }
  return mapped;
}

std::string first_line(std::string text) {
  const std::size_t newline = text.find('\n');
  if (newline != std::string::npos) {
    text = text.substr(0, newline);
  }
  return text;
}

std::vector<std::string> error_lines(const std::string &text) {
  std::vector<std::string> lines;
  std::string line;
  for (char c : text) {
    if (c == '\n') {
      if (!line.empty()) {
        lines.push_back(line);
      }
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  if (!line.empty()) {
    lines.push_back(line);
  }
  return lines;
}

std::vector<amber::runtime::RuntimeTextOutputEvent> merge_output_events(
    const std::shared_ptr<amber::runtime::RuntimeTextWriter> &stdout_sink,
    const std::shared_ptr<amber::runtime::RuntimeTextWriter> &stderr_sink) {
  std::vector<amber::runtime::RuntimeTextOutputEvent> events;
  if (stdout_sink != nullptr) {
    const std::vector<amber::runtime::RuntimeTextOutputEvent> stdout_events =
        stdout_sink->events();
    events.insert(events.end(), stdout_events.begin(), stdout_events.end());
  }
  if (stderr_sink != nullptr) {
    const std::vector<amber::runtime::RuntimeTextOutputEvent> stderr_events =
        stderr_sink->events();
    events.insert(events.end(), stderr_events.begin(), stderr_events.end());
  }
  std::sort(events.begin(), events.end(),
            [](const amber::runtime::RuntimeTextOutputEvent &left,
               const amber::runtime::RuntimeTextOutputEvent &right) {
              return left.order < right.order;
            });
  return events;
}

std::size_t cell_index_for_source_line(const std::vector<CellSourceMap> &maps,
                                       std::size_t fallback_index,
                                       std::uint32_t source_line) {
  if (source_line == 0U) {
    return fallback_index;
  }
  for (const CellSourceMap &map : maps) {
    const std::size_t end_line =
        map.start_line + std::max<std::size_t>(1U, map.source_line_count) -
        1U;
    if (source_line >= map.start_line && source_line <= end_line) {
      return map.cell_index;
    }
  }
  return fallback_index;
}

std::vector<amber::runtime::RuntimeTextOutputEvent>
output_events_for_cell(
    const std::vector<amber::runtime::RuntimeTextOutputEvent> &events,
    const std::vector<Cell> &cells, std::size_t end_index) {
  std::vector<amber::runtime::RuntimeTextOutputEvent> filtered;
  const std::vector<CellSourceMap> maps =
      cell_source_maps_until(cells, end_index);
  for (const amber::runtime::RuntimeTextOutputEvent &event : events) {
    const std::size_t event_cell =
        event.source.present
            ? cell_index_for_source_line(maps, end_index, event.source.line)
            : end_index;
    if (event_cell == end_index) {
      filtered.push_back(event);
    }
  }
  return filtered;
}

bool should_show_local(const LocalView &local) {
  if (local.name.empty() || local.role == "temp") {
    return false;
  }
  return true;
}

EvalView evaluate_source_for_cell(const std::vector<Cell> &cells,
                                  std::size_t end_index,
                                  const std::string &source,
                                  bool *compiled_ok) {
  EvalView view;
  if (compiled_ok != nullptr) {
    *compiled_ok = false;
  }
  CompileResult compiled = compile_source_text(source, "<iamber>");
  if (!compiled.ok) {
    view.error = compiled.error;
    view.error_ranges = map_source_error_ranges_to_cells(cells, end_index,
                                                         compiled.error_ranges);
    return view;
  }
  if (compiled_ok != nullptr) {
    *compiled_ok = true;
  }
  if (!compiled.module.init.has_entry_code_id) {
    view.error = "compiled module has no init entry";
    return view;
  }

  const std::string module_name =
      compiled.module_name.empty() ? "iamber.session" : compiled.module_name;
  amber::runtime::RuntimeModuleLoader loader;
  const std::vector<std::uint8_t> bytes =
      amber::bytecode::serialize_module(compiled.module);
  const amber::runtime::RuntimeModuleLoadResult added =
      loader.add_serialized_module(module_name, bytes);
  if (!added.ok) {
    view.error = loader_result_to_summary(added);
    view.error_ranges = map_source_error_ranges_to_cells(
        cells, end_index, source_error_ranges_from_loader_result(added));
    return view;
  }

  std::shared_ptr<amber::runtime::RuntimeTextWriter> stdout_sink =
      amber::runtime::RuntimeTextWriter::cell_stream("stdout");
  std::shared_ptr<amber::runtime::RuntimeTextWriter> stderr_sink =
      amber::runtime::RuntimeTextWriter::cell_stream("stderr");
  amber::runtime::RuntimeModuleLoadResult initialized;
  {
    amber::runtime::RuntimeOutputScope output_scope(stdout_sink, stderr_sink);
    initialized = loader.initialize_module(module_name);
  }
  view.output_events =
      output_events_for_cell(merge_output_events(stdout_sink, stderr_sink),
                             cells, end_index);
  if (!initialized.ok) {
    view.error = loader_result_to_summary(initialized);
    view.error_ranges = map_source_error_ranges_to_cells(
        cells, end_index, source_error_ranges_from_loader_result(initialized));
    return view;
  }
  if (!initialized.has_execution_result) {
    view.error = "module init produced no execution result";
    return view;
  }

  view.ok = true;
  view.result = amber::runtime::value_to_debug_string(
      initialized.value, &compiled.module, &initialized.runtime_strings,
      &initialized.runtime_symbols);
  view.watch_epoch = initialized.watch_epoch;
  view.watch_event_count = initialized.watch_events.size();
  for (const amber::runtime::ExecutionLocal &local : initialized.locals) {
    LocalView local_view;
    local_view.name = local.name;
    local_view.role = local.role;
    local_view.binding_kind = local.binding_kind;
    local_view.initialized = local.initialized;
    local_view.watched = local.watched;
    local_view.watch_cell_id = local.watch_cell_id;
    local_view.watch_revision = local.watch_revision;
    local_view.value = local.initialized
                           ? amber::runtime::value_to_debug_string(
                                 local.value, &compiled.module,
                                 &initialized.runtime_strings,
                                 &initialized.runtime_symbols)
                           : "<uninitialized>";
    if (should_show_local(local_view)) {
      view.locals.push_back(std::move(local_view));
    }
  }
  return view;
}

EvalView evaluate_prefix(const std::vector<Cell> &cells,
                         std::size_t end_index) {
  if (end_index > 0U && end_index < cells.size()) {
    bool compiled_ok = false;
    EvalView isolated = evaluate_source_for_cell(
        cells, end_index, session_source_for_cell_only(cells, end_index),
        &compiled_ok);
    if (compiled_ok) {
      return isolated;
    }
  }
  return evaluate_source_for_cell(
      cells, end_index, session_source_until(cells, end_index), nullptr);
}

void clear_error_ranges_until(Session *session, std::size_t end_index) {
  if (session == nullptr || session->cells.empty()) {
    return;
  }
  end_index = std::min(end_index, session->cells.size() - 1U);
  for (std::size_t i = 0; i <= end_index; ++i) {
    session->cells[i].error_ranges.clear();
    session->cells[i].errors.clear();
    session->cells[i].selected_error = 0;
  }
}

std::string error_message_for_index(const std::vector<std::string> &messages,
                                    std::size_t index,
                                    const std::string &fallback) {
  if (messages.empty()) {
    return fallback.empty() ? "error" : first_line(fallback);
  }
  return messages[std::min(index, messages.size() - 1U)];
}

void push_cell_error(Cell *cell, std::string message,
                     const CodeErrorRange *range) {
  if (cell == nullptr) {
    return;
  }
  CellErrorView error;
  error.message = std::move(message);
  if (range != nullptr) {
    error.range = *range;
    error.has_range = true;
  }
  cell->errors.push_back(std::move(error));
}

void focus_selected_error(Session *session);

void apply_eval(Session *session, std::size_t index, EvalView view) {
  if (session == nullptr || index >= session->cells.size()) {
    return;
  }
  const std::vector<std::string> messages = error_lines(view.error);
  clear_error_ranges_until(session, index);
  for (std::size_t i = 0; i < view.error_ranges.size(); ++i) {
    const CellErrorRange &range = view.error_ranges[i];
    if (range.cell_index < session->cells.size()) {
      session->cells[range.cell_index].error_ranges.push_back(range.range);
      push_cell_error(&session->cells[range.cell_index],
                      error_message_for_index(messages, i, view.error),
                      &range.range);
    }
  }
  const std::size_t unmatched_start =
      view.error_ranges.empty()
          ? 0U
          : std::min(messages.size(), view.error_ranges.size());
  for (std::size_t i = unmatched_start; i < messages.size(); ++i) {
    push_cell_error(&session->cells[index], messages[i], nullptr);
  }
  if (!view.ok && messages.empty() && view.error_ranges.empty()) {
    push_cell_error(&session->cells[index], view.error, nullptr);
  }

  Cell *cell = &session->cells[index];
  cell->dirty = false;
  cell->running = false;
  cell->ok = view.ok;
  cell->locals = std::move(view.locals);
  cell->output_events = std::move(view.output_events);
  cell->watch_epoch = view.watch_epoch;
  cell->watch_event_count = view.watch_event_count;
  if (view.ok) {
    cell->result = std::move(view.result);
    cell->error.clear();
  } else {
    cell->result = "error";
    cell->error = std::move(view.error);
    if (index == session->selected && !cell->errors.empty()) {
      focus_selected_error(session);
    }
  }
}

void draw(Session *session, bool edit_mode);

void mark_cell_running(Session *session, std::size_t index, bool edit_mode) {
  if (session == nullptr || index >= session->cells.size()) {
    return;
  }
  session->cells[index].running = true;
  session->status = "cell running";
  draw(session, edit_mode);
}

void evaluate_cell(Session *session, std::size_t index, bool edit_mode,
                   bool show_running = true) {
  if (session == nullptr || index >= session->cells.size()) {
    return;
  }
  if (std::optional<std::string> cycle =
          cyclic_watch_error_for_cell(session->cells, index)) {
    apply_eval(session, index, watch_cycle_eval_view(std::move(*cycle)));
    session->status = "cyclic watch blocked";
    return;
  }
  if (show_running) {
    mark_cell_running(session, index, edit_mode);
  }
  apply_eval(session, index, evaluate_prefix(session->cells, index));
  session->status = session->cells[index].ok ? "cell evaluated" : "cell failed";
}

void evaluate_from(Session *session, std::size_t start, bool force_all,
                   bool edit_mode = false, bool show_running = true) {
  if (session == nullptr || session->cells.empty()) {
    return;
  }
  start = std::min(start, session->cells.size() - 1U);
  for (std::size_t i = start; i < session->cells.size(); ++i) {
    if (!force_all && i != start && !session->cells[i].watch) {
      session->cells[i].dirty = true;
      continue;
    }
    if (std::optional<std::string> cycle =
            cyclic_watch_error_for_cell(session->cells, i)) {
      apply_eval(session, i, watch_cycle_eval_view(std::move(*cycle)));
      continue;
    }
    if (show_running) {
      mark_cell_running(session, i, edit_mode);
    }
    apply_eval(session, i, evaluate_prefix(session->cells, i));
  }
  session->status =
      force_all ? "all cells evaluated" : "watched cells evaluated";
}

bool should_evaluate_on_leave(const Session &session, std::size_t index) {
  if (index >= session.cells.size()) {
    return false;
  }
  const Cell &cell = session.cells[index];
  return session.auto_watch && cell.watch && cell.dirty && !cell.source.empty();
}

void evaluate_selected_on_leave(Session *session, bool edit_mode) {
  if (session == nullptr || session->selected >= session->cells.size()) {
    return;
  }
  if (should_evaluate_on_leave(*session, session->selected)) {
    evaluate_from(session, session->selected, false, edit_mode);
  }
}

std::vector<std::string> split_lines(const std::string &text) {
  std::vector<std::string> lines;
  std::string line;
  for (char c : text) {
    if (c == '\n') {
      lines.push_back(line);
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  lines.push_back(line);
  return lines;
}

std::vector<std::size_t> line_start_offsets(const std::string &text) {
  std::vector<std::size_t> offsets{0U};
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      offsets.push_back(i + 1U);
    }
  }
  return offsets;
}

std::pair<int, int> cursor_line_column(const std::string &text,
                                       std::size_t cursor) {
  int line = 0;
  int column = 0;
  cursor = std::min(cursor, text.size());
  for (std::size_t i = 0; i < cursor; ++i) {
    if (text[i] == '\n') {
      ++line;
      column = 0;
    } else {
      ++column;
    }
  }
  return {line, column};
}

std::size_t offset_for_line_column(const std::string &text, int target_line,
                                   int target_column) {
  target_line = std::max(0, target_line);
  target_column = std::max(0, target_column);
  int line = 0;
  int column = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (line == target_line && column == target_column) {
      return i;
    }
    if (text[i] == '\n') {
      if (line == target_line) {
        return i;
      }
      ++line;
      column = 0;
    } else {
      ++column;
    }
  }
  return text.size();
}

void clamp_cursor(Cell *cell) {
  if (cell != nullptr && cell->cursor > cell->source.size()) {
    cell->cursor = cell->source.size();
  }
}

void mark_edited(Session *session) {
  if (session == nullptr || session->selected >= session->cells.size()) {
    return;
  }
  session->cells[session->selected].dirty = true;
  for (std::size_t i = session->selected; i < session->cells.size(); ++i) {
    session->cells[i].error_ranges.clear();
    session->cells[i].errors.clear();
    session->cells[i].selected_error = 0;
  }
  session->status = "cell edited";
}

void insert_text(Session *session, const std::string &text) {
  Cell &cell = session->cells[session->selected];
  clamp_cursor(&cell);
  cell.source.insert(cell.cursor, text);
  cell.cursor += text.size();
  session->preferred_column =
      cursor_line_column(cell.source, cell.cursor).second;
  mark_edited(session);
}

void insert_char(Session *session, char c) { insert_text(session, {c}); }

std::string current_line_before_cursor(const Cell &cell) {
  const std::size_t cursor = std::min(cell.cursor, cell.source.size());
  if (cursor == 0U) {
    return "";
  }
  const std::size_t newline = cell.source.rfind('\n', cursor - 1U);
  const std::size_t line_start =
      newline == std::string::npos ? 0U : newline + 1U;
  return cell.source.substr(line_start, cursor - line_start);
}

std::string leading_indent(const std::string &line) {
  std::size_t count = 0;
  while (count < line.size() && (line[count] == ' ' || line[count] == '\t')) {
    ++count;
  }
  return line.substr(0, count);
}

std::string rstrip_copy(std::string value) {
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                            value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

void insert_auto_newline(Session *session) {
  const Cell &cell = session->cells[session->selected];
  const std::string line = current_line_before_cursor(cell);
  std::string indent = leading_indent(line);
  const std::string trimmed_right = rstrip_copy(line);
  if (!trimmed_right.empty() && trimmed_right.back() == ':') {
    indent += "  ";
  }
  insert_text(session, "\n" + indent);
}

void erase_before_cursor(Session *session) {
  Cell &cell = session->cells[session->selected];
  clamp_cursor(&cell);
  if (cell.cursor == 0U) {
    return;
  }
  cell.source.erase(cell.cursor - 1U, 1U);
  --cell.cursor;
  session->preferred_column =
      cursor_line_column(cell.source, cell.cursor).second;
  mark_edited(session);
}

void erase_at_cursor(Session *session) {
  Cell &cell = session->cells[session->selected];
  clamp_cursor(&cell);
  if (cell.cursor >= cell.source.size()) {
    return;
  }
  cell.source.erase(cell.cursor, 1U);
  mark_edited(session);
}

void move_cursor_horizontal(Session *session, int delta) {
  Cell &cell = session->cells[session->selected];
  clamp_cursor(&cell);
  if (delta < 0 && cell.cursor > 0U) {
    --cell.cursor;
  } else if (delta > 0 && cell.cursor < cell.source.size()) {
    ++cell.cursor;
  }
  session->preferred_column =
      cursor_line_column(cell.source, cell.cursor).second;
}

void move_cursor_vertical(Session *session, int delta) {
  Cell &cell = session->cells[session->selected];
  const auto [line, column] = cursor_line_column(cell.source, cell.cursor);
  if (session->preferred_column < 0) {
    session->preferred_column = column;
  }
  const std::vector<std::string> lines = split_lines(cell.source);
  const int target_line =
      std::max(0, std::min(static_cast<int>(lines.size()) - 1, line + delta));
  const int target_column = std::min(
      session->preferred_column, static_cast<int>(lines[target_line].size()));
  cell.cursor = offset_for_line_column(cell.source, target_line, target_column);
}

void select_cell(Session *session, int delta) {
  if (session == nullptr || session->cells.empty()) {
    return;
  }
  const int selected = static_cast<int>(session->selected);
  const int next =
      std::max(0, std::min(static_cast<int>(session->cells.size()) - 1,
                           selected + delta));
  session->selected = static_cast<std::size_t>(next);
  clamp_cursor(&session->cells[session->selected]);
  session->preferred_column =
      cursor_line_column(session->cells[session->selected].source,
                         session->cells[session->selected].cursor)
          .second;
  session->editor_scroll = 0;
}

bool selection_would_change(const Session *session, int delta) {
  if (session == nullptr || session->cells.empty()) {
    return false;
  }
  const int selected = static_cast<int>(session->selected);
  const int next =
      std::max(0, std::min(static_cast<int>(session->cells.size()) - 1,
                           selected + delta));
  return next != selected;
}

void clamp_selected_error(Cell *cell) {
  if (cell == nullptr) {
    return;
  }
  if (cell->errors.empty()) {
    cell->selected_error = 0;
    return;
  }
  cell->selected_error =
      std::min(cell->selected_error, cell->errors.size() - 1U);
}

bool selected_cell_has_errors(const Session *session) {
  return session != nullptr && session->selected < session->cells.size() &&
         !session->cells[session->selected].errors.empty();
}

void focus_selected_error(Session *session) {
  if (!selected_cell_has_errors(session)) {
    return;
  }
  Cell &cell = session->cells[session->selected];
  clamp_selected_error(&cell);
  const CellErrorView &error = cell.errors[cell.selected_error];
  if (error.has_range) {
    const int line = std::max(0, error.range.start_line);
    const int column = std::max(0, error.range.start_column);
    cell.cursor = offset_for_line_column(cell.source, line, column);
    session->preferred_column = column;
    if (line < session->editor_scroll) {
      session->editor_scroll = line;
    }
  }
}

void select_error(Session *session, int delta) {
  if (!selected_cell_has_errors(session)) {
    return;
  }
  Cell &cell = session->cells[session->selected];
  clamp_selected_error(&cell);
  const int current = static_cast<int>(cell.selected_error);
  const int next = std::max(
      0, std::min(static_cast<int>(cell.errors.size()) - 1, current + delta));
  cell.selected_error = static_cast<std::size_t>(next);
  focus_selected_error(session);
  std::ostringstream status;
  status << "error " << (cell.selected_error + 1U) << "/" << cell.errors.size();
  session->status = status.str();
}

void new_cell(Session *session) {
  Cell cell;
  const std::size_t insert_at =
      session->cells.empty() ? 0U : session->selected + 1U;
  session->cells.insert(session->cells.begin() + insert_at, std::move(cell));
  session->selected = insert_at;
  session->editor_scroll = 0;
  session->preferred_column = 0;
  clear_error_ranges_until(session, session->cells.size() - 1U);
  session->status = "new cell";
}

void delete_cell(Session *session) {
  if (session == nullptr || session->cells.empty()) {
    return;
  }
  session->cells.erase(session->cells.begin() + session->selected);
  if (session->cells.empty()) {
    session->cells.push_back(Cell{});
  }
  if (session->selected >= session->cells.size()) {
    session->selected = session->cells.size() - 1U;
  }
  clear_error_ranges_until(session, session->cells.size() - 1U);
  session->status = "cell deleted";
}

void print_clipped(WINDOW *window, int y, int x, int width,
                   const std::string &text) {
  if (width <= 0) {
    return;
  }
  std::string clipped = text;
  if (static_cast<int>(clipped.size()) > width) {
    clipped = clipped.substr(0, static_cast<std::size_t>(width));
  }
  mvwaddnstr(window, y, x, clipped.c_str(), width);
}

void append_wrapped_line(std::vector<std::string> *lines,
                         const std::string &text, int width) {
  if (lines == nullptr || width <= 0) {
    return;
  }
  if (text.empty()) {
    lines->push_back("");
    return;
  }

  std::size_t offset = 0;
  const std::size_t wrap_width = static_cast<std::size_t>(width);
  while (offset < text.size()) {
    const std::size_t remaining = text.size() - offset;
    if (remaining <= wrap_width) {
      lines->push_back(text.substr(offset));
      return;
    }

    std::size_t next = text.rfind(' ', offset + wrap_width);
    if (next == std::string::npos || next < offset) {
      next = offset + wrap_width;
    }
    lines->push_back(text.substr(offset, next - offset));
    offset = next;
    while (offset < text.size() && text[offset] == ' ') {
      ++offset;
    }
  }
}

std::vector<std::string> wrap_text(const std::string &text, int width) {
  std::vector<std::string> wrapped;
  if (width <= 0) {
    return wrapped;
  }
  for (const std::string &line : split_lines(text)) {
    append_wrapped_line(&wrapped, line, width);
  }
  if (wrapped.empty()) {
    wrapped.push_back("");
  }
  return wrapped;
}

std::vector<std::string> output_detail_lines(
    const std::vector<amber::runtime::RuntimeTextOutputEvent> &events,
    int width) {
  std::vector<std::string> lines;
  std::string current_stream;
  std::string current_text;
  auto flush_current = [&]() {
    if (current_text.empty()) {
      return;
    }
    const std::string prefix =
        current_stream == "stderr" ? "stderr: " : "";
    std::vector<std::string> raw_lines = split_lines(current_text);
    if (!raw_lines.empty() && raw_lines.back().empty()) {
      raw_lines.pop_back();
    }
    for (const std::string &line : raw_lines) {
      const std::vector<std::string> wrapped =
          wrap_text(prefix + line, width);
      lines.insert(lines.end(), wrapped.begin(), wrapped.end());
    }
    current_text.clear();
  };

  for (const amber::runtime::RuntimeTextOutputEvent &event : events) {
    if (!current_text.empty() && event.stream != current_stream) {
      flush_current();
    }
    current_stream = event.stream;
    current_text += event.text;
  }
  flush_current();
  return lines;
}

int visible_cell_count(const Session &session, int body_height) {
  if (session.cells.empty() || body_height <= 0) {
    return 0;
  }
  const int min_pane_height = 8;
  return std::max(1, std::min(static_cast<int>(session.cells.size()),
                              std::max(1, body_height / min_pane_height)));
}

void clamp_cell_scroll(Session *session, int visible_count) {
  if (session == nullptr || session->cells.empty() || visible_count <= 0) {
    return;
  }
  if (static_cast<int>(session->selected) < session->cell_scroll) {
    session->cell_scroll = static_cast<int>(session->selected);
  }
  if (static_cast<int>(session->selected) >=
      session->cell_scroll + visible_count) {
    session->cell_scroll =
        static_cast<int>(session->selected) - visible_count + 1;
  }
  const int max_scroll =
      std::max(0, static_cast<int>(session->cells.size()) - visible_count);
  session->cell_scroll =
      std::max(0, std::min(session->cell_scroll, max_scroll));
}

std::string cell_status(const Cell &cell) {
  if (cell.running) {
    return "running";
  }
  if (cell.dirty) {
    return "dirty";
  }
  return cell.ok ? "ok" : "error";
}

bool cell_has_error(const Cell &cell) {
  return !cell.running && !cell.dirty &&
         ((!cell.ok && !cell.error.empty()) || !cell.errors.empty());
}

int border_attr_for(const Cell &cell, bool selected, bool edit_mode) {
  if (cell.running) {
    return COLOR_PAIR(kBorderRunningColor) | A_BOLD;
  }
  if (cell_has_error(cell)) {
    return COLOR_PAIR(kBorderErrorColor) | A_BOLD;
  }
  if (selected && edit_mode) {
    return COLOR_PAIR(kBorderEditColor) | A_BOLD;
  }
  return selected ? A_BOLD : A_NORMAL;
}

std::vector<std::pair<std::string, std::string>>
footer_actions(bool edit_mode) {
  if (edit_mode) {
    return {{"Esc", "Nav"},  {"C-X", "Run"},  {"C-R", "All"}, {"F2", "New"},
            {"Arw", "Move"}, {"Bksp", "Del"}, {"F10", "Quit"}};
  }
  return {{"F2", "New"},  {"Ent", "Run"}, {"R", "All"},
          {"E", "Edit"},  {"L/R", "Err"}, {"D", "Del"},
          {"W", "Watch"}, {"A", "Auto"},  {"F10", "Quit"}};
}

int footer_key_attr() {
  return has_colors() ? COLOR_PAIR(kFooterKeyColor) | A_BOLD
                      : A_REVERSE | A_BOLD;
}

int footer_label_attr() {
  return has_colors() ? COLOR_PAIR(kFooterLabelColor) : A_REVERSE;
}

int footer_status_attr() {
  return has_colors() ? COLOR_PAIR(kFooterStatusColor) : A_REVERSE;
}

int line_number_attr() {
  return has_colors() ? COLOR_PAIR(kLineNumberColor) | A_DIM : A_DIM;
}

int error_highlight_attr() {
  return has_colors() ? COLOR_PAIR(kErrorHighlightColor) | A_BOLD
                      : A_REVERSE | A_BOLD;
}

int error_frame_attr() {
  return has_colors() ? COLOR_PAIR(kBorderErrorColor) | A_BOLD | A_UNDERLINE
                      : A_BOLD | A_UNDERLINE;
}

int syntax_attr(SyntaxStyle style) {
  if (!has_colors()) {
    switch (style) {
    case SyntaxStyle::Keyword:
    case SyntaxStyle::Special:
      return A_BOLD;
    case SyntaxStyle::Comment:
      return A_DIM;
    default:
      return A_NORMAL;
    }
  }

  switch (style) {
  case SyntaxStyle::Keyword:
    return COLOR_PAIR(kSyntaxKeywordColor) | A_BOLD;
  case SyntaxStyle::Literal:
    return COLOR_PAIR(kSyntaxLiteralColor);
  case SyntaxStyle::String:
    return COLOR_PAIR(kSyntaxStringColor);
  case SyntaxStyle::Operator:
    return COLOR_PAIR(kSyntaxOperatorColor);
  case SyntaxStyle::Comment:
    return COLOR_PAIR(kSyntaxCommentColor) | A_DIM;
  case SyntaxStyle::Special:
    return COLOR_PAIR(kSyntaxSpecialColor) | A_BOLD;
  case SyntaxStyle::Plain:
    return A_NORMAL;
  }
  return A_NORMAL;
}

SyntaxStyle syntax_style_at_offset(const std::vector<SyntaxSpan> &spans,
                                   std::size_t offset) {
  for (const SyntaxSpan &span : spans) {
    if (offset < span.start_offset) {
      return SyntaxStyle::Plain;
    }
    if (offset >= span.start_offset && offset < span.end_offset) {
      return span.style;
    }
  }
  return SyntaxStyle::Plain;
}

std::pair<int, int> highlighted_columns_for_line(const CodeErrorRange &range,
                                                 int line_index,
                                                 int line_length) {
  if (line_index < range.start_line || line_index > range.end_line) {
    return {0, 0};
  }
  if (range.whole_line) {
    return {0, std::max(1, line_length)};
  }

  const int start =
      line_index == range.start_line ? std::max(0, range.start_column) : 0;
  int end = line_index == range.end_line ? range.end_column : line_length;
  if (line_index != range.end_line) {
    end = std::max(1, end);
  }
  end = std::max(start + 1, end);
  return {start, end};
}

enum class ErrorColumnStyle {
  None,
  Framed,
  Selected,
};

ErrorColumnStyle error_column_style_for(const Cell &cell, bool selected_cell,
                                        int line_index, int column,
                                        int line_length) {
  bool framed = false;
  for (std::size_t i = 0; i < cell.errors.size(); ++i) {
    const CellErrorView &error = cell.errors[i];
    if (!error.has_range) {
      continue;
    }
    const auto [start, end] =
        highlighted_columns_for_line(error.range, line_index, line_length);
    if (column >= start && column < end) {
      if (selected_cell && i == cell.selected_error) {
        return ErrorColumnStyle::Selected;
      }
      framed = true;
    }
  }
  return framed ? ErrorColumnStyle::Framed : ErrorColumnStyle::None;
}

void draw_code_text_line(WINDOW *window, int y, int x, int width,
                         const std::string &text, const Cell &cell,
                         bool selected_cell, int line_index,
                         std::size_t line_start_offset,
                         const std::vector<SyntaxSpan> &syntax_spans) {
  if (width <= 0) {
    return;
  }
  const int line_length = static_cast<int>(text.size());
  int draw_limit = line_length;
  for (const CellErrorView &error : cell.errors) {
    if (!error.has_range) {
      continue;
    }
    const auto [start, end] =
        highlighted_columns_for_line(error.range, line_index, line_length);
    if (end > start) {
      draw_limit = std::max(draw_limit, end);
    }
  }
  draw_limit = std::min(width, draw_limit);

  for (int column = 0; column < draw_limit; ++column) {
    const ErrorColumnStyle style = error_column_style_for(
        cell, selected_cell, line_index, column, line_length);
    if (style == ErrorColumnStyle::Selected) {
      wattron(window, error_highlight_attr());
    } else if (style == ErrorColumnStyle::Framed) {
      wattron(window, error_frame_attr());
    } else if (column < line_length) {
      const SyntaxStyle syntax_style = syntax_style_at_offset(
          syntax_spans, line_start_offset + static_cast<std::size_t>(column));
      if (syntax_style != SyntaxStyle::Plain) {
        wattron(window, syntax_attr(syntax_style));
      }
    }
    const char ch =
        column < line_length ? text[static_cast<std::size_t>(column)] : ' ';
    mvwaddch(window, y, x + column, ch);
    if (style == ErrorColumnStyle::Selected) {
      wattroff(window, error_highlight_attr());
    } else if (style == ErrorColumnStyle::Framed) {
      wattroff(window, error_frame_attr());
    } else if (column < line_length) {
      const SyntaxStyle syntax_style = syntax_style_at_offset(
          syntax_spans, line_start_offset + static_cast<std::size_t>(column));
      if (syntax_style != SyntaxStyle::Plain) {
        wattroff(window, syntax_attr(syntax_style));
      }
    }
  }
}

void draw_footer_segment(int y, int *x, int cols, const std::string &key,
                         const std::string &label) {
  if (x == nullptr || *x >= cols) {
    return;
  }
  const int needed = static_cast<int>(key.size() + label.size() + 2U);
  if (*x + needed > cols) {
    return;
  }

  attron(footer_key_attr());
  mvaddnstr(y, *x, key.c_str(), cols - *x);
  attroff(footer_key_attr());
  *x += static_cast<int>(key.size());

  attron(footer_label_attr());
  mvaddch(y, *x, ' ');
  ++(*x);
  mvaddnstr(y, *x, label.c_str(), cols - *x);
  attroff(footer_label_attr());
  *x += static_cast<int>(label.size());

  if (*x < cols) {
    mvaddch(y, *x, ' ');
    ++(*x);
  }
}

void draw_footer(const Session &session, bool edit_mode, int rows, int cols) {
  const int status_y = rows - 2;
  const int actions_y = rows - 1;

  std::ostringstream status;
  status << " iamber  mode:" << (edit_mode ? "edit" : "nav")
         << "  auto-watch:" << (session.auto_watch ? "on" : "off") << "  "
         << session.status;

  attron(footer_status_attr());
  for (int x = 0; x < cols; ++x) {
    mvaddch(status_y, x, ' ');
  }
  mvaddnstr(status_y, 0, status.str().c_str(), cols);
  attroff(footer_status_attr());

  for (int x = 0; x < cols; ++x) {
    mvaddch(actions_y, x, ' ');
  }
  int x = 0;
  for (const auto &action : footer_actions(edit_mode)) {
    draw_footer_segment(actions_y, &x, cols, action.first, action.second);
  }
}

void draw_double_border(WINDOW *window, int height, int width) {
  if (height <= 1 || width <= 1) {
    return;
  }
  mvwaddstr(window, 0, 0, "╔");
  mvwaddstr(window, 0, width - 1, "╗");
  mvwaddstr(window, height - 1, 0, "╚");
  mvwaddstr(window, height - 1, width - 1, "╝");
  for (int x = 1; x < width - 1; ++x) {
    mvwaddstr(window, 0, x, "═");
    mvwaddstr(window, height - 1, x, "═");
  }
  for (int y = 1; y < height - 1; ++y) {
    mvwaddstr(window, y, 0, "║");
    mvwaddstr(window, y, width - 1, "║");
  }
}

void draw_single_border(WINDOW *window, int height, int width) {
  if (height <= 1 || width <= 1) {
    return;
  }
  mvwaddstr(window, 0, 0, "┌");
  mvwaddstr(window, 0, width - 1, "┐");
  mvwaddstr(window, height - 1, 0, "└");
  mvwaddstr(window, height - 1, width - 1, "┘");
  for (int x = 1; x < width - 1; ++x) {
    mvwaddstr(window, 0, x, "─");
    mvwaddstr(window, height - 1, x, "─");
  }
  for (int y = 1; y < height - 1; ++y) {
    mvwaddstr(window, y, 0, "│");
    mvwaddstr(window, y, width - 1, "│");
  }
}

void draw_pane_border(WINDOW *window, int height, int width, bool selected) {
  if (selected) {
    draw_double_border(window, height, width);
  } else {
    draw_single_border(window, height, width);
  }
}

void draw_split_border(WINDOW *window, int height, int split_x,
                       bool outer_double) {
  if (split_x <= 0) {
    return;
  }
  for (int y = 1; y < height - 1; ++y) {
    mvwaddstr(window, y, split_x, "│");
  }
  if (outer_double) {
    mvwaddstr(window, 0, split_x, "╤");
    mvwaddstr(window, height - 1, split_x, "╧");
  } else {
    mvwaddstr(window, 0, split_x, "┬");
    mvwaddstr(window, height - 1, split_x, "┴");
  }
}

std::string selected_error_message(const Cell &cell) {
  if (cell.errors.empty()) {
    return "";
  }
  const std::size_t index =
      std::min(cell.selected_error, cell.errors.size() - 1U);
  return cell.errors[index].message;
}

std::string cell_result_line(const Cell &cell) {
  return cell.ok ? "=> " + cell.result
                 : (cell.error.empty() ? "=> not evaluated"
                                       : "! " + first_line(cell.error));
}

std::vector<std::string> cell_detail_lines(const Cell &cell, int width) {
  if (!cell.errors.empty()) {
    return wrap_text("! " + selected_error_message(cell), width);
  }
  std::vector<std::string> lines =
      output_detail_lines(cell.output_events, width);
  lines.push_back(cell_result_line(cell));
  return lines;
}

int cell_detail_row_count(const Cell &cell, int height, int width) {
  const int content_rows = std::max(0, height - 2);
  if (content_rows == 0 || width <= 0) {
    return 0;
  }
  const std::vector<std::string> detail_lines = cell_detail_lines(cell, width);
  return std::min(content_rows, static_cast<int>(detail_lines.size()));
}

void draw_cell_code(WINDOW *window, Session *session, std::size_t index,
                    bool edit_mode, int height, int code_width) {
  Cell &cell = session->cells[index];
  const bool selected = index == session->selected;
  const std::vector<std::string> lines = split_lines(cell.source);
  const std::vector<std::size_t> line_offsets = line_start_offsets(cell.source);
  const std::vector<SyntaxSpan> syntax_spans =
      syntax_spans_for_source(cell.source);
  const int text_width = code_width - 2;
  const int detail_rows = cell_detail_row_count(cell, height, text_width);
  const int detail_start = height - 1 - detail_rows;
  const int code_rows = std::max(0, detail_start - 1);
  int code_scroll = 0;

  const auto [cursor_line, cursor_column] =
      cursor_line_column(cell.source, cell.cursor);
  if (selected) {
    if (code_rows > 0) {
      if (cursor_line < session->editor_scroll) {
        session->editor_scroll = cursor_line;
      }
      if (cursor_line >= session->editor_scroll + code_rows) {
        session->editor_scroll = cursor_line - code_rows + 1;
      }
      session->editor_scroll = std::max(0, session->editor_scroll);
      code_scroll = session->editor_scroll;
    } else {
      session->editor_scroll = std::max(0, cursor_line);
    }
  }

  for (int row = 0; row < code_rows; ++row) {
    const int line_index = code_scroll + row;
    if (line_index >= static_cast<int>(lines.size())) {
      break;
    }
    std::ostringstream prefix;
    prefix.width(4);
    prefix << (line_index + 1);
    prefix << " ";
    wattron(window, line_number_attr());
    mvwaddnstr(window, row + 1, 1, prefix.str().c_str(), code_width - 2);
    wattroff(window, line_number_attr());
    const std::size_t line_start =
        line_index < static_cast<int>(line_offsets.size())
            ? line_offsets[static_cast<std::size_t>(line_index)]
            : cell.source.size();
    draw_code_text_line(window, row + 1, 6, code_width - 7, lines[line_index],
                        cell, selected, line_index, line_start, syntax_spans);
  }

  wattron(window, A_DIM);
  const std::vector<std::string> detail_lines =
      cell_detail_lines(cell, text_width);
  for (int row = 0; row < detail_rows; ++row) {
    const std::string &line = detail_lines[static_cast<std::size_t>(row)];
    mvwaddnstr(window, detail_start + row, 1, line.c_str(), text_width);
  }
  wattroff(window, A_DIM);

  (void)selected;
  (void)edit_mode;
  (void)cursor_column;
}

int first_visible_error_index(const Cell &cell, int visible_rows) {
  if (visible_rows <= 0 || cell.errors.empty()) {
    return 0;
  }
  const int selected =
      static_cast<int>(std::min(cell.selected_error, cell.errors.size() - 1U));
  if (selected < visible_rows) {
    return 0;
  }
  return selected - visible_rows + 1;
}

void draw_cell_errors(WINDOW *window, Cell *cell, int height, int split_x,
                      int width, bool selected) {
  const int panel_x = split_x + 1;
  const int panel_width = width - panel_x - 1;
  if (cell == nullptr || panel_width <= 0) {
    return;
  }
  clamp_selected_error(cell);

  wattron(window, A_BOLD);
  std::ostringstream title;
  title << "errors";
  if (!cell->errors.empty()) {
    title << " " << (cell->selected_error + 1U) << "/" << cell->errors.size();
  }
  print_clipped(window, 1, panel_x + 1, panel_width - 2, title.str());
  wattroff(window, A_BOLD);

  const int visible_rows = std::max(0, height - 3);
  const int first = first_visible_error_index(*cell, visible_rows);
  int row = 2;
  for (int shown = 0; shown < visible_rows; ++shown) {
    const int error_index = first + shown;
    if (error_index >= static_cast<int>(cell->errors.size())) {
      break;
    }
    const bool active = selected && static_cast<std::size_t>(error_index) ==
                                        cell->selected_error;
    std::ostringstream line;
    line << (active ? "> " : "  ") << (error_index + 1) << ". "
         << cell->errors[static_cast<std::size_t>(error_index)].message;
    if (active) {
      wattron(window, A_BOLD);
    }
    print_clipped(window, row++, panel_x + 1, panel_width - 2, line.str());
    if (active) {
      wattroff(window, A_BOLD);
    }
  }
}

void draw_cell_locals(WINDOW *window, const Cell &cell, int height, int split_x,
                      int width) {
  const int locals_x = split_x + 1;
  const int locals_width = width - locals_x - 1;
  if (locals_width <= 0) {
    return;
  }

  wattron(window, A_BOLD);
  print_clipped(window, 1, locals_x + 1, locals_width - 2, "locals");
  wattroff(window, A_BOLD);

  int row = 2;
  if (!cell.ok && !cell.error.empty()) {
    print_clipped(window, row++, locals_x + 1, locals_width - 2, "error");
  } else if (cell.locals.empty()) {
    print_clipped(window, row++, locals_x + 1, locals_width - 2, "(none)");
  }

  for (const LocalView &local : cell.locals) {
    if (row > height - 2) {
      break;
    }
    std::ostringstream line;
    line << local.name << " = " << local.value;
    print_clipped(window, row++, locals_x + 1, locals_width - 2, line.str());
    if (row > height - 2) {
      break;
    }
    std::ostringstream meta;
    meta << "  " << local.role;
    if (local.watched) {
      meta << " watch#" << local.watch_cell_id << " r"
           << local.watch_revision;
    }
    wattron(window, A_DIM);
    print_clipped(window, row++, locals_x + 1, locals_width - 2, meta.str());
    wattroff(window, A_DIM);
  }
}

void place_edit_cursor(WINDOW *window, Session *session, std::size_t index,
                       int height, int code_width) {
  if (index != session->selected) {
    return;
  }
  const Cell &cell = session->cells[index];
  const auto [cursor_line, cursor_column] =
      cursor_line_column(cell.source, cell.cursor);
  const int detail_rows = cell_detail_row_count(cell, height, code_width - 2);
  const int detail_start = height - 1 - detail_rows;
  const int code_rows = std::max(0, detail_start - 1);
  const int code_scroll = session->editor_scroll;
  if (code_rows <= 0) {
    return;
  }
  if (cursor_line < code_scroll || cursor_line >= code_scroll + code_rows) {
    return;
  }
  const int y = 1 + cursor_line - code_scroll;
  const int x = std::min(code_width - 2, 6 + cursor_column);
  wmove(window, y, x);
}

void draw_cell_pane(WINDOW *window, Session *session, std::size_t index,
                    bool edit_mode, int height, int width) {
  Cell &cell = session->cells[index];
  const bool selected = index == session->selected;
  const int border_attr = border_attr_for(cell, selected, edit_mode);
  wattron(window, border_attr);
  draw_pane_border(window, height, width, selected);

  const int locals_width = std::max(24, std::min(42, width / 3));
  const int split_x = std::max(18, width - locals_width - 1);
  draw_split_border(window, height, split_x, selected);

  std::ostringstream title;
  title << " cell " << (index + 1U) << " " << cell_status(cell) << " "
        << (cell.watch ? "watch:on" : "watch:off");
  if (cell.watch_event_count != 0U) {
    title << " events:" << cell.watch_event_count << " epoch:"
          << cell.watch_epoch;
  }
  if (selected) {
    if (cell.running) {
      title << " RUNNING";
    } else {
      title << (edit_mode ? " EDIT" : " NAV");
    }
  }
  mvwaddnstr(window, 0, 2, title.str().c_str(), std::max(0, width - 4));
  wattroff(window, border_attr);

  draw_cell_code(window, session, index, edit_mode, height, split_x);
  if (!cell.errors.empty()) {
    draw_cell_errors(window, &cell, height, split_x, width, selected);
  } else {
    draw_cell_locals(window, cell, height, split_x, width);
  }
  if (selected && edit_mode) {
    place_edit_cursor(window, session, index, height, split_x);
  }
}

void draw(Session *session, bool edit_mode) {
  int rows = 0;
  int cols = 0;
  getmaxyx(stdscr, rows, cols);
  curs_set(edit_mode ? 1 : 0);
  erase();
  if (rows < 12 || cols < 60) {
    mvaddstr(0, 0, "iamber needs at least 60x12");
    refresh();
    return;
  }

  const int footer_height = 2;
  const int body_height = rows - footer_height;
  const int visible_count = visible_cell_count(*session, body_height);
  clamp_cell_scroll(session, visible_count);
  const int base_height =
      visible_count <= 0 ? body_height : body_height / visible_count;
  const int remainder = visible_count <= 0 ? 0 : body_height % visible_count;

  draw_footer(*session, edit_mode, rows, cols);
  wnoutrefresh(stdscr);

  std::vector<WINDOW *> panes;
  WINDOW *active_pane = nullptr;
  int y = 0;
  for (int slot = 0; slot < visible_count; ++slot) {
    const std::size_t index =
        static_cast<std::size_t>(session->cell_scroll + slot);
    if (index >= session->cells.size()) {
      break;
    }
    const int pane_height = base_height + (slot < remainder ? 1 : 0);
    WINDOW *pane = newwin(pane_height, cols, y, 0);
    draw_cell_pane(pane, session, index, edit_mode, pane_height, cols);
    wnoutrefresh(pane);
    if (index == session->selected) {
      active_pane = pane;
    }
    panes.push_back(pane);
    y += pane_height;
  }
  if (active_pane != nullptr) {
    touchwin(active_pane);
    wnoutrefresh(active_pane);
  }
  doupdate();

  for (WINDOW *pane : panes) {
    delwin(pane);
  }
}

bool handle_nav_key(Session *session, int ch, bool *edit_mode) {
  switch (ch) {
  case 'q':
  case KEY_F(10):
    return false;
  case 'e':
    *edit_mode = true;
    curs_set(1);
    return true;
  case KEY_UP:
    if (selection_would_change(session, -1)) {
      evaluate_selected_on_leave(session, false);
    }
    select_cell(session, -1);
    return true;
  case KEY_DOWN:
    if (selection_would_change(session, 1)) {
      evaluate_selected_on_leave(session, false);
    }
    select_cell(session, 1);
    return true;
  case KEY_LEFT:
    select_error(session, -1);
    return true;
  case KEY_RIGHT:
    select_error(session, 1);
    return true;
  case '\n':
  case '\r':
    evaluate_cell(session, session->selected, false);
    return true;
  case 'r':
    evaluate_from(session, 0, true, false);
    return true;
  case 'n':
  case KEY_F(2):
    evaluate_selected_on_leave(session, false);
    new_cell(session);
    return true;
  case 'd':
    delete_cell(session);
    return true;
  case 'w':
    session->cells[session->selected].watch =
        !session->cells[session->selected].watch;
    session->status = session->cells[session->selected].watch
                          ? "object.watch enabled"
                          : "object.watch disabled";
    return true;
  case 'a':
    session->auto_watch = !session->auto_watch;
    session->status =
        session->auto_watch ? "auto-watch enabled" : "auto-watch disabled";
    return true;
  default:
    return true;
  }
}

bool handle_edit_key(Session *session, int ch, bool *edit_mode) {
  switch (ch) {
  case KEY_F(10):
    return false;
  case 27:
    evaluate_selected_on_leave(session, true);
    *edit_mode = false;
    curs_set(0);
    return true;
  case KEY_F(2):
  case 14:
    evaluate_selected_on_leave(session, true);
    new_cell(session);
    return true;
  case 24:
    evaluate_from(session, session->selected, false, true);
    *edit_mode = false;
    curs_set(0);
    return true;
  case 18:
    evaluate_from(session, 0, true, true);
    return true;
  case KEY_LEFT:
    move_cursor_horizontal(session, -1);
    return true;
  case KEY_RIGHT:
    move_cursor_horizontal(session, 1);
    return true;
  case KEY_UP:
    move_cursor_vertical(session, -1);
    return true;
  case KEY_DOWN:
    move_cursor_vertical(session, 1);
    return true;
  case KEY_HOME:
  case 1: {
    Cell &cell = session->cells[session->selected];
    const auto [line, column] = cursor_line_column(cell.source, cell.cursor);
    (void)column;
    cell.cursor = offset_for_line_column(cell.source, line, 0);
    session->preferred_column = 0;
    return true;
  }
  case KEY_END:
  case 5: {
    Cell &cell = session->cells[session->selected];
    const auto [line, column] = cursor_line_column(cell.source, cell.cursor);
    (void)column;
    const std::vector<std::string> lines = split_lines(cell.source);
    cell.cursor = offset_for_line_column(cell.source, line,
                                         static_cast<int>(lines[line].size()));
    session->preferred_column = static_cast<int>(lines[line].size());
    return true;
  }
  case KEY_BACKSPACE:
  case 127:
  case 8:
    erase_before_cursor(session);
    return true;
  case KEY_DC:
    erase_at_cursor(session);
    return true;
  case '\n':
  case '\r':
    insert_auto_newline(session);
    return true;
  default:
    if ((ch >= 32 && ch < 127) || (ch >= 128 && ch <= 255)) {
      insert_char(session, static_cast<char>(ch));
    }
    return true;
  }
}

int run_curses_console() {
  Session session;
  session.cells.push_back(Cell{});

  setlocale(LC_ALL, "");
  initscr();
  raw();
  noecho();
  keypad(stdscr, TRUE);
  meta(stdscr, TRUE);
  curs_set(0);
  if (has_colors()) {
    start_color();
    use_default_colors();
    init_pair(kBorderEditColor, COLOR_CYAN, -1);
    init_pair(kBorderErrorColor, COLOR_RED, -1);
    init_pair(kBorderRunningColor, COLOR_YELLOW, -1);
    init_pair(kFooterKeyColor, COLOR_BLACK, COLOR_CYAN);
    init_pair(kFooterLabelColor, COLOR_BLACK, COLOR_GREEN);
    init_pair(kFooterStatusColor, COLOR_BLACK, COLOR_WHITE);
    init_pair(kLineNumberColor, COLOR_WHITE, -1);
    init_pair(kErrorHighlightColor, COLOR_WHITE, COLOR_RED);
    init_pair(kSyntaxKeywordColor, COLOR_CYAN, -1);
    init_pair(kSyntaxLiteralColor, COLOR_YELLOW, -1);
    init_pair(kSyntaxStringColor, COLOR_GREEN, -1);
    init_pair(kSyntaxOperatorColor, COLOR_MAGENTA, -1);
    init_pair(kSyntaxCommentColor, COLOR_WHITE, -1);
    init_pair(kSyntaxSpecialColor, COLOR_BLUE, -1);
  }

  bool edit_mode = false;
  bool running = true;
  while (running) {
    draw(&session, edit_mode);
    const int ch = getch();
    if (edit_mode) {
      running = handle_edit_key(&session, ch, &edit_mode);
    } else {
      running = handle_nav_key(&session, ch, &edit_mode);
    }
  }

  endwin();
  return 0;
}

int run_eval_command(const std::string &source) {
  std::vector<Cell> cells;
  Cell cell;
  cell.source = source;
  cells.push_back(std::move(cell));
  const EvalView view = evaluate_prefix(cells, 0);
  if (!view.ok) {
    std::cerr << view.error << "\n";
    return 1;
  }
  for (const std::string &line : output_detail_lines(view.output_events, 120)) {
    std::cout << line << "\n";
  }
  std::cout << "=> " << view.result << "\n";
  if (view.watch_event_count != 0U) {
    std::cout << "watch events = " << view.watch_event_count
              << " [epoch " << view.watch_epoch << "]\n";
  }
  for (const LocalView &local : view.locals) {
    std::cout << local.name << " = " << local.value << " [" << local.role
              << "]";
    if (local.watched) {
      std::cout << " watch#" << local.watch_cell_id << " r"
                << local.watch_revision;
    }
    std::cout << "\n";
  }
  return 0;
}

void usage(std::ostream &out) {
  out << "usage:\n";
  out << "  iamber\n";
  out << "  iamber --eval <source>\n";
  out << "  iamber --eval-file <file>\n";
}

} // namespace

#ifndef AMBER_IAMBER_TESTING
int main(int argc, char **argv) {
  try {
    if (argc == 1) {
      return run_curses_console();
    }
    if (argc == 3 && std::string(argv[1]) == "--eval") {
      return run_eval_command(argv[2]);
    }
    if (argc == 3 && std::string(argv[1]) == "--eval-file") {
      return run_eval_command(read_file(argv[2]));
    }
    usage(std::cerr);
    return 2;
  } catch (const std::exception &error) {
    std::cerr << "iamber: " << error.what() << "\n";
    return 1;
  }
}
#endif
