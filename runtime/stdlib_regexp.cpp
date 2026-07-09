#include "runtime/stdlib_regexp.h"

#include <cctype>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace amber::runtime {

namespace {

Value regexp_pattern_value(std::shared_ptr<RuntimeRegexpPatternValue> pattern) {
  return Value::regexp_pattern(std::move(pattern));
}

Value regexp_match_value(std::shared_ptr<RuntimeRegexpMatchValue> match) {
  return Value::regexp_match(std::move(match));
}

std::optional<std::string> required_text(NativeStdlibCall &call,
                                         const Value &value,
                                         const std::string &context) {
  const std::optional<std::string> text = call.text_of(value);
  if (!text.has_value()) {
    call.fault("TypeError", context + " must be Str or Symbol");
    return std::nullopt;
  }
  return text;
}

std::optional<std::regex::flag_type>
regexp_flags_from_value(NativeStdlibCall &call, const Value &value) {
  const std::optional<std::string> text = required_text(call, value, "flags");
  if (!text.has_value()) {
    return std::nullopt;
  }
  std::regex::flag_type flags = std::regex::ECMAScript;
  for (char c : *text) {
    if (c == 'i') {
      flags |= std::regex::icase;
      continue;
    }
    call.fault("ArgumentError", "Regexp flags currently support only `i`");
    return std::nullopt;
  }
  return flags;
}

std::optional<std::shared_ptr<RuntimeRegexpPatternValue>>
compile_regexp(NativeStdlibCall &call, const std::string &source,
               std::regex::flag_type flags) {
  try {
    auto pattern = std::make_shared<RuntimeRegexpPatternValue>();
    pattern->source = source;
    pattern->flags = flags;
    pattern->compiled = std::regex(source, flags);
    return pattern;
  } catch (const std::regex_error &error) {
    call.fault("RegexpCompileError",
               std::string("invalid regexp: ") + error.what());
    return std::nullopt;
  }
}

std::optional<std::shared_ptr<RuntimeRegexpPatternValue>>
regexp_pattern_arg(NativeStdlibCall &call, const Value &value,
                   const std::string &context) {
  if (!value.is_regexp_pattern()) {
    call.fault("TypeError", context + " expects Regexp.Pattern");
    return std::nullopt;
  }
  const std::shared_ptr<RuntimeRegexpPatternValue> pattern =
      value.as_regexp_pattern();
  if (pattern == nullptr) {
    call.fault("TypeError", "Regexp.Pattern value is null");
    return std::nullopt;
  }
  return pattern;
}

std::shared_ptr<RuntimeRegexpMatchValue>
make_match(const std::shared_ptr<RuntimeRegexpPatternValue> &pattern,
           const std::string &source, const std::smatch &match,
           std::size_t base_offset) {
  auto value = std::make_shared<RuntimeRegexpMatchValue>();
  value->source = source;
  value->pattern = pattern;
  value->captures.reserve(match.size());
  for (std::size_t i = 0; i < match.size(); ++i) {
    RuntimeRegexpCaptureRange capture;
    capture.matched = match[i].matched;
    if (capture.matched) {
      capture.start = base_offset + static_cast<std::size_t>(match.position(i));
      capture.end = capture.start + static_cast<std::size_t>(match.length(i));
    }
    value->captures.push_back(capture);
  }
  return value;
}

std::optional<std::shared_ptr<RuntimeRegexpMatchValue>>
search_regexp(NativeStdlibCall &call,
              const std::shared_ptr<RuntimeRegexpPatternValue> &pattern,
              const std::string &source, bool full_match) {
  try {
    std::smatch match;
    const bool matched =
        full_match ? std::regex_match(source, match, pattern->compiled)
                   : std::regex_search(source, match, pattern->compiled);
    if (!matched) {
      return std::nullopt;
    }
    return make_match(pattern, source, match, 0U);
  } catch (const std::regex_error &error) {
    call.fault("RegexpError",
               std::string("regexp match failed: ") + error.what());
    return std::nullopt;
  }
}

std::optional<std::string> group_text(const RuntimeRegexpMatchValue &match,
                                      std::size_t index) {
  if (index >= match.captures.size()) {
    return std::nullopt;
  }
  const RuntimeRegexpCaptureRange &capture = match.captures[index];
  if (!capture.matched || capture.start > capture.end ||
      capture.end > match.source.size()) {
    return std::nullopt;
  }
  return match.source.substr(capture.start, capture.end - capture.start);
}

std::optional<std::size_t> replacement_limit(NativeStdlibCall &call) {
  const std::optional<Value> value = call.keyword("count");
  if (!value.has_value()) {
    return std::numeric_limits<std::size_t>::max();
  }
  if (value->is_integer()) {
    if (value->as_integer() < 0) {
      call.fault("ArgumentError", "replace count must be non-negative");
      return std::nullopt;
    }
    return static_cast<std::size_t>(value->as_integer());
  }
  const std::optional<std::string> text = call.text_of(*value);
  if (text.has_value() && *text == "all") {
    return std::numeric_limits<std::size_t>::max();
  }
  call.fault("TypeError", "replace count must be Int or :all");
  return std::nullopt;
}

std::optional<std::size_t> parse_group_index(const std::string &text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t value = 0;
  for (char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return std::nullopt;
    }
    const std::size_t digit = static_cast<std::size_t>(c - '0');
    if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
      return std::nullopt;
    }
    value = value * 10U + digit;
  }
  return value;
}

