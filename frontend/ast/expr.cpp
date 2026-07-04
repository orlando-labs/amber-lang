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

std::unique_ptr<Expr> clone_expr(const Expr &expr) {
  auto copy = std::make_unique<Expr>(expr.kind, expr.span);
  copy->string_fields = expr.string_fields;
  copy->bool_fields = expr.bool_fields;
  for (const NodeField &field : expr.node_fields) {
    copy->node_fields.push_back(
        NodeField{field.name,
                  field.value ? clone_expr(*field.value) : nullptr});
  }
  for (const ListField &field : expr.list_fields) {
    ListField list_copy;
    list_copy.name = field.name;
    for (const std::unique_ptr<Expr> &child : field.values) {
      list_copy.values.push_back(child ? clone_expr(*child) : nullptr);
    }
    copy->list_fields.push_back(std::move(list_copy));
  }
  return copy;
}

lexer::Span join_spans(const lexer::Span &start, const lexer::Span &end) {
  return lexer::Span{start.file, start.start, end.end};
}

namespace {

std::unique_ptr<Expr> make_string_literal(const std::string &text,
                                          const lexer::Span &span) {
  auto lit = make_expr("AstStringLiteral", span);
  lit->string_field("quote_kind", "double");
  lit->bool_field("interpolation", false);
  auto part = make_expr("AstStringText", span);
  part->string_field("value", text);
  std::vector<std::unique_ptr<Expr>> parts;
  parts.push_back(std::move(part));
  lit->list_field("parts", std::move(parts));
  return lit;
}

std::unique_ptr<Expr> make_bool_literal(bool value, const lexer::Span &span) {
  auto lit = make_expr("AstLiteral", span);
  lit->string_field("token", value ? "KEYWORD_TRUE" : "KEYWORD_FALSE");
  lit->string_field("value", value ? "true" : "false");
  return lit;
}

std::unique_ptr<Expr> make_map_entry(const std::string &name,
                                     std::unique_ptr<Expr> value,
                                     const lexer::Span &span) {
  auto entry = make_expr("AstMapEntry", span);
  entry->string_field("key_kind", "symbol");
  entry->string_field("key", name);
  entry->node_field("value", std::move(value));
  return entry;
}

void expand_quotes_in(std::unique_ptr<Expr> &slot);
std::unique_ptr<Expr> lower_quoted_node(const Expr &node, bool hygienic = true);

// Build `Ast.<member>(arg)` — the call shape shared by the quote lowering's
// builder (`Ast.node`) and splice coercions (`Ast.lift` / `Ast.lift_list`).
std::unique_ptr<Expr> make_ast_member_call(const std::string &member,
                                           std::vector<std::unique_ptr<Expr>> args,
                                           const lexer::Span &span) {
  auto base = make_expr("AstName", span);
  base->string_field("name", "Ast");
  auto dot = make_expr("AstTailDotMember", span);
  dot->string_field("name", member);
  dot->bool_field("chain_boundary", false);
  auto call = make_expr("AstTailCall", span);
  call->string_field("call_style", "paren");
  call->list_field("args", std::move(args));
  std::vector<std::unique_ptr<Expr>> tails;
  tails.push_back(std::move(dot));
  tails.push_back(std::move(call));
  auto chain = make_expr("AstPostfixChain", span);
  chain->node_field("base", std::move(base));
  chain->list_field("tails", std::move(tails));
  return chain;
}

// Extract and lower the compile-time expression carried by an AstUnquote /
// AstUnquoteSplice hole (expanding any quote nested inside it).
std::unique_ptr<Expr> lowered_splice_inner(const Expr &hole) {
  const Expr *inner = nullptr;
  for (const NodeField &field : hole.node_fields) {
    if (field.name == "expr") {
      inner = field.value.get();
    }
  }
  std::unique_ptr<Expr> lowered =
      inner != nullptr ? clone_expr(*inner) : make_expr("AstError", hole.span);
  expand_quotes_in(lowered);
  return lowered;
}

// Build `Ast.node("<kind>", { field: value, ... })` for a node appearing inside
// a quote. Every field dispatches by its concrete field vector, so this handles
// all node kinds uniformly (the homoiconic AST pays off here). `hygienic`
// controls whether emitted identifiers are marked for a fresh syntax context;
// it is false inside an `unhygienic(...)` region.
std::unique_ptr<Expr> lower_generic_node(const Expr &node, bool hygienic) {
  const lexer::Span span = node.span;
  std::vector<std::unique_ptr<Expr>> entries;
  for (const StringField &field : node.string_fields) {
    entries.push_back(
        make_map_entry(field.name, make_string_literal(field.value, span), span));
  }
  for (const BoolField &field : node.bool_fields) {
    entries.push_back(
        make_map_entry(field.name, make_bool_literal(field.value, span), span));
  }
  for (const NodeField &field : node.node_fields) {
    if (!field.value) {
      continue;
    }
    entries.push_back(make_map_entry(
        field.name, lower_quoted_node(*field.value, hygienic), span));
  }
  for (const ListField &field : node.list_fields) {
    bool has_splice = false;
    for (const std::unique_ptr<Expr> &child : field.values) {
      if (child && child->kind == "AstUnquoteSplice") {
        has_splice = true;
      }
    }
    if (!has_splice) {
      std::vector<std::unique_ptr<Expr>> elements;
      for (const std::unique_ptr<Expr> &child : field.values) {
        if (child) {
          elements.push_back(lower_quoted_node(*child, hygienic));
        }
      }
      auto list = make_expr("AstListLiteral", span);
      list->list_field("elements", std::move(elements));
      entries.push_back(make_map_entry(field.name, std::move(list), span));
      continue;
    }
    // `unquote_splice(list)` / `#{*list}` splices siblings into this list
    // field. Lower to segment concatenation: literal runs stay list literals,
    // each splice contributes `Ast.lift_list(expr)` (which validates and
    // lifts the compile-time List/Tuple of Ast), joined with `+`.
    std::unique_ptr<Expr> acc;
    std::vector<std::unique_ptr<Expr>> pending;
    const auto append_segment = [&](std::unique_ptr<Expr> segment) {
      if (!acc) {
        acc = std::move(segment);
        return;
      }
      auto plus = make_expr("AstBinary", span);
      plus->string_field("op", "+");
      plus->node_field("left", std::move(acc));
      plus->node_field("right", std::move(segment));
      acc = std::move(plus);
    };
    const auto flush_pending = [&]() {
      if (pending.empty()) {
        return;
      }
      auto list = make_expr("AstListLiteral", span);
      list->list_field("elements", std::move(pending));
      pending.clear();
      append_segment(std::move(list));
    };
    for (const std::unique_ptr<Expr> &child : field.values) {
      if (!child) {
        continue;
      }
      if (child->kind == "AstUnquoteSplice") {
        flush_pending();
        std::vector<std::unique_ptr<Expr>> args;
        args.push_back(lowered_splice_inner(*child));
        append_segment(
            make_ast_member_call("lift_list", std::move(args), child->span));
        continue;
      }
      pending.push_back(lower_quoted_node(*child, hygienic));
    }
    flush_pending();
    if (!acc) {
      acc = make_expr("AstListLiteral", span);
      acc->list_field("elements", {});
    }
    entries.push_back(make_map_entry(field.name, std::move(acc), span));
  }
  // Identifiers written literally inside a quote are hygienic by default: mark
  // them so the expander can stamp a fresh per-expansion syntax context
  // (DESIGN-macro-system §9). Names arriving via unquote are spliced verbatim
  // (not built here) and so carry no mark; names inside `unhygienic(...)`
  // (hygienic == false) are deliberately left unmarked.
  if (hygienic && node.kind == "AstName") {
    entries.push_back(
        make_map_entry("hygienic", make_bool_literal(true, span), span));
  }
  auto map = make_expr("AstMapLiteral", span);
  map->list_field("entries", std::move(entries));

  std::vector<std::unique_ptr<Expr>> args;
  args.push_back(make_string_literal(node.kind, span));
  args.push_back(std::move(map));
  return make_ast_member_call("node", std::move(args), span);
}

std::unique_ptr<Expr> lower_quoted_node(const Expr &node, bool hygienic) {
  if (node.kind == "AstUnhygienic") {
    // Lower the enclosed expression with hygiene disabled so its identifiers
    // bind into the caller's context.
    for (const NodeField &field : node.node_fields) {
      if (field.name == "expr" && field.value) {
        return lower_quoted_node(*field.value, /*hygienic=*/false);
      }
    }
    return make_expr("AstError", node.span);
  }
  if (node.kind == "AstUnquote") {
    // The unquoted expression evaluates at expansion time; `Ast.lift` splices
    // an Ast value through unchanged and lifts compile-time scalars
    // (Str/Int/Float/Bool) into literal nodes — that is what makes
    // `#{check.source}` splice as a string literal (DESIGN-macro-system §6).
    std::vector<std::unique_ptr<Expr>> args;
    args.push_back(lowered_splice_inner(node));
    return make_ast_member_call("lift", std::move(args), node.span);
  }
  if (node.kind == "AstUnquoteSplice") {
    // Splices are consumed by the list-field lowering in lower_generic_node.
    // One reaching a node position is left intact so the binder's macro guard
    // rejects it cleanly (a splice needs a sibling-list context).
    return clone_expr(node);
  }
  return lower_generic_node(node, hygienic);
}

std::unique_ptr<Expr> lower_quote(const Expr &quote) {
  const ListField *body = nullptr;
  for (const ListField &field : quote.list_fields) {
    if (field.name == "body") {
      body = &field;
    }
  }
  if (body != nullptr && body->values.size() == 1) {
    const Expr &only = *body->values[0];
    if (only.kind == "AstExprStmt") {
      for (const NodeField &field : only.node_fields) {
        if (field.name == "expr" && field.value) {
          return lower_quoted_node(*field.value);
        }
      }
    }
    return lower_quoted_node(only);
  }
  // Zero or many statements: represent the quote body as an AstBlock value.
  auto block = make_expr("AstBlock", quote.span);
  std::vector<std::unique_ptr<Expr>> items;
  if (body != nullptr) {
    for (const std::unique_ptr<Expr> &stmt : body->values) {
      if (stmt) {
        items.push_back(clone_expr(*stmt));
      }
    }
  }
  block->list_field("body", std::move(items));
  return lower_quoted_node(*block);
}

void expand_quotes_in(std::unique_ptr<Expr> &slot) {
  if (!slot) {
    return;
  }
  if (slot->kind == "AstQuote") {
    slot = lower_quote(*slot);
    return;
  }
  for (NodeField &field : slot->node_fields) {
    expand_quotes_in(field.value);
  }
  for (ListField &field : slot->list_fields) {
    for (std::unique_ptr<Expr> &child : field.values) {
      expand_quotes_in(child);
    }
  }
}

} // namespace

void expand_quotes(std::vector<std::unique_ptr<Expr>> &items) {
  for (std::unique_ptr<Expr> &item : items) {
    expand_quotes_in(item);
  }
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
