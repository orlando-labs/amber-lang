#include "runtime/value_display.h"

#include "bytecode/format.h"
#include "runtime/errors.h"
#include "runtime/io.h"
#include "runtime/objects.h"
#include "runtime/text.h"
#include "runtime/watch.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace amber::runtime {

namespace {

using amber::bytecode::BcModule;

struct RuntimeStringifyContext {
  const BcModule *module = nullptr;
  const std::vector<std::string> *runtime_strings = nullptr;
  const std::vector<std::string> *runtime_symbols = nullptr;
  RuntimePrettyPrintOptions options;
  std::unordered_set<const void *> active;
};

const std::vector<std::string> *
string_table_for(const RuntimeStringifyContext &context) {
  return context.runtime_strings != nullptr && !context.runtime_strings->empty()
             ? context.runtime_strings
             : (context.module == nullptr ? nullptr : &context.module->strings);
}

const std::vector<std::string> *
symbol_table_for(const RuntimeStringifyContext &context) {
  return context.runtime_symbols != nullptr && !context.runtime_symbols->empty()
             ? context.runtime_symbols
             : (context.module == nullptr ? nullptr : &context.module->symbols);
}

std::optional<std::string>
string_text_for(const RuntimeStringifyContext &context,
                std::uint32_t string_id) {
  const std::vector<std::string> *strings = string_table_for(context);
  if (strings == nullptr || string_id >= strings->size()) {
    return std::nullopt;
  }
  return (*strings)[string_id];
}

std::optional<std::string>
symbol_text_for(const RuntimeStringifyContext &context,
                std::uint32_t symbol_id) {
  const std::vector<std::string> *symbols = symbol_table_for(context);
  if (symbols == nullptr || symbol_id >= symbols->size()) {
    return std::nullopt;
  }
  return (*symbols)[symbol_id];
}

std::string escape_string_literal(const std::string &text) {
  std::string out;
  out.reserve(text.size() + 2U);
  for (unsigned char c : text) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20U) {
        constexpr char hex[] = "0123456789abcdef";
        out += "\\x";
        out.push_back(hex[(c >> 4U) & 0xFU]);
        out.push_back(hex[c & 0xFU]);
      } else {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  return out;
}

std::string indent_text(std::size_t depth) {
  return std::string(depth * 2U, ' ');
}

std::string type_label_for_cycle(const Value &value) {
  if (value.is_list()) {
    return "Array";
  }
  if (value.is_tuple()) {
    return "Tuple";
  }
  if (value.is_set()) {
    return "Set";
  }
  if (value.is_map()) {
    return "Map";
  }
  if (value.is_instance_object()) {
    return "Object";
  }
  if (value.is_closure()) {
    return "Closure";
  }
  return "Object";
}

const void *heap_identity_for(const Value &value) {
  if (value.is_list()) {
    return value.as_list().get();
  }
  if (value.is_tuple()) {
    return value.as_tuple().get();
  }
  if (value.is_set()) {
    return value.as_set().get();
  }
  if (value.is_map()) {
    return value.as_map().get();
  }
  if (value.is_instance_object()) {
    return value.as_instance_object().get();
  }
  if (value.is_closure()) {
    return value.as_closure().get();
  }
  return nullptr;
}

class RuntimeStringifyGuard {
public:
  RuntimeStringifyGuard(RuntimeStringifyContext *context, const void *identity)
      : context_(context), identity_(identity) {
    if (context_ != nullptr && identity_ != nullptr) {
      inserted_ = context_->active.insert(identity_).second;
    }
  }

  RuntimeStringifyGuard(const RuntimeStringifyGuard &) = delete;
  RuntimeStringifyGuard &operator=(const RuntimeStringifyGuard &) = delete;

  ~RuntimeStringifyGuard() {
    if (context_ != nullptr && identity_ != nullptr && inserted_) {
      context_->active.erase(identity_);
    }
  }

