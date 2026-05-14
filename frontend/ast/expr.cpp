#include "frontend/ast/expr.h"

#include <iomanip>
#include <sstream>

namespace amber::ast {
namespace {

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
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
      if (c < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<int>(c) << std::dec << std::setfill(' ');
      } else {
        out << static_cast<char>(c);
      }
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

void append_expr_json(std::ostringstream &out, const Expr &expr) {
  out << "{\"kind\":\"" << json_escape(expr.kind) << "\",\"span\":";
  append_span_json(out, expr.span);

  for (const StringField &field : expr.string_fields) {
    out << ",\"" << json_escape(field.name) << "\":\""
        << json_escape(field.value) << "\"";
  }
  for (const BoolField &field : expr.bool_fields) {
    out << ",\"" << json_escape(field.name)
        << "\":" << (field.value ? "true" : "false");
  }
  for (const NodeField &field : expr.node_fields) {
    out << ",\"" << json_escape(field.name) << "\":";
    if (field.value) {
      append_expr_json(out, *field.value);
    } else {
      out << "null";
    }
  }
  for (const ListField &field : expr.list_fields) {
    out << ",\"" << json_escape(field.name) << "\":[";
    for (std::size_t i = 0; i < field.values.size(); ++i) {
      if (i != 0) {
        out << ",";
      }
      append_expr_json(out, *field.values[i]);
    }
    out << "]";
  }
  out << "}";
}

} // namespace

Expr::Expr(std::string kind_value, lexer::Span span_value)
    : kind(std::move(kind_value)), span(std::move(span_value)) {}

Expr &Expr::string_field(std::string name, std::string value) {
  string_fields.push_back(StringField{std::move(name), std::move(value)});
  return *this;
}

Expr &Expr::bool_field(std::string name, bool value) {
  bool_fields.push_back(BoolField{std::move(name), value});
  return *this;
}

Expr &Expr::node_field(std::string name, std::unique_ptr<Expr> value) {
  node_fields.push_back(NodeField{std::move(name), std::move(value)});
  return *this;
}

Expr &Expr::list_field(std::string name,
                       std::vector<std::unique_ptr<Expr>> values) {
  list_fields.push_back(ListField{std::move(name), std::move(values)});
  return *this;
}

std::unique_ptr<Expr> make_expr(std::string kind, lexer::Span span) {
  return std::make_unique<Expr>(std::move(kind), std::move(span));
}

lexer::Span join_spans(const lexer::Span &start, const lexer::Span &end) {
  return lexer::Span{start.file, start.start, end.end};
}

std::string ast_module_to_json(const Expr &expr,
                               const std::string &source_hash) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"format\": \"amber.ast.v1\",\n";
  out << "  \"module\": null,\n";
  out << "  \"items\": [\n";
  out << "    {\"kind\":\"AstExprStmt\",\"span\":";
  append_span_json(out, expr.span);
  out << ",\"expr\":";
  append_expr_json(out, expr);
  out << "}\n";
  out << "  ],\n";
  out << "  \"source_hash\": \"sha256:" << source_hash << "\"\n";
  out << "}\n";
  return out.str();
}

std::string ast_module_to_json(const std::vector<std::unique_ptr<Expr>> &items,
                               const std::string &module_name,
                               const std::string &source_hash) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"format\": \"amber.ast.v1\",\n";
  if (module_name.empty()) {
    out << "  \"module\": null,\n";
  } else {
    out << "  \"module\": \"" << json_escape(module_name) << "\",\n";
  }
  out << "  \"items\": [\n";
  for (std::size_t i = 0; i < items.size(); ++i) {
    out << "    ";
    append_expr_json(out, *items[i]);
    if (i + 1 < items.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"source_hash\": \"sha256:" << source_hash << "\"\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::ast