std::optional<std::string>
expand_replacement(NativeStdlibCall &call, const RuntimeRegexpMatchValue &match,
                   const std::string &replacement) {
  std::string out;
  out.reserve(replacement.size());
  for (std::size_t i = 0; i < replacement.size();) {
    if (replacement[i] != '$') {
      out.push_back(replacement[i++]);
      continue;
    }
    if (i + 1U >= replacement.size()) {
      out.push_back('$');
      ++i;
      continue;
    }
    const char next = replacement[i + 1U];
    if (next == '$') {
      out.push_back('$');
      i += 2U;
      continue;
    }
    if (next == '&') {
      if (const std::optional<std::string> text = group_text(match, 0U)) {
        out += *text;
      }
      i += 2U;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(next))) {
      std::size_t cursor = i + 1U;
      while (cursor < replacement.size() &&
             std::isdigit(static_cast<unsigned char>(replacement[cursor]))) {
        ++cursor;
      }
      const std::optional<std::size_t> index =
          parse_group_index(replacement.substr(i + 1U, cursor - (i + 1U)));
      if (index.has_value()) {
        if (const std::optional<std::string> text = group_text(match, *index)) {
          out += *text;
        }
      }
      i = cursor;
      continue;
    }
    if (next == '{') {
      const std::size_t close = replacement.find('}', i + 2U);
      if (close == std::string::npos) {
        call.fault("RegexpError", "unterminated replacement group name");
        return std::nullopt;
      }
      const std::string name = replacement.substr(i + 2U, close - (i + 2U));
      const std::optional<std::size_t> index = parse_group_index(name);
      if (!index.has_value()) {
        call.fault("RegexpError",
                   "named regexp replacement groups are not available with "
                   "the current engine");
        return std::nullopt;
      }
      if (const std::optional<std::string> text = group_text(match, *index)) {
        out += *text;
      }
      i = close + 1U;
      continue;
    }
    out.push_back('$');
    ++i;
  }
  return out;
}

SendStatus regexp_compile(NativeStdlibCall &call) {
  if (call.args.size() != 1U || !call.require_no_block() ||
      !call.reject_unknown_keywords({"flags"})) {
    if (call.args.size() != 1U) {
      return call.fault("TypeError", "Regexp.compile expects one Str");
    }
    return SendStatus::Faulted;
  }
  const std::optional<std::string> source =
      required_text(call, call.args[0], "Regexp source");
  if (!source.has_value()) {
    return SendStatus::Faulted;
  }
  std::regex::flag_type flags = std::regex::ECMAScript;
  if (const std::optional<Value> flag_value = call.keyword("flags")) {
    const std::optional<std::regex::flag_type> parsed =
        regexp_flags_from_value(call, *flag_value);
    if (!parsed.has_value()) {
      return SendStatus::Faulted;
    }
    flags = *parsed;
  }
  const std::optional<std::shared_ptr<RuntimeRegexpPatternValue>> pattern =
      compile_regexp(call, *source, flags);
  if (!pattern.has_value()) {
    return SendStatus::Faulted;
  }
  *call.out = regexp_pattern_value(*pattern);
  return SendStatus::Matched;
}