  bool inserted() const { return inserted_; }

private:
  RuntimeStringifyContext *context_ = nullptr;
  const void *identity_ = nullptr;
  bool inserted_ = true;
};

std::string runtime_stringify_value_impl(RuntimeStringifyContext *context,
                                         const Value &value,
                                         RuntimeStringifyMode mode,
                                         std::size_t depth);

std::string compact_join_values(RuntimeStringifyContext *context,
                                const std::vector<Value> &items,
                                RuntimeStringifyMode mode, std::size_t depth) {
  std::ostringstream out;
  const std::size_t limit = std::min(items.size(), context->options.max_items);
  for (std::size_t i = 0; i < limit; ++i) {
    if (i != 0U) {
      out << ", ";
    }
    out << runtime_stringify_value_impl(context, items[i], mode, depth + 1U);
  }
  if (items.size() > limit) {
    if (limit != 0U) {
      out << ", ";
    }
    out << "... " << (items.size() - limit) << " more";
  }
  return out.str();
}

std::string pretty_join_values(RuntimeStringifyContext *context,
                               const std::vector<Value> &items,
                               RuntimeStringifyMode mode, std::size_t depth) {
  std::ostringstream out;
  const std::size_t limit = std::min(items.size(), context->options.max_items);
  for (std::size_t i = 0; i < limit; ++i) {
    out << indent_text(depth + 1U)
        << runtime_stringify_value_impl(context, items[i], mode, depth + 1U)
        << ",\n";
  }
  if (items.size() > limit) {
    out << indent_text(depth + 1U) << "... " << (items.size() - limit)
        << " more,\n";
  }
  return out.str();
}

std::string runtime_stringify_value_impl(RuntimeStringifyContext *context,
                                         const Value &value,
                                         RuntimeStringifyMode mode,
                                         std::size_t depth) {
  if (value.is_null()) {
    return "null";
  }
  if (value.is_bool()) {
    return value.as_bool() ? "true" : "false";
  }
  if (value.is_integer()) {
    return std::to_string(value.as_integer());
  }
  if (value.is_float()) {
    std::ostringstream out;
    out << value.as_float();
    return out.str();
  }
  if (value.is_symbol()) {
    const std::optional<std::string> text =
        symbol_text_for(*context, value.as_symbol().symbol_id);
    if (!text.has_value()) {
      return mode == RuntimeStringifyMode::Display ? "<invalid-symbol>"
                                                   : ":<invalid>";
    }
    return mode == RuntimeStringifyMode::Display ? *text : ":" + *text;
  }
  if (value.is_string()) {
    const std::optional<std::string> text =
        string_text_for(*context, value.as_string().string_id);
    if (!text.has_value()) {
      return mode == RuntimeStringifyMode::Display ? "<invalid-string>"
                                                   : "\"<invalid>\"";
    }
    if (mode == RuntimeStringifyMode::Display) {
      return *text;
    }
    return "\"" + escape_string_literal(*text) + "\"";
  }
  if (value.is_result()) {
    const std::shared_ptr<ResultValue> result = value.as_result();
    if (result == nullptr) {
      return "<result null>";
    }
    // The wrapped payload renders in inspect mode so strings are quoted
    // (Ok("x"), Err(ValueError: ...)) regardless of the outer mode.
    return std::string(result->is_ok ? "Ok(" : "Err(") +
           runtime_stringify_value_impl(context, result->payload,
                                        RuntimeStringifyMode::Inspect,
                                        depth + 1U) +
           ")";
  }
  if (value.is_native_type()) {
    return std::string("<type ") +
           native_type_name(value.as_native_type().kind) + ">";
  }
  if (value.is_native_function()) {
    return std::string("<function ") +
           native_function_name(value.as_native_function().kind) + ">";
  }
  if (value.is_native_error_class()) {
    return runtime_error_name(value.as_native_error_class().error_id);
  }
  if (value.is_error_instance()) {
    const std::shared_ptr<ErrorInstanceValue> error_instance =
        value.as_error_instance();
    if (error_instance == nullptr) {
      return "<error null>";
    }
    return std::string(runtime_error_name(error_instance->error_id)) + ": " +
           error_instance->message;
  }
  if (value.is_big_int()) {
    const std::shared_ptr<BigIntValue> big = value.as_big_int();
    return big == nullptr ? "<bigint null>" : big_int_to_decimal_string(*big);
  }
  if (value.is_arg_parser()) {
    return value.as_arg_parser() == nullptr ? "<ArgParser null>"
                                            : "<ArgParser>";
  }
  if (value.is_uuid()) {
    const std::shared_ptr<RuntimeUuidValue> uuid = value.as_uuid();
    return uuid == nullptr ? "<uuid null>" : runtime_uuid_to_string(*uuid);
  }
  if (value.is_foreign_handle()) {
    const std::shared_ptr<RuntimeForeignHandle> handle =
        value.as_foreign_handle();
    if (handle == nullptr) {
      return "<native handle null>";
    }
    return "#<native " + handle->tag + (handle->live ? ">" : " destroyed>");
  }
  if (value.is_ast_node()) {
    const std::shared_ptr<RuntimeAstNode> node = value.as_ast_node();
    if (node == nullptr || node->node == nullptr) {
      return "#<Ast null>";
    }
    return "#<Ast " + runtime_ast_node_kind(*node) + ">";
  }
  if (value.is_time()) {
    const std::shared_ptr<RuntimeTimeValue> time = value.as_time();
    return time == nullptr ? "<time null>" : runtime_time_to_iso8601(*time);
  }
  if (value.is_time_zone()) {
    const std::shared_ptr<RuntimeTimeZoneValue> zone = value.as_time_zone();
    return zone == nullptr ? "<time-zone null>"
                           : runtime_time_zone_to_string(*zone);
  }
  if (value.is_time_period()) {
    const std::shared_ptr<RuntimeTimePeriodValue> period =
        value.as_time_period();
    return period == nullptr ? "<time-period null>"
                             : runtime_time_period_to_string(*period);
  }
  if (value.is_text_writer()) {
    const std::shared_ptr<RuntimeTextWriter> writer = value.as_text_writer();
    if (writer == nullptr) {
      return "<io.TextWriter null>";
    }
    if (writer->buffered()) {
      return "<io.Buffer>";
    }
    const std::string stream = writer->stream_name();
    return stream.empty() ? "<io.TextWriter>"
                          : "<io.TextWriter " + stream + ">";
  }
  if (value.is_logger()) {
    return value.as_logger() == nullptr ? "<io.Logger null>" : "<io.Logger>";
  }
  if (value.is_io_value()) {
    const std::shared_ptr<RuntimeIoValue> io_value = value.as_io_value();
    return io_value == nullptr ? "<io null>"
                               : std::string("<") + io_value->type_name() + ">";
  }

  const void *identity = heap_identity_for(value);
  if (identity != nullptr &&
      context->active.find(identity) != context->active.end()) {
    return "#<cycle " + type_label_for_cycle(value) + ">";
  }
  if (identity != nullptr && depth >= context->options.max_depth) {
    return "#<max-depth " + type_label_for_cycle(value) + ">";
  }
  RuntimeStringifyGuard guard(context, identity);
  if (identity != nullptr && !guard.inserted()) {
    return "#<cycle " + type_label_for_cycle(value) + ">";
  }

  if (value.is_class_object()) {
    const ClassObjectValue klass = value.as_class_object();
    std::ostringstream out;
    out << "<class";
    if (context->module != nullptr &&
        klass.class_index < context->module->classes.size()) {
      const std::uint32_t symbol_id =
          context->module->classes[klass.class_index].class_name_sym_id;
      const std::optional<std::string> name =
          symbol_text_for(*context, symbol_id);
      out << " " << (name.has_value() ? *name : "?");
    } else {
      out << " #" << klass.class_index;
    }
    out << ">";
    return out.str();
  }
  if (value.is_watch_cell()) {
    const std::shared_ptr<RuntimeWatchCell> cell = value.as_watch_cell();
    if (cell == nullptr) {
      return "<watch-cell null>";
    }
    const RuntimeWatchCellSnapshot snapshot = cell->snapshot();
    std::ostringstream out;
    out << "<watch-cell #" << snapshot.cell_id << " r" << snapshot.revision
        << " "
        << runtime_stringify_value_impl(context, snapshot.value, mode,
                                        depth + 1U)
        << ">";
    return out.str();
  }
  if (value.is_watch_handle()) {
    const std::shared_ptr<RuntimeWatchHandle> handle = value.as_watch_handle();
    if (handle == nullptr) {
      return "<watch null>";
    }
    const RuntimeWatchCellSnapshot snapshot = handle->snapshot();
    std::ostringstream out;
    out << "<watch #" << handle->handle_id();
    if (snapshot.cell_id != 0) {
      out << " cell:" << snapshot.cell_id << " r" << snapshot.revision;
    }
    out << ">";
    return out.str();
  }
  if (value.is_closure()) {
    const IntrusivePtr<ClosureValue> closure = value.as_closure();
    if (closure == nullptr) {
      return "<closure null>";
    }
    const std::string lifecycle = lifecycle_debug_label(closure->header);
    return lifecycle.empty()
               ? "<closure c" + std::to_string(closure->code_id) + ">"
               : "<" + lifecycle + " closure>";
  }
  if (value.is_instance_object()) {
    const IntrusivePtr<InstanceValue> instance = value.as_instance_object();
    std::ostringstream out;
    out << "<instance";
    if (instance == nullptr) {
      out << " null>";
      return out.str();
    }
    const std::string lifecycle = lifecycle_debug_label(instance->header);
    if (!lifecycle.empty()) {
      out << " " << lifecycle << ">";
      return out.str();
    }
    if (instance_is_native_range(instance)) {
      out << " Range";
    } else if (context->module != nullptr &&
               instance->class_index < context->module->classes.size()) {
      const std::uint32_t symbol_id =
          context->module->classes[instance->class_index].class_name_sym_id;
      const std::optional<std::string> name =
          symbol_text_for(*context, symbol_id);
      out << " " << (name.has_value() ? *name : "?");
    } else {
      out << " #" << instance->class_index;
    }
    out << ">";
    return out.str();
  }
  if (value.is_list()) {
    const IntrusivePtr<ListValue> list = value.as_list();
    if (list == nullptr) {
      return "[<null-list>]";
    }
    const std::string lifecycle = lifecycle_debug_label(list->header);
    if (!lifecycle.empty()) {
      return "[<" + lifecycle + "-list>]";
    }
    if (mode == RuntimeStringifyMode::Pretty && !list->items.empty()) {
      std::ostringstream out;
      out << "[\n"
          << pretty_join_values(context, list->items, mode, depth)
          << indent_text(depth) << "]";
      return out.str();
    }
    return "[" + compact_join_values(context, list->items, mode, depth) + "]";
  }
  if (value.is_tuple()) {
    const IntrusivePtr<TupleValue> tuple = value.as_tuple();
    if (tuple == nullptr) {
      return "(<null-tuple>)";
    }
    const std::string lifecycle = lifecycle_debug_label(tuple->header);
    if (!lifecycle.empty()) {
      return "(<" + lifecycle + "-tuple>)";
    }
    if (mode == RuntimeStringifyMode::Pretty && !tuple->items.empty()) {
      std::ostringstream out;
      out << "(\n"
          << pretty_join_values(context, tuple->items, mode, depth)
          << indent_text(depth) << ")";
      return out.str();
    }
    return "(" + compact_join_values(context, tuple->items, mode, depth) + ")";
  }
  if (value.is_set()) {
    const IntrusivePtr<SetValue> set = value.as_set();
    if (set == nullptr) {
      return "{<null-set>}";
    }
    const std::string lifecycle = lifecycle_debug_label(set->header);
    if (!lifecycle.empty()) {
      return "{<" + lifecycle + "-set>}";
    }
    if (set->items.empty()) {
      return "Set{}";
    }
    if (mode == RuntimeStringifyMode::Pretty) {
      std::ostringstream out;
      out << "{\n"
          << pretty_join_values(context, set->items, mode, depth)
          << indent_text(depth) << "}";
      return out.str();
    }
    std::string body = compact_join_values(context, set->items, mode, depth);
    if (set->items.size() == 1U) {
      body += ",";
    }
    return "{" + body + "}";
  }
  if (value.is_map()) {
    const IntrusivePtr<MapValue> map = value.as_map();
    if (map == nullptr) {
      return "{<null-map>}";
    }
    const std::string lifecycle = lifecycle_debug_label(map->header);
    if (!lifecycle.empty()) {
      return "{<" + lifecycle + "-map>}";
    }
    if (map->entries.empty()) {
      return "{}";
    }
    std::ostringstream out;
    if (mode == RuntimeStringifyMode::Pretty) {
      out << "{\n";
      const std::size_t limit =
          std::min(map->entries.size(), context->options.max_items);
      for (std::size_t i = 0; i < limit; ++i) {
        out << indent_text(depth + 1U)
            << runtime_stringify_value_impl(context, map->entries[i].key, mode,
                                            depth + 1U)
            << ": "
            << runtime_stringify_value_impl(context, map->entries[i].value,
                                            mode, depth + 1U)
            << ",\n";
      }
      if (map->entries.size() > limit) {
        out << indent_text(depth + 1U) << "... "
            << (map->entries.size() - limit) << " more,\n";
      }
      out << indent_text(depth) << "}";
      return out.str();
    }
    out << "{";
    const std::size_t limit =
        std::min(map->entries.size(), context->options.max_items);
    for (std::size_t i = 0; i < limit; ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << runtime_stringify_value_impl(context, map->entries[i].key, mode,
                                          depth + 1U)
          << ": "
          << runtime_stringify_value_impl(context, map->entries[i].value, mode,
                                          depth + 1U);
    }
    if (map->entries.size() > limit) {
      if (limit != 0U) {
        out << ", ";
      }
      out << "... " << (map->entries.size() - limit) << " more";
    }
    out << "}";
    return out.str();
  }
  return "<unknown>";
}

} // namespace

