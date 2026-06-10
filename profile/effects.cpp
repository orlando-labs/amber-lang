#include "profile/effects.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace amber::effect {

namespace {

std::string trim(const std::string &value) {
  std::size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (const char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
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
      out << c;
      break;
    }
  }
  return out.str();
}

const std::set<std::string> &canonical_name_set() {
  static const std::set<std::string> names = {
      "alloc", "async",  "db",     "env",      "ffi",    "fs",     "gpu",
      "mut",   "net",    "random", "reflect",  "schema", "strand", "time",
      "trace", "unsafe", "watch",  "workflow", "world"};
  return names;
}

bool valid_name_char(char c) {
  const unsigned char ch = static_cast<unsigned char>(c);
  return std::isalnum(ch) != 0 || c == '.' || c == '_' || c == '-';
}

bool valid_vendor_name(const std::string &name) {
  if (name.size() < 5U || name.find('.') == std::string::npos) {
    return false;
  }
  if (name.front() == '.' || name.back() == '.') {
    return false;
  }
  std::size_t components = 1;
  for (char c : name) {
    if (!valid_name_char(c)) {
      return false;
    }
    if (c == '.') {
      ++components;
    }
  }
  return components >= 3U;
}

EffectDiagnostic diagnostic(std::string error_name, std::string message,
                            std::string owner = {}, std::string effect = {}) {
  EffectDiagnostic out;
  out.error_name = std::move(error_name);
  out.message = std::move(message);
  out.owner = std::move(owner);
  out.effect = std::move(effect);
  return out;
}

std::vector<std::string> sorted_effects(std::vector<std::string> effects) {
  std::sort(effects.begin(), effects.end());
  effects.erase(std::unique(effects.begin(), effects.end()), effects.end());
  return effects;
}

void emit_string_array(std::ostringstream &out, const char *name,
                       const std::vector<std::string> &values,
                       bool trailing_comma) {
  out << "  \"" << name << "\": [";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\"" << json_escape(values[i]) << "\"";
  }
  out << "]";
  if (trailing_comma) {
    out << ",";
  }
  out << "\n";
}

void emit_effect_array(std::ostringstream &out, const char *name,
                       const std::vector<EffectSummary> &summaries,
                       bool trailing_comma) {
  out << "  \"" << name << "\": [";
  for (std::size_t i = 0; i < summaries.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"owner\":\"" << json_escape(summaries[i].owner)
        << "\",\"kind\":\"" << json_escape(summaries[i].kind)
        << "\",\"declared\":\""
        << json_escape(effect_row_to_text(summaries[i].declared_effects))
        << "\",\"observed\":\""
        << json_escape(effect_row_to_text(summaries[i].observed_effects))
        << "\",\"flags\":" << summaries[i].flags << "}";
  }
  out << "\n  ]";
  if (trailing_comma) {
    out << ",";
  }
  out << "\n";
}

} // namespace

std::vector<std::string> canonical_effect_names() {
  return std::vector<std::string>(canonical_name_set().begin(),
                                  canonical_name_set().end());
}

std::string canonical_effect_name(const std::string &name) {
  static const std::map<std::string, std::string> aliases = {
      {"accelerator", "gpu"},      {"device.accelerator", "gpu"},
      {"device.gpu", "gpu"},       {"fs.read", "fs"},
      {"fs.write", "fs"},          {"fs.metadata", "fs"},
      {"fs_read", "fs"},           {"fs_write", "fs"},
      {"fs_metadata", "fs"},       {"io_wait", "async"},
      {"net.connect", "net"},      {"net.listen", "net"},
      {"net_connect", "net"},      {"net_listen", "net"},
      {"net_accept", "net"},       {"net_udp", "net"},
      {"env.read", "env"},         {"env.write", "env"},
      {"time.now", "time"},        {"time.sleep", "time"},
      {"random.secure", "random"}, {"random.pseudo", "random"},
      {"ffi.call", "ffi"},         {"ffi.load", "ffi"},
      {"db.connect", "db"},        {"notebook.watch", "watch"},
      {"trace.emit", "trace"},     {"workflow.persist", "workflow"}};
  const std::string value = trim(name);
  const auto found = aliases.find(value);
  if (found != aliases.end()) {
    return found->second;
  }
  return value;
}

bool valid_effect_name(const std::string &name) {
  const std::string canonical = canonical_effect_name(name);
  if (canonical_name_set().find(canonical) != canonical_name_set().end()) {
    return true;
  }
  return valid_vendor_name(canonical);
}

std::vector<std::string> normalize_effects(std::vector<std::string> effects) {
  std::vector<std::string> out;
  out.reserve(effects.size());
  for (const std::string &effect : effects) {
    const std::string canonical = canonical_effect_name(effect);
    if (canonical.empty() || canonical == "pure") {
      continue;
    }
    out.push_back(canonical);
  }
  return sorted_effects(std::move(out));
}

std::string effect_row_to_text(const std::vector<std::string> &effects) {
  const std::vector<std::string> sorted = normalize_effects(
      std::vector<std::string>(effects.begin(), effects.end()));
  std::ostringstream out;
  out << "!{";
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    if (i != 0U) {
      out << ", ";
    }
    out << sorted[i];
  }
  out << "}";
  return out.str();
}

