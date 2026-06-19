#include "runtime/stdlib_registry.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

namespace amber::runtime {

namespace {

using Parser = RuntimeArgParserValue;
using Spec = RuntimeArgParserValue::Spec;
using SpecKind = RuntimeArgParserValue::SpecKind;

struct ParseError {
  std::string klass;
  std::string message;
  std::string option;
  std::string value;
  std::string help;
  std::int64_t exit_code = 2;
};

struct ParseResult {
  enum class Status { Ok, Error, Faulted };

  Status status = Status::Ok;
  Value value = Value::null();
  Value exception = Value::null();
  ParseError error;
};

bool starts_with(const std::string &text, const std::string &prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

std::string lower_ascii(std::string text) {
  for (char &ch : text) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return text;
}

std::string derive_name_from_spelling(const std::string &spelling) {
  std::string name = spelling;
  while (!name.empty() && name.front() == '-') {
    name.erase(name.begin());
  }
  for (char &ch : name) {
    if (ch == '-') {
      ch = '_';
    }
  }
  return name;
}

std::string value_type_name(RuntimeNativeTypeKind kind) {
  switch (kind) {
  case RuntimeNativeTypeKind::Str:
    return "Str";
  case RuntimeNativeTypeKind::Int:
    return "Int";
  case RuntimeNativeTypeKind::Float:
    return "Float";
  case RuntimeNativeTypeKind::Bool:
    return "Bool";
  case RuntimeNativeTypeKind::Symbol:
    return "Symbol";
  default:
    return "unsupported";
  }
}

bool is_supported_arg_type(RuntimeNativeTypeKind kind) {
  return kind == RuntimeNativeTypeKind::Str ||
         kind == RuntimeNativeTypeKind::Int ||
         kind == RuntimeNativeTypeKind::Float ||
         kind == RuntimeNativeTypeKind::Bool ||
         kind == RuntimeNativeTypeKind::Symbol;
}

bool value_to_text(NativeStdlibCall &call, const Value &value,
                   const std::string &label, std::string *out) {
  const std::optional<std::string> text = call.text_of(value);
  if (!text.has_value()) {
    call.fault("TypeError", label + " must be Str or Symbol");
    return false;
  }
  *out = *text;
  return true;
}

bool list_to_strings(NativeStdlibCall &call, const Value &value,
                     const std::string &label, std::vector<std::string> *out) {
  std::vector<Value> items;
  if (!call.list_items(value, &items)) {
    return false;
  }
  out->clear();
  out->reserve(items.size());
  for (const Value &item : items) {
    std::string text;
    if (!value_to_text(call, item, label, &text)) {
      return false;
    }
    out->push_back(std::move(text));
  }
  return true;
}

bool map_to_string_pairs(
    NativeStdlibCall &call, const Value &value, const std::string &label,
    std::vector<std::pair<std::string, std::string>> *out) {
  std::vector<std::pair<std::string, Value>> entries;
  if (!call.string_keyed_entries(value, &entries)) {
    return false;
  }
  out->clear();
  out->reserve(entries.size());
  for (const auto &[key, raw] : entries) {
    std::string text;
    if (!value_to_text(call, raw, label, &text)) {
      return false;
    }
    out->push_back({key, std::move(text)});
  }
  return true;
}

bool optional_string_keyword(NativeStdlibCall &call, const std::string &name,
                             std::string *out, bool *present) {
  const std::optional<Value> value = call.keyword(name);
  if (!value.has_value()) {
    *present = false;
    return true;
  }
  *present = true;
  return value_to_text(call, *value, name, out);
}

bool optional_type_keyword(NativeStdlibCall &call,
                           RuntimeNativeTypeKind fallback,
                           RuntimeNativeTypeKind *out) {
  const std::optional<Value> value = call.keyword("type");
  if (!value.has_value()) {
    *out = fallback;
    return true;
  }
  if (!value->is_native_type()) {
    return call.fault("TypeError", "type must be a native type object") ==
           SendStatus::Matched;
  }
  const RuntimeNativeTypeKind kind = value->as_native_type().kind;
  if (!is_supported_arg_type(kind)) {
    return call.fault(
               "ArgumentError",
               "ArgParser type must be Str, Int, Float, Bool, or Symbol") ==
           SendStatus::Matched;
  }
  *out = kind;
  return true;
}

bool optional_choices_keyword(NativeStdlibCall &call, Spec *spec) {
  const std::optional<Value> value = call.keyword("choices");
  if (!value.has_value() || value->is_null()) {
    return true;
  }
  std::vector<Value> items;
  if (!call.list_items(*value, &items)) {
    return false;
  }
  spec->has_choices = true;
  spec->choices = std::move(items);
  return true;
}

bool spelling_valid(const std::string &spelling) {
  if (spelling.size() < 2 || spelling[0] != '-') {
    return false;
  }
  if (spelling.find_first_of(" \t\r\n") != std::string::npos) {
    return false;
  }
  if (starts_with(spelling, "--")) {
    return spelling.size() > 2 && spelling[2] != '-';
  }
  return spelling[1] != '-';
}

bool spelling_used_by_parser(const Parser &parser,
                             const std::string &spelling) {
  for (const Spec &spec : parser.specs) {
    for (const std::string &existing : spec.spellings) {
      if (existing == spelling) {
        return true;
      }
    }
    if (spec.kind == SpecKind::Flag && spec.negatable &&
        starts_with(spelling, "--no-")) {
      const std::string positive = "--" + spelling.substr(5);
      if (std::find(spec.spellings.begin(), spec.spellings.end(), positive) !=
          spec.spellings.end()) {
        return true;
      }
    }
  }
  return false;
}

bool validate_spelling_for_spec(NativeStdlibCall &call, const Parser &parser,
                                const std::string &spelling) {
  if (!spelling_valid(spelling)) {
    call.fault("ArgumentError", "invalid ArgParser option spelling");
    return false;
  }
  if (spelling_used_by_parser(parser, spelling)) {
    call.fault("ArgumentError", "duplicate ArgParser option spelling");
    return false;
  }
  if ((spelling == "-h" || spelling == "--help") && parser.add_help) {
    call.fault("ArgumentError",
               "ArgParser reserves -h/--help unless add_help: false");
    return false;
  }
  return true;
}

bool parser_cmdline_from_keyword(NativeStdlibCall &call, const Parser &parser,
                                 std::vector<std::string> *out) {
  const std::optional<Value> cmdline = call.keyword("cmdline");
  if (!cmdline.has_value()) {
    *out = parser.cmdline;
    return true;
  }
  return list_to_strings(call, *cmdline, "cmdline", out);
}

Value make_error_value(NativeStdlibCall &call, const ParseError &error) {
  const std::optional<std::uint16_t> error_id = runtime_error_id(error.klass);
  if (!error_id.has_value()) {
    call.fault("VMError", "ArgParser error class is not registered");
    return Value::null();
  }
  auto instance = std::make_shared<ErrorInstanceValue>();
  instance->error_id = *error_id;
  instance->message = error.message;
  instance->fields.push_back({"exit_code", Value::integer(error.exit_code)});
  if (error.klass != "ArgParser.HelpRequested") {
    instance->fields.push_back(
        {"option", error.option.empty() ? Value::null()
                                        : call.string_value(error.option)});
    instance->fields.push_back({"value", error.value.empty()
                                             ? Value::null()
                                             : call.string_value(error.value)});
  }
  instance->fields.push_back({"usage", error.help.empty()
                                           ? Value::null()
                                           : call.string_value(error.help)});
  instance->fields.push_back({"help", error.help.empty()
                                          ? Value::null()
                                          : call.string_value(error.help)});
  return Value::error_instance(std::move(instance));
}

bool is_captured_parse_exception(const Value &value) {
  if (!value.is_error_instance() || value.as_error_instance() == nullptr) {
    return false;
  }
  const std::optional<std::uint16_t> parse_error_id =
      runtime_error_id("ArgParser.ParseError");
  const std::optional<std::uint16_t> help_requested_id =
      runtime_error_id("ArgParser.HelpRequested");
  const std::uint16_t error_id = value.as_error_instance()->error_id;
  return (parse_error_id.has_value() &&
          runtime_error_is_a(error_id, *parse_error_id)) ||
         (help_requested_id.has_value() &&
          runtime_error_is_a(error_id, *help_requested_id));
}

bool error_is_a(const Value &value, const std::string &class_name) {
  if (!value.is_error_instance() || value.as_error_instance() == nullptr) {
    return false;
  }
  const std::optional<std::uint16_t> class_id = runtime_error_id(class_name);
  return class_id.has_value() &&
         runtime_error_is_a(value.as_error_instance()->error_id, *class_id);
}

std::optional<Value> error_field(const Value &value, const std::string &name) {
  if (!value.is_error_instance() || value.as_error_instance() == nullptr) {
    return std::nullopt;
  }
  for (const auto &[field_name, field_value] :
       value.as_error_instance()->fields) {
    if (field_name == name) {
      return field_value;
    }
  }
  return std::nullopt;
}

ParseError parse_error(std::string klass, std::string message,
                       std::string option = {}, std::string value = {}) {
  ParseError error;
  error.klass = std::move(klass);
  error.message = std::move(message);
  error.option = std::move(option);
  error.value = std::move(value);
  return error;
}

std::string help_text(const Parser &parser) {
  std::ostringstream out;
  out << "usage: " << (parser.name.empty() ? "program" : parser.name);
  bool has_options = parser.add_help;
  for (const Spec &spec : parser.specs) {
    if (spec.kind == SpecKind::Option || spec.kind == SpecKind::Flag) {
      has_options = true;
      break;
    }
  }
  if (has_options) {
    out << " [options]";
  }
  for (const Spec &spec : parser.specs) {
    if (spec.kind == SpecKind::Positional) {
      out << " <" << spec.name << ">";
    } else if (spec.kind == SpecKind::Rest) {
      out << " [" << spec.name << "...]";
    }
  }
  if (!parser.about.empty()) {
    out << "\n\n" << parser.about;
  }
  if (has_options) {
    out << "\n\noptions:";
    if (parser.add_help) {
      out << "\n  -h, --help";
    }
    for (const Spec &spec : parser.specs) {
      if (spec.kind != SpecKind::Option && spec.kind != SpecKind::Flag) {
        continue;
      }
      out << "\n  ";
      for (std::size_t i = 0; i < spec.spellings.size(); ++i) {
        if (i > 0) {
          out << ", ";
        }
        out << spec.spellings[i];
      }
      if (spec.kind == SpecKind::Option) {
        out << " <" << value_type_name(spec.type) << ">";
      }
    }
  }
  return out.str();
}

ParseError help_requested(const Parser &parser) {
  ParseError error;
  error.klass = "ArgParser.HelpRequested";
  error.message = "help requested";
  error.exit_code = 0;
  error.help = help_text(parser);
  return error;
}

bool env_lookup(const Parser &parser, const std::string &name,
                std::string *out) {
  for (const auto &[key, value] : parser.env) {
    if (key == name) {
      *out = value;
      return true;
    }
  }
  return false;
}

bool parse_int(const std::string &text, std::int64_t *out) {
  if (text.empty()) {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  const long long parsed = std::strtoll(text.c_str(), &end, 10);
  if (errno == ERANGE || end == text.c_str() || *end != '\0') {
    return false;
  }
  if (parsed < std::numeric_limits<std::int64_t>::min() ||
      parsed > std::numeric_limits<std::int64_t>::max()) {
    return false;
  }
  *out = static_cast<std::int64_t>(parsed);
  return true;
}

bool parse_float(const std::string &text, double *out) {
  if (text.empty()) {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  const double parsed = std::strtod(text.c_str(), &end);
  if (errno == ERANGE || end == text.c_str() || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

bool parse_bool_text(const std::string &text, bool *out) {
  const std::string lowered = lower_ascii(text);
  if (lowered == "true" || lowered == "1" || lowered == "yes" ||
      lowered == "on") {
    *out = true;
    return true;
  }
  if (lowered == "false" || lowered == "0" || lowered == "no" ||
      lowered == "off") {
    *out = false;
    return true;
  }
  return false;
}

bool convert_text(NativeStdlibCall &call, const std::string &text,
                  RuntimeNativeTypeKind type, Value *out) {
  switch (type) {
  case RuntimeNativeTypeKind::Str:
    *out = call.string_value(text);
    return true;
  case RuntimeNativeTypeKind::Int: {
    std::int64_t parsed = 0;
    if (!parse_int(text, &parsed)) {
      return false;
    }
    *out = Value::integer(parsed);
    return true;
  }
  case RuntimeNativeTypeKind::Float: {
    double parsed = 0.0;
    if (!parse_float(text, &parsed)) {
      return false;
    }
    *out = Value::floating(parsed);
    return true;
  }
  case RuntimeNativeTypeKind::Bool: {
    bool parsed = false;
    if (!parse_bool_text(text, &parsed)) {
      return false;
    }
    *out = Value::boolean(parsed);
    return true;
  }
  case RuntimeNativeTypeKind::Symbol:
    *out = call.symbol_value(text);
    return true;
  default:
    return false;
  }
}

bool simple_value_equal(NativeStdlibCall &call, const Value &lhs,
                        const Value &rhs) {
  if (lhs.is_null() || rhs.is_null()) {
    return lhs.is_null() && rhs.is_null();
  }
  if (lhs.is_bool() || rhs.is_bool()) {
    return lhs.is_bool() && rhs.is_bool() && lhs.as_bool() == rhs.as_bool();
  }
  if (lhs.is_integer() || rhs.is_integer()) {
    return lhs.is_integer() && rhs.is_integer() &&
           lhs.as_integer() == rhs.as_integer();
  }
  if (lhs.is_float() || rhs.is_float()) {
    return lhs.is_float() && rhs.is_float() && lhs.as_float() == rhs.as_float();
  }
  const std::optional<std::string> left = call.text_of(lhs);
  const std::optional<std::string> right = call.text_of(rhs);
  return left.has_value() && right.has_value() && *left == *right;
}

bool choice_allowed(NativeStdlibCall &call, const Spec &spec,
                    const Value &value) {
  if (!spec.has_choices) {
    return true;
  }
  for (const Value &choice : spec.choices) {
    if (simple_value_equal(call, value, choice)) {
      return true;
    }
  }
  return false;
}

ParseResult apply_value(NativeStdlibCall &call, const Spec &spec,
                        const std::string &raw_text,
                        const std::string &option_label, Value *out) {
  Value converted = Value::null();
  if (!convert_text(call, raw_text, spec.type, &converted)) {
    ParseResult result;
    result.status = ParseResult::Status::Error;
    result.error =
        parse_error("ArgParser.InvalidValue",
                    option_label + " expects " + value_type_name(spec.type),
                    option_label, raw_text);
    return result;
  }
  if (!spec.block.is_null()) {
    const StdlibBlockResult block_result =
        call.call_block(spec.block, {converted});
    if (block_result.status == StdlibBlockStatus::Raised) {
      ParseResult result;
      if (is_captured_parse_exception(block_result.exception)) {
        result.status = ParseResult::Status::Error;
        result.exception = block_result.exception;
        return result;
      }
      call.raise(block_result.exception);
      result.status = ParseResult::Status::Faulted;
      return result;
    }
    if (block_result.status == StdlibBlockStatus::Faulted) {
      ParseResult result;
      result.status = ParseResult::Status::Faulted;
      return result;
    }
    converted = block_result.value;
  }
  if (!choice_allowed(call, spec, converted)) {
    ParseResult result;
    result.status = ParseResult::Status::Error;
    result.error = parse_error("ArgParser.InvalidChoice",
                               option_label + " has invalid choice",
                               option_label, raw_text);
    return result;
  }
  *out = converted;
  return {};
}

void set_parsed_value(const Spec &spec, std::size_t index, Value value,
                      std::vector<Value> *scalar_values,
                      std::vector<std::vector<Value>> *multi_values,
                      std::vector<bool> *seen) {
  if (spec.multiple) {
    (*multi_values)[index].push_back(std::move(value));
  } else {
    (*scalar_values)[index] = std::move(value);
  }
  (*seen)[index] = true;
}

const Spec *find_option_spec(const Parser &parser, const std::string &token,
                             bool *negated, std::size_t *index) {
  *negated = false;
  for (std::size_t i = 0; i < parser.specs.size(); ++i) {
    const Spec &spec = parser.specs[i];
    if (spec.kind != SpecKind::Option && spec.kind != SpecKind::Flag) {
      continue;
    }
    if (std::find(spec.spellings.begin(), spec.spellings.end(), token) !=
        spec.spellings.end()) {
      *index = i;
      return &spec;
    }
    if (spec.kind == SpecKind::Flag && spec.negatable &&
        starts_with(token, "--no-")) {
      const std::string positive = "--" + token.substr(5);
      if (std::find(spec.spellings.begin(), spec.spellings.end(), positive) !=
          spec.spellings.end()) {
        *negated = true;
        *index = i;
        return &spec;
      }
    }
  }
  return nullptr;
}

bool token_requests_help(const Parser &parser, const std::string &token) {
  return parser.add_help && (token == "-h" || token == "--help");
}

ParseResult parse_with_cmdline(NativeStdlibCall &call, const Parser &parser,
                               const std::vector<std::string> &cmdline) {
  for (const std::string &token : cmdline) {
    if (token_requests_help(parser, token)) {
      ParseResult result;
      result.status = ParseResult::Status::Error;
      result.error = help_requested(parser);
      return result;
    }
  }

  std::vector<Value> scalar_values(parser.specs.size(), Value::null());
  std::vector<std::vector<Value>> multi_values(parser.specs.size());
  std::vector<bool> seen(parser.specs.size(), false);
  std::vector<std::string> positionals;

  bool parse_options = true;
  for (std::size_t i = 0; i < cmdline.size(); ++i) {
    const std::string &token = cmdline[i];
    if (parse_options && token == "--") {
      parse_options = false;
      continue;
    }
    if (parse_options && starts_with(token, "-") && token.size() > 1) {
      std::string spelling = token;
      std::string attached_value;
      const std::size_t equals = token.find('=');
      if (equals != std::string::npos && starts_with(token, "--")) {
        spelling = token.substr(0, equals);
        attached_value = token.substr(equals + 1);
      }

      bool negated = false;
      std::size_t spec_index = 0;
      const Spec *spec =
          find_option_spec(parser, spelling, &negated, &spec_index);
      if (spec == nullptr) {
        ParseResult result;
        result.status = ParseResult::Status::Error;
        result.error = parse_error("ArgParser.UnknownOption",
                                   "unknown option " + spelling, spelling);
        return result;
      }

      if (spec->kind == SpecKind::Flag) {
        Value value = Value::boolean(!negated);
        if (!attached_value.empty()) {
          bool parsed = false;
          if (!parse_bool_text(attached_value, &parsed)) {
            ParseResult result;
            result.status = ParseResult::Status::Error;
            result.error = parse_error("ArgParser.InvalidValue",
                                       spelling + " expects Bool", spelling,
                                       attached_value);
            return result;
          }
          value = Value::boolean(parsed);
        }
        set_parsed_value(*spec, spec_index, std::move(value), &scalar_values,
                         &multi_values, &seen);
        continue;
      }

      std::string raw_value = attached_value;
      if (raw_value.empty()) {
        if (i + 1 >= cmdline.size()) {
          ParseResult result;
          result.status = ParseResult::Status::Error;
          result.error = parse_error("ArgParser.MissingValue",
                                     spelling + " requires a value", spelling);
          return result;
        }
        raw_value = cmdline[++i];
      }
      Value parsed = Value::null();
      ParseResult applied =
          apply_value(call, *spec, raw_value, spelling, &parsed);
      if (applied.status != ParseResult::Status::Ok) {
        return applied;
      }
      set_parsed_value(*spec, spec_index, std::move(parsed), &scalar_values,
                       &multi_values, &seen);
      continue;
    }
    positionals.push_back(token);
  }

  std::size_t positional_index = 0;
  bool has_rest = false;
  for (std::size_t i = 0; i < parser.specs.size(); ++i) {
    const Spec &spec = parser.specs[i];
    if (spec.kind == SpecKind::Positional) {
      if (positional_index >= positionals.size()) {
        continue;
      }
      Value parsed = Value::null();
      ParseResult applied = apply_value(
          call, spec, positionals[positional_index], spec.name, &parsed);
      if (applied.status != ParseResult::Status::Ok) {
        return applied;
      }
      set_parsed_value(spec, i, std::move(parsed), &scalar_values,
                       &multi_values, &seen);
      ++positional_index;
    } else if (spec.kind == SpecKind::Rest) {
      has_rest = true;
      while (positional_index < positionals.size()) {
        Value parsed = Value::null();
        ParseResult applied = apply_value(
            call, spec, positionals[positional_index], spec.name, &parsed);
        if (applied.status != ParseResult::Status::Ok) {
          return applied;
        }
        multi_values[i].push_back(std::move(parsed));
        ++positional_index;
      }
      seen[i] = !multi_values[i].empty();
    }
  }
  if (positional_index < positionals.size() && !has_rest) {
    ParseResult result;
    result.status = ParseResult::Status::Error;
    result.error =
        parse_error("ArgParser.UnexpectedArgument",
                    "unexpected argument " + positionals[positional_index], {},
                    positionals[positional_index]);
    return result;
  }

  for (std::size_t i = 0; i < parser.specs.size(); ++i) {
    const Spec &spec = parser.specs[i];
    if (seen[i]) {
      continue;
    }
    if (!spec.env.empty()) {
      std::string env_value;
      if (env_lookup(parser, spec.env, &env_value)) {
        Value parsed = Value::null();
        ParseResult applied =
            apply_value(call, spec, env_value, spec.env, &parsed);
        if (applied.status != ParseResult::Status::Ok) {
          return applied;
        }
        set_parsed_value(spec, i, std::move(parsed), &scalar_values,
                         &multi_values, &seen);
        continue;
      }
    }
    if (spec.has_default) {
      scalar_values[i] = spec.default_value;
      seen[i] = true;
      continue;
    }
    if (spec.kind == SpecKind::Flag) {
      scalar_values[i] = Value::boolean(false);
      seen[i] = true;
      continue;
    }
    if (spec.multiple || spec.kind == SpecKind::Rest) {
      seen[i] = true;
      continue;
    }
    if (spec.required || spec.kind == SpecKind::Positional) {
      ParseResult result;
      result.status = ParseResult::Status::Error;
      result.error =
          parse_error("ArgParser.MissingRequired",
                      "missing required argument " + spec.name, spec.name);
      return result;
    }
  }

  std::vector<std::pair<std::string, Value>> entries;
  entries.reserve(parser.specs.size());
  for (std::size_t i = 0; i < parser.specs.size(); ++i) {
    const Spec &spec = parser.specs[i];
    if (spec.multiple || spec.kind == SpecKind::Rest) {
      entries.push_back(
          {spec.name, call.make_list(std::move(multi_values[i]))});
    } else {
      entries.push_back({spec.name, scalar_values[i]});
    }
  }

  ParseResult result;
  result.value = call.make_object(std::move(entries));
  return result;
}

bool apply_parser_metadata(NativeStdlibCall &call, Parser *parser) {
  if (!call.reject_unknown_keywords(
          {"cmdline", "name", "about", "env", "add_help"})) {
    return false;
  }
  const std::optional<Value> cmdline = call.keyword("cmdline");
  if (cmdline.has_value() &&
      !list_to_strings(call, *cmdline, "cmdline", &parser->cmdline)) {
    return false;
  }
  bool present = false;
  std::string text;
  if (!optional_string_keyword(call, "name", &text, &present)) {
    return false;
  }
  if (present) {
    parser->name = std::move(text);
  }
  if (!optional_string_keyword(call, "about", &text, &present)) {
    return false;
  }
  if (present) {
    parser->about = std::move(text);
  }
  const std::optional<Value> env = call.keyword("env");
  if (env.has_value() &&
      !map_to_string_pairs(call, *env, "env values", &parser->env)) {
    return false;
  }
  bool add_help = parser->add_help;
  if (!call.bool_keyword("add_help", add_help, &add_help)) {
    return false;
  }
  parser->add_help = add_help;
  return true;
}

SendStatus construct_parser(NativeStdlibCall &call) {
  if (!call.require_arity(0) || !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  auto parser = std::make_shared<Parser>();
  if (!apply_parser_metadata(call, parser.get())) {
    return SendStatus::Faulted;
  }
  *call.out = Value::arg_parser(std::move(parser));
  return SendStatus::Matched;
}

bool append_option_spec(NativeStdlibCall &call, Parser *parser, SpecKind kind) {
  if (!call.reject_unknown_keywords({"name", "type", "default", "required",
                                     "choices", "multiple", "env",
                                     "negatable"})) {
    return false;
  }
  Spec spec;
  spec.kind = kind;
  spec.type = kind == SpecKind::Flag ? RuntimeNativeTypeKind::Bool
                                     : RuntimeNativeTypeKind::Str;
  for (const Value &arg : call.args) {
    if (arg.is_null()) {
      continue;
    }
    std::string spelling;
    if (!value_to_text(call, arg, "option spelling", &spelling)) {
      return false;
    }
    if (!validate_spelling_for_spec(call, *parser, spelling)) {
      return false;
    }
    spec.spellings.push_back(std::move(spelling));
  }
  if (spec.spellings.empty()) {
    call.fault("ArgumentError", "ArgParser option requires a spelling");
    return false;
  }

  bool present = false;
  std::string name;
  if (!optional_string_keyword(call, "name", &name, &present)) {
    return false;
  }
  spec.name = present ? std::move(name)
                      : derive_name_from_spelling(spec.spellings.back());
  if (!optional_type_keyword(call, spec.type, &spec.type)) {
    return false;
  }
  bool required = false;
  if (!call.bool_keyword("required", false, &required)) {
    return false;
  }
  spec.required = required;
  bool multiple = false;
  if (!call.bool_keyword("multiple", false, &multiple)) {
    return false;
  }
  spec.multiple = multiple;
  bool negatable = false;
  if (!call.bool_keyword("negatable", false, &negatable)) {
    return false;
  }
  spec.negatable = kind == SpecKind::Flag && negatable;
  const std::optional<Value> default_value = call.keyword("default");
  if (default_value.has_value()) {
    spec.has_default = true;
    spec.default_value = *default_value;
  }
  if (!optional_choices_keyword(call, &spec)) {
    return false;
  }
  std::string env;
  if (!optional_string_keyword(call, "env", &env, &present)) {
    return false;
  }
  if (present) {
    spec.env = std::move(env);
  }
  spec.block = call.block;
  parser->specs.push_back(std::move(spec));
  *call.out = call.receiver;
  return true;
}

bool append_positional_spec(NativeStdlibCall &call, Parser *parser,
                            SpecKind kind) {
  if (!call.reject_unknown_keywords(
          {"type", "default", "required", "choices", "multiple", "env"})) {
    return false;
  }
  if (!call.require_arity(1)) {
    return false;
  }
  Spec spec;
  spec.kind = kind;
  spec.type = RuntimeNativeTypeKind::Str;
  if (!value_to_text(call, call.args[0], "argument name", &spec.name)) {
    return false;
  }
  if (!optional_type_keyword(call, spec.type, &spec.type)) {
    return false;
  }
  bool required = kind == SpecKind::Positional;
  if (!call.bool_keyword("required", required, &required)) {
    return false;
  }
  spec.required = required;
  spec.multiple = kind == SpecKind::Rest;
  bool multiple = spec.multiple;
  if (!call.bool_keyword("multiple", multiple, &multiple)) {
    return false;
  }
  spec.multiple = kind == SpecKind::Rest || multiple;
  const std::optional<Value> default_value = call.keyword("default");
  if (default_value.has_value()) {
    spec.has_default = true;
    spec.default_value = *default_value;
  }
  if (!optional_choices_keyword(call, &spec)) {
    return false;
  }
  bool present = false;
  std::string env;
  if (!optional_string_keyword(call, "env", &env, &present)) {
    return false;
  }
  if (present) {
    spec.env = std::move(env);
  }
  spec.block = call.block;
  parser->specs.push_back(std::move(spec));
  *call.out = call.receiver;
  return true;
}

enum class ParseMode { Result, Strict, Cli };

SendStatus parse_mode(NativeStdlibCall &call, ParseMode mode) {
  if (!call.require_arity(0) || !call.reject_unknown_keywords({"cmdline"}) ||
      !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  const std::shared_ptr<Parser> parser = call.receiver.as_arg_parser();
  if (parser == nullptr) {
    return call.fault("TypeError", "ArgParser receiver is null");
  }
  std::vector<std::string> cmdline;
  if (!parser_cmdline_from_keyword(call, *parser, &cmdline)) {
    return SendStatus::Faulted;
  }
  ParseResult result = parse_with_cmdline(call, *parser, cmdline);
  if (result.status == ParseResult::Status::Faulted) {
    return SendStatus::Faulted;
  }
  if (result.status == ParseResult::Status::Error) {
    ParseError structured_error = result.error;
    if (structured_error.help.empty()) {
      structured_error.help = help_text(*parser);
    }
    Value exception = result.exception.is_null()
                          ? make_error_value(call, structured_error)
                          : result.exception;
    if (exception.is_null()) {
      return SendStatus::Faulted;
    }
    if (mode == ParseMode::Result) {
      *call.out = make_result_value(false, std::move(exception));
      return SendStatus::Matched;
    }
    if (mode == ParseMode::Cli) {
      const std::shared_ptr<ErrorInstanceValue> instance =
          exception.as_error_instance();
      if (error_is_a(exception, "ArgParser.HelpRequested")) {
        std::string rendered = help_text(*parser);
        if (const std::optional<Value> help = error_field(exception, "help")) {
          if (const std::optional<std::string> text = call.text_of(*help)) {
            rendered = *text;
          }
        }
        if (!call.write_stdout(rendered + "\n")) {
          return SendStatus::Faulted;
        }
        *call.out = Value::null();
        return SendStatus::Matched;
      }
      std::string usage = help_text(*parser);
      if (const std::optional<Value> field = error_field(exception, "usage")) {
        if (const std::optional<std::string> text = call.text_of(*field)) {
          usage = *text;
        }
      }
      const std::string program =
          parser->name.empty() ? "program" : parser->name;
      const std::string message =
          instance == nullptr ? "argument parsing failed" : instance->message;
      if (!call.write_stderr(usage + "\n\n" + program + ": error: " + message +
                             "\n")) {
        return SendStatus::Faulted;
      }
      return call.fault(instance == nullptr
                            ? "ArgParser.ParseError"
                            : runtime_error_name(instance->error_id),
                        message);
    }
    return call.raise(std::move(exception));
  }
  *call.out = mode == ParseMode::Result ? make_result_value(true, result.value)
                                        : result.value;
  return SendStatus::Matched;
}

SendStatus parser_dispatch(NativeStdlibCall &call) {
  if (call.receiver.is_native_type()) {
    if (call.selector == "new") {
      return construct_parser(call);
    }
    return SendStatus::NotHandled;
  }
  if (!call.receiver.is_arg_parser()) {
    return SendStatus::NotHandled;
  }
  const std::shared_ptr<Parser> parser = call.receiver.as_arg_parser();
  if (parser == nullptr) {
    return call.fault("TypeError", "ArgParser receiver is null");
  }

  if (call.selector == "name") {
    if (!call.require_arity(1) || !call.reject_unknown_keywords({}) ||
        !call.require_no_block()) {
      return SendStatus::Faulted;
    }
    if (!value_to_text(call, call.args[0], "name", &parser->name)) {
      return SendStatus::Faulted;
    }
    *call.out = call.receiver;
    return SendStatus::Matched;
  }
  if (call.selector == "about") {
    if (!call.require_arity(1) || !call.reject_unknown_keywords({}) ||
        !call.require_no_block()) {
      return SendStatus::Faulted;
    }
    if (!value_to_text(call, call.args[0], "about", &parser->about)) {
      return SendStatus::Faulted;
    }
    *call.out = call.receiver;
    return SendStatus::Matched;
  }
  if (call.selector == "arg") {
    return append_option_spec(call, parser.get(), SpecKind::Option)
               ? SendStatus::Matched
               : SendStatus::Faulted;
  }
  if (call.selector == "flag") {
    return append_option_spec(call, parser.get(), SpecKind::Flag)
               ? SendStatus::Matched
               : SendStatus::Faulted;
  }
  if (call.selector == "pos") {
    return append_positional_spec(call, parser.get(), SpecKind::Positional)
               ? SendStatus::Matched
               : SendStatus::Faulted;
  }
  if (call.selector == "rest") {
    return append_positional_spec(call, parser.get(), SpecKind::Rest)
               ? SendStatus::Matched
               : SendStatus::Faulted;
  }
  if (call.selector == "try_parse") {
    return parse_mode(call, ParseMode::Result);
  }
  if (call.selector == "parse_or_raise") {
    return parse_mode(call, ParseMode::Strict);
  }
  if (call.selector == "parse") {
    return parse_mode(call, ParseMode::Cli);
  }
  return SendStatus::NotHandled;
}

} // namespace

void register_argparser(NativeRegistry &registry) {
  registry.register_path("ArgParser", RuntimeNativeTypeKind::ArgParser);
  registry.register_handler(RuntimeNativeTypeKind::ArgParser, &parser_dispatch);
}

} // namespace amber::runtime