std::string
runtime_stringify_value(const Value &value, RuntimeStringifyMode mode,
                        const bytecode::BcModule *module,
                        const std::vector<std::string> *runtime_strings,
                        const std::vector<std::string> *runtime_symbols,
                        RuntimePrettyPrintOptions options) {
  RuntimeStringifyContext context;
  context.module = module;
  context.runtime_strings = runtime_strings;
  context.runtime_symbols = runtime_symbols;
  context.options = options;
  return runtime_stringify_value_impl(&context, value, mode, 0);
}

std::string
value_to_debug_string(const Value &value, const bytecode::BcModule *module,
                      const std::vector<std::string> *runtime_strings,
                      const std::vector<std::string> *runtime_symbols) {
  const std::vector<std::string> *debug_strings =
      runtime_strings != nullptr && !runtime_strings->empty()
          ? runtime_strings
          : (module == nullptr ? nullptr : &module->strings);
  const std::vector<std::string> *debug_symbols =
      runtime_symbols != nullptr && !runtime_symbols->empty()
          ? runtime_symbols
          : (module == nullptr ? nullptr : &module->symbols);
  if (value.is_null()) {
    return "null";
  }
  if (value.is_bool()) {
    return value.as_bool() ? "true" : "false";
  }
  if (value.is_integer()) {
    return std::to_string(value.as_integer());
  }
  if (value.is_float()) {
    std::ostringstream out;
    out << value.as_float();
    return out.str();
  }
  if (value.is_symbol()) {
    const SymbolValue symbol = value.as_symbol();
    if (debug_symbols != nullptr && symbol.symbol_id < debug_symbols->size()) {
      return ":" + (*debug_symbols)[symbol.symbol_id];
    }
    return ":<invalid>";
  }
  if (value.is_string()) {
    const StringValue string = value.as_string();
    if (debug_strings != nullptr && string.string_id < debug_strings->size()) {
      return "\"" + (*debug_strings)[string.string_id] + "\"";
    }
    return "\"<invalid>\"";
  }
  if (value.is_result()) {
    const std::shared_ptr<ResultValue> result = value.as_result();
    if (result == nullptr) {
      return "<result null>";
    }
    return std::string(result->is_ok ? "Ok(" : "Err(") +
           value_to_debug_string(result->payload, module, runtime_strings,
                                 runtime_symbols) +
           ")";
  }
  if (value.is_class_object()) {
    const ClassObjectValue klass = value.as_class_object();
    std::ostringstream out;
    out << "<class";
    if (module != nullptr && klass.class_index < module->classes.size()) {
      const std::uint32_t symbol_id =
          module->classes[klass.class_index].class_name_sym_id;
      if (debug_symbols != nullptr && symbol_id < debug_symbols->size()) {
        out << " " << (*debug_symbols)[symbol_id];
      } else {
        out << " #" << klass.class_index;
      }
    } else {
      out << " #" << klass.class_index;
    }
    out << ">";
    return out.str();
  }
  if (value.is_native_type()) {
    return std::string("<type ") +
           native_type_name(value.as_native_type().kind) + ">";
  }
  if (value.is_native_function()) {
    return std::string("<function ") +
           native_function_name(value.as_native_function().kind) + ">";
  }
  if (value.is_native_error_class()) {
    return runtime_error_name(value.as_native_error_class().error_id);
  }
  if (value.is_error_instance()) {
    const std::shared_ptr<ErrorInstanceValue> error_instance =
        value.as_error_instance();
    if (error_instance == nullptr) {
      return "<error null>";
    }
    return std::string(runtime_error_name(error_instance->error_id)) + ": " +
           error_instance->message;
  }
  if (value.is_big_int()) {
    const std::shared_ptr<BigIntValue> big = value.as_big_int();
    return big == nullptr ? "<bigint null>" : big_int_to_decimal_string(*big);
  }
  if (value.is_arg_parser()) {
    return value.as_arg_parser() == nullptr ? "<ArgParser null>"
                                            : "<ArgParser>";
  }
  if (value.is_uuid()) {
    const std::shared_ptr<RuntimeUuidValue> uuid = value.as_uuid();
    return uuid == nullptr ? "<uuid null>" : runtime_uuid_to_string(*uuid);
  }
  if (value.is_foreign_handle()) {
    const std::shared_ptr<RuntimeForeignHandle> handle =
        value.as_foreign_handle();
    if (handle == nullptr) {
      return "<native handle null>";
    }
    return "#<native " + handle->tag + (handle->live ? ">" : " destroyed>");
  }
  if (value.is_ast_node()) {
    const std::shared_ptr<RuntimeAstNode> node = value.as_ast_node();
    if (node == nullptr || node->node == nullptr) {
      return "#<Ast null>";
    }
    return "#<Ast " + runtime_ast_node_kind(*node) + ">";
  }
  if (value.is_time()) {
    const std::shared_ptr<RuntimeTimeValue> time = value.as_time();
    return time == nullptr ? "<time null>" : runtime_time_to_iso8601(*time);
  }
  if (value.is_time_zone()) {
    const std::shared_ptr<RuntimeTimeZoneValue> zone = value.as_time_zone();
    return zone == nullptr ? "<time-zone null>"
                           : runtime_time_zone_to_string(*zone);
  }
  if (value.is_time_period()) {
    const std::shared_ptr<RuntimeTimePeriodValue> period =
        value.as_time_period();
    return period == nullptr ? "<time-period null>"
                             : runtime_time_period_to_string(*period);
  }
  if (value.is_text_writer()) {
    const std::shared_ptr<RuntimeTextWriter> writer = value.as_text_writer();
    if (writer == nullptr) {
      return "<io.TextWriter null>";
    }
    if (writer->buffered()) {
      return "<io.Buffer>";
    }
    const std::string stream = writer->stream_name();
    return stream.empty() ? "<io.TextWriter>"
                          : "<io.TextWriter " + stream + ">";
  }
  if (value.is_logger()) {
    return value.as_logger() == nullptr ? "<io.Logger null>" : "<io.Logger>";
  }
  if (value.is_io_value()) {
    const std::shared_ptr<RuntimeIoValue> io_value = value.as_io_value();
    return io_value == nullptr ? "<io null>"
                               : std::string("<") + io_value->type_name() + ">";
  }
  if (value.is_watch_cell()) {
    const std::shared_ptr<RuntimeWatchCell> cell = value.as_watch_cell();
    if (cell == nullptr) {
      return "<watch-cell null>";
    }
    const RuntimeWatchCellSnapshot snapshot = cell->snapshot();
    std::ostringstream out;
    out << "<watch-cell #" << snapshot.cell_id << " r" << snapshot.revision
        << " "
        << value_to_debug_string(snapshot.value, module, runtime_strings,
                                 runtime_symbols)
        << ">";
    return out.str();
  }
  if (value.is_watch_handle()) {
    const std::shared_ptr<RuntimeWatchHandle> handle = value.as_watch_handle();
    if (handle == nullptr) {
      return "<watch null>";
    }
    const RuntimeWatchCellSnapshot snapshot = handle->snapshot();
    std::ostringstream out;
    out << "<watch #" << handle->handle_id();
    if (snapshot.cell_id != 0) {
      out << " cell:" << snapshot.cell_id << " r" << snapshot.revision;
    }
    out << ">";
    return out.str();
  }
  if (value.is_closure()) {
    const IntrusivePtr<ClosureValue> closure = value.as_closure();
    std::ostringstream out;
    if (closure == nullptr) {
      out << "<closure null>";
      return out.str();
    }
    const std::string lifecycle = lifecycle_debug_label(closure->header);
    if (!lifecycle.empty()) {
      out << "<" << lifecycle << " closure>";
      return out.str();
    }
    out << "<closure c" << closure->code_id << ">";
    return out.str();
  }
  if (value.is_instance_object()) {
    const IntrusivePtr<InstanceValue> instance = value.as_instance_object();
    std::ostringstream out;
    out << "<instance";
    if (instance == nullptr) {
      out << " null>";
      return out.str();
    }
    const std::string lifecycle = lifecycle_debug_label(instance->header);
    if (!lifecycle.empty()) {
      out << " " << lifecycle << ">";
      return out.str();
    }
    if (instance_is_native_range(instance)) {
      out << " Range";
    } else if (module != nullptr &&
               instance->class_index < module->classes.size()) {
      const std::uint32_t symbol_id =
          module->classes[instance->class_index].class_name_sym_id;
      if (debug_symbols != nullptr && symbol_id < debug_symbols->size()) {
        out << " " << (*debug_symbols)[symbol_id];
      } else {
        out << " #" << instance->class_index;
      }
    } else {
      out << " #" << instance->class_index;
    }
    out << ">";
    return out.str();
  }
  if (value.is_list()) {
    const IntrusivePtr<ListValue> list = value.as_list();
    if (list == nullptr) {
      return "[<null-list>]";
    }
    const std::string lifecycle = lifecycle_debug_label(list->header);
    if (!lifecycle.empty()) {
      return "[<" + lifecycle + "-list>]";
    }
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < list->items.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << value_to_debug_string(list->items[i], module, runtime_strings,
                                   runtime_symbols);
    }
    out << "]";
    return out.str();
  }
  if (value.is_tuple()) {
    const IntrusivePtr<TupleValue> tuple = value.as_tuple();
    if (tuple == nullptr) {
      return "(<null-tuple>)";
    }
    const std::string lifecycle = lifecycle_debug_label(tuple->header);
    if (!lifecycle.empty()) {
      return "(<" + lifecycle + "-tuple>)";
    }
    std::ostringstream out;
    out << "(";
    for (std::size_t i = 0; i < tuple->items.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << value_to_debug_string(tuple->items[i], module, runtime_strings,
                                   runtime_symbols);
    }
    out << ")";
    return out.str();
  }
  if (value.is_set()) {
    const IntrusivePtr<SetValue> set = value.as_set();
    if (set == nullptr) {
      return "{<null-set>}";
    }
    const std::string lifecycle = lifecycle_debug_label(set->header);
    if (!lifecycle.empty()) {
      return "{<" + lifecycle + "-set>}";
    }
    if (set->items.empty()) {
      return "Set{}";
    }
    std::ostringstream out;
    out << "{";
    for (std::size_t i = 0; i < set->items.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << value_to_debug_string(set->items[i], module, runtime_strings,
                                   runtime_symbols);
    }
    if (set->items.size() == 1U) {
      out << ",";
    }
    out << "}";
    return out.str();
  }
  if (value.is_map()) {
    const IntrusivePtr<MapValue> map = value.as_map();
    if (map == nullptr) {
      return "{<null-map>}";
    }
    const std::string lifecycle = lifecycle_debug_label(map->header);
    if (!lifecycle.empty()) {
      return "{<" + lifecycle + "-map>}";
    }
    std::ostringstream out;
    out << "{";
    for (std::size_t i = 0; i < map->entries.size(); ++i) {
      if (i != 0U) {
        out << ", ";
      }
      out << value_to_debug_string(map->entries[i].key, module, runtime_strings,
                                   runtime_symbols)
          << ": "
          << value_to_debug_string(map->entries[i].value, module,
                                   runtime_strings, runtime_symbols);
    }
    out << "}";
    return out.str();
  }
  return "<unknown>";
}

} // namespace amber::runtime