bool parse_effect_row(const std::string &raw, std::vector<std::string> *effects,
                      std::vector<EffectDiagnostic> *diagnostics,
                      const std::string &owner) {
  std::string text = trim(raw);
  if (text.size() >= 3U && text[0] == '!' && text[1] == '{' &&
      text.back() == '}') {
    text = text.substr(2, text.size() - 3U);
  } else if (text.size() >= 2U && text.front() == '{' && text.back() == '}') {
    text = text.substr(1, text.size() - 2U);
  }

  std::vector<std::string> parsed;
  bool saw_pure = false;
  bool ok = true;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::string item = trim(text.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!item.empty()) {
      const std::string canonical = canonical_effect_name(item);
      if (canonical == "pure") {
        saw_pure = true;
      } else if (!valid_effect_name(canonical)) {
        ok = false;
        if (diagnostics != nullptr) {
          diagnostics->push_back(diagnostic(
              "EffectRowError", "unknown effect label: " + item, owner, item));
        }
      } else {
        parsed.push_back(canonical);
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1U;
  }

  if (saw_pure && !parsed.empty()) {
    ok = false;
    if (diagnostics != nullptr) {
      diagnostics->push_back(diagnostic(
          "EffectRowError", "pure effect row cannot include other effects",
          owner, "pure"));
    }
  }

  if (effects != nullptr) {
    *effects = saw_pure ? std::vector<std::string>{}
                        : normalize_effects(std::move(parsed));
  }
  return ok;
}

EffectSummary make_effect_summary(std::string owner, std::string kind,
                                  std::vector<std::string> declared_effects,
                                  std::vector<std::string> observed_effects,
                                  bool has_declared_row) {
  EffectSummary summary;
  summary.owner = std::move(owner);
  summary.kind = std::move(kind);
  summary.declared_effects = normalize_effects(std::move(declared_effects));
  summary.observed_effects = normalize_effects(std::move(observed_effects));
  summary.flags = has_declared_row ? kEffectSummaryFlagDeclared : 0U;
  return summary;
}

bool effects_subset_of(const std::vector<std::string> &actual,
                       const std::vector<std::string> &allowed) {
  const std::vector<std::string> actual_normalized =
      normalize_effects(std::vector<std::string>(actual.begin(), actual.end()));
  const std::vector<std::string> allowed_normalized = normalize_effects(
      std::vector<std::string>(allowed.begin(), allowed.end()));
  return std::all_of(actual_normalized.begin(), actual_normalized.end(),
                     [&](const std::string &effect) {
                       return std::find(allowed_normalized.begin(),
                                        allowed_normalized.end(),
                                        effect) != allowed_normalized.end();
                     });
}

EffectValidationResult
validate_effect_summaries(const std::vector<EffectSummary> &summaries,
                          const std::vector<std::string> &allowed_effects,
                          bool enforce_allowed_effects) {
  EffectValidationResult result;
  result.allowed_effects = normalize_effects(
      std::vector<std::string>(allowed_effects.begin(), allowed_effects.end()));
  for (const EffectSummary &summary : summaries) {
    EffectSummary normalized =
        make_effect_summary(summary.owner, summary.kind,
                            summary.declared_effects, summary.observed_effects,
                            (summary.flags & kEffectSummaryFlagDeclared) != 0U);
    result.summaries.push_back(normalized);
    if ((normalized.flags & kEffectSummaryFlagDeclared) != 0U &&
        !effects_subset_of(normalized.observed_effects,
                           normalized.declared_effects)) {
      result.diagnostics.push_back(
          diagnostic("EffectViolationError",
                     "observed effects " +
                         effect_row_to_text(normalized.observed_effects) +
                         " exceed declared row " +
                         effect_row_to_text(normalized.declared_effects),
                     normalized.owner));
    }
    if (enforce_allowed_effects &&
        !effects_subset_of(normalized.observed_effects,
                           result.allowed_effects)) {
      result.diagnostics.push_back(
          diagnostic("EffectViolationError",
                     "observed effects " +
                         effect_row_to_text(normalized.observed_effects) +
                         " exceed host allowance " +
                         effect_row_to_text(result.allowed_effects),
                     normalized.owner));
    }
  }
  std::sort(result.summaries.begin(), result.summaries.end(),
            [](const EffectSummary &left, const EffectSummary &right) {
              if (left.owner != right.owner) {
                return left.owner < right.owner;
              }
              return left.kind < right.kind;
            });
  result.ok = result.diagnostics.empty();
  return result;
}

std::string validation_to_json(const EffectValidationResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.effects.v1\",\n";
  out << "  \"status\": \"" << (result.ok ? "ok" : "error") << "\",\n";
  emit_effect_array(out, "summaries", result.summaries, true);
  emit_string_array(out, "allowed_effects", result.allowed_effects, true);
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error_name\":\""
        << json_escape(result.diagnostics[i].error_name) << "\",\"message\":\""
        << json_escape(result.diagnostics[i].message) << "\",\"owner\":\""
        << json_escape(result.diagnostics[i].owner) << "\",\"effect\":\""
        << json_escape(result.diagnostics[i].effect) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::effect