SendStatus regexp_escape(NativeStdlibCall &call) {
  if (!call.require_arity(1) || !call.require_no_block() ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::string> source =
      required_text(call, call.args[0], "Regexp.escape argument");
  if (!source.has_value()) {
    return SendStatus::Faulted;
  }
  std::string out;
  out.reserve(source->size() * 2U);
  for (char c : *source) {
    switch (c) {
    case '\\':
    case '^':
    case '$':
    case '.':
    case '|':
    case '?':
    case '*':
    case '+':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
      out.push_back('\\');
      break;
    default:
      break;
    }
    out.push_back(c);
  }
  *call.out = call.string_value(std::move(out));
  return SendStatus::Matched;
}

SendStatus regexp_pattern_dispatch(NativeStdlibCall &call) {
  const std::shared_ptr<RuntimeRegexpPatternValue> pattern =
      call.receiver.as_regexp_pattern();
  if (pattern == nullptr) {
    return call.fault("TypeError", "Regexp.Pattern value is null");
  }
  if (call.selector == "source" || call.selector == "to_str" ||
      call.selector == "inspect") {
    if (!call.require_arity(0) || !call.require_no_block() ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out =
        call.selector == "source"
            ? call.string_value(pattern->source)
            : call.string_value(runtime_regexp_pattern_to_string(*pattern));
    return SendStatus::Matched;
  }
  if (call.selector == "match" || call.selector == "find" ||
      call.selector == "match?" || call.selector == "matches?" ||
      call.selector == "full_match" || call.selector == "full_match?" ||
      call.selector == "=~" || call.selector == "!~") {
    if (!call.require_arity(1) || !call.require_no_block() ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    const std::optional<std::string> source =
        required_text(call, call.args[0], "Regexp match input");
    if (!source.has_value()) {
      return SendStatus::Faulted;
    }
    const bool full =
        call.selector == "full_match" || call.selector == "full_match?";
    const std::optional<std::shared_ptr<RuntimeRegexpMatchValue>> match =
        search_regexp(call, pattern, *source, full);
    if (call.selector == "match?" || call.selector == "matches?" ||
        call.selector == "full_match?" || call.selector == "!~") {
      *call.out = Value::boolean(call.selector == "!~" ? !match.has_value()
                                                       : match.has_value());
      return SendStatus::Matched;
    }
    *call.out = match.has_value() ? regexp_match_value(*match) : Value::null();
    return SendStatus::Matched;
  }
  return SendStatus::NotHandled;
}

SendStatus regexp_match_dispatch(NativeStdlibCall &call) {
  const std::shared_ptr<RuntimeRegexpMatchValue> match =
      call.receiver.as_regexp_match();
  if (match == nullptr) {
    return call.fault("TypeError", "Regexp.Match value is null");
  }
  if (call.selector == "text" || call.selector == "to_str" ||
      call.selector == "inspect") {
    if (!call.require_arity(0) || !call.require_no_block() ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = call.string_value(runtime_regexp_match_to_string(*match));
    return SendStatus::Matched;
  }
  if (call.selector == "pattern") {
    if (!call.require_arity(0) || !call.require_no_block() ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = regexp_pattern_value(match->pattern);
    return SendStatus::Matched;
  }
  if (call.selector == "source") {
    if (!call.require_arity(0) || !call.require_no_block() ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = call.string_value(match->source);
    return SendStatus::Matched;
  }
  if (call.selector == "count" || call.selector == "size" ||
      call.selector == "length") {
    if (!call.require_arity(0) || !call.require_no_block() ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    const std::size_t captures =
        match->captures.empty() ? 0U : match->captures.size() - 1U;
    *call.out = Value::integer(static_cast<std::int64_t>(captures));
    return SendStatus::Matched;
  }
  if (call.selector == "[]" || call.selector == "group") {
    if (!call.require_arity(1) || !call.require_no_block() ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    if (!call.args[0].is_integer() || call.args[0].as_integer() < 0) {
      return call.fault("TypeError", "regexp group index must be Int >= 0");
    }
    const std::optional<std::string> text =
        group_text(*match, static_cast<std::size_t>(call.args[0].as_integer()));
    *call.out = text.has_value() ? call.string_value(*text) : Value::null();
    return SendStatus::Matched;
  }
  if (call.selector == "start" || call.selector == "finish" ||
      call.selector == "end") {
    if (!call.require_arity(0) || !call.require_no_block() ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    if (match->captures.empty() || !match->captures[0].matched) {
      *call.out = Value::null();
      return SendStatus::Matched;
    }
    *call.out = Value::integer(static_cast<std::int64_t>(
        call.selector == "start" ? match->captures[0].start
                                 : match->captures[0].end));
    return SendStatus::Matched;
  }
  if (call.selector == "captures") {
    if (!call.require_arity(0) || !call.require_no_block() ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    std::vector<Value> captures;
    for (std::size_t i = 1; i < match->captures.size(); ++i) {
      const std::optional<std::string> text = group_text(*match, i);
      captures.push_back(text.has_value() ? call.string_value(*text)
                                          : Value::null());
    }
    *call.out = call.make_list(std::move(captures));
    return SendStatus::Matched;
  }
  return SendStatus::NotHandled;
}

SendStatus regexp_dispatch(NativeStdlibCall &call) {
  if (call.kind != RuntimeNativeTypeKind::Regexp) {
    return SendStatus::NotHandled;
  }
  if (call.receiver.is_regexp_pattern()) {
    return regexp_pattern_dispatch(call);
  }
  if (call.receiver.is_regexp_match()) {
    return regexp_match_dispatch(call);
  }
  if (call.selector == "compile" || call.selector == "new" ||
      call.selector == "r" || call.selector == "__call__") {
    return regexp_compile(call);
  }
  if (call.selector == "escape") {
    return regexp_escape(call);
  }
  return SendStatus::NotHandled;
}

} // namespace

std::string
runtime_regexp_pattern_to_string(const RuntimeRegexpPatternValue &pattern) {
  return "/" + pattern.source + "/";
}

std::string
runtime_regexp_match_to_string(const RuntimeRegexpMatchValue &match) {
  const std::optional<std::string> text = group_text(match, 0U);
  return text.value_or(std::string{});
}

SendStatus regexp_string_match_operator(NativeStdlibCall &call,
                                        const std::string &self) {
  if (call.selector != "=~" && call.selector != "!~") {
    return SendStatus::NotHandled;
  }
  if (call.args.size() != 1U || !call.block.is_null() ||
      !call.reject_unknown_keywords({})) {
    if (call.args.size() != 1U) {
      return call.fault("TypeError", "regexp match operator expects one arg");
    }
    if (!call.block.is_null()) {
      return call.fault("TypeError", "regexp match operator takes no block");
    }
    return SendStatus::Faulted;
  }
  if (!call.args[0].is_regexp_pattern()) {
    return SendStatus::NotHandled;
  }
  const std::optional<std::shared_ptr<RuntimeRegexpPatternValue>> pattern =
      regexp_pattern_arg(call, call.args[0], "regexp match operator");
  if (!pattern.has_value()) {
    return SendStatus::Faulted;
  }
  const std::optional<std::shared_ptr<RuntimeRegexpMatchValue>> match =
      search_regexp(call, *pattern, self, false);
  if (call.selector == "!~") {
    *call.out = Value::boolean(!match.has_value());
  } else {
    *call.out = match.has_value() ? regexp_match_value(*match) : Value::null();
  }
  return SendStatus::Matched;
}

SendStatus regexp_string_replace(NativeStdlibCall &call,
                                 const std::string &self) {
  if (call.selector != "replace" && call.selector != "replaced") {
    return SendStatus::NotHandled;
  }
  if (call.args.empty() || !call.args[0].is_regexp_pattern()) {
    return SendStatus::NotHandled;
  }
  if (!call.reject_unknown_keywords({"count"})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::shared_ptr<RuntimeRegexpPatternValue>> pattern =
      regexp_pattern_arg(call, call.args[0], "Str#replace");
  if (!pattern.has_value()) {
    return SendStatus::Faulted;
  }
  const std::optional<std::size_t> limit = replacement_limit(call);
  if (!limit.has_value()) {
    return SendStatus::Faulted;
  }

  std::optional<std::string> replacement;
  if (call.block.is_null()) {
    if (call.args.size() != 2U) {
      return call.fault("TypeError",
                        "regexp replace expects pattern and replacement");
    }
    replacement = required_text(call, call.args[1], "replacement");
    if (!replacement.has_value()) {
      return SendStatus::Faulted;
    }
  } else if (call.args.size() != 1U) {
    return call.fault("TypeError",
                      "regexp block replace expects only the pattern arg");
  } else if (call.block_suspension_in_property_arm("regexp replace block")) {
    return SendStatus::Faulted;
  }

  if (*limit == 0U) {
    *call.out = call.string_value(self);
    return SendStatus::Matched;
  }

  std::string out;
  std::size_t last_append = 0;
  std::size_t search_offset = 0;
  std::size_t replaced = 0;

  try {
    while (replaced < *limit && search_offset <= self.size()) {
      std::smatch match;
      const auto begin =
          self.cbegin() + static_cast<std::ptrdiff_t>(search_offset);
      if (!std::regex_search(begin, self.cend(), match, (*pattern)->compiled)) {
        break;
      }
      const std::size_t match_start =
          search_offset + static_cast<std::size_t>(match.position(0));
      const std::size_t match_end =
          match_start + static_cast<std::size_t>(match.length(0));
      out.append(self, last_append, match_start - last_append);
      const std::shared_ptr<RuntimeRegexpMatchValue> match_value =
          make_match(*pattern, self, match, search_offset);

      std::string subst;
      if (replacement.has_value()) {
        const std::optional<std::string> expanded =
            expand_replacement(call, *match_value, *replacement);
        if (!expanded.has_value()) {
          return SendStatus::Faulted;
        }
        subst = *expanded;
      } else {
        StdlibBlockResult block =
            call.call_block(call.block, {regexp_match_value(match_value)});
        if (block.status == StdlibBlockStatus::Returned) {
          const std::optional<std::string> text =
              required_text(call, block.value, "regexp replace block result");
          if (!text.has_value()) {
            return SendStatus::Faulted;
          }
          subst = *text;
        } else if (block.status == StdlibBlockStatus::Raised) {
          return call.raise(block.exception);
        } else {
          return SendStatus::Faulted;
        }
      }
      out += subst;
      ++replaced;
      last_append = match_end;
      if (match_start == match_end) {
        if (match_end >= self.size()) {
          break;
        }
        search_offset = match_end + 1U;
      } else {
        search_offset = match_end;
      }
    }
  } catch (const std::regex_error &error) {
    return call.fault("RegexpError",
                      std::string("regexp replace failed: ") + error.what());
  }

  out.append(self, last_append, std::string::npos);
  *call.out = call.string_value(std::move(out));
  return SendStatus::Matched;
}

RuntimeNativeModuleDescriptor regexp_module_descriptor() {
  return {
      {{"Regexp", RuntimeNativeTypeKind::Regexp}},
      {{RuntimeNativeTypeKind::Regexp, &regexp_dispatch}},
      {},
      {{RuntimeNativeTypeKind::Regexp, "__call__"}},
      {{"RegexpError", "Exception"}, {"RegexpCompileError", "RegexpError"}}};
}

void register_regexp(NativeRegistry &registry) {
  register_native_module_descriptor(registry, regexp_module_descriptor());
}

void register_regexp_runtime_module(RuntimeModuleRegistry &modules,
                                    RuntimeDispatchRegistry &dispatch,
                                    RuntimeTypeRegistry &types,
                                    RuntimeErrorRegistry *errors) {
  const RuntimeNativeModuleDescriptor descriptor = regexp_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
  if (errors != nullptr) {
    register_runtime_error_descriptor(*errors, descriptor);
  }
}

} // namespace amber::runtime
