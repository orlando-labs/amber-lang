#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber::effect {

inline constexpr std::uint32_t kEffectSummaryFlagDeclared = 0x1U;

struct EffectSummary {
  std::string owner;
  std::string kind;
  std::vector<std::string> declared_effects;
  std::vector<std::string> observed_effects;
  std::uint32_t flags = 0;
};

struct EffectDiagnostic {
  std::string error_name;
  std::string message;
  std::string owner;
  std::string effect;
};

struct EffectValidationResult {
  bool ok = false;
  std::vector<EffectSummary> summaries;
  std::vector<std::string> allowed_effects;
  std::vector<EffectDiagnostic> diagnostics;
};

std::vector<std::string> canonical_effect_names();
std::string canonical_effect_name(const std::string &name);
bool valid_effect_name(const std::string &name);

std::vector<std::string> normalize_effects(std::vector<std::string> effects);
std::string effect_row_to_text(const std::vector<std::string> &effects);
bool parse_effect_row(const std::string &raw, std::vector<std::string> *effects,
                      std::vector<EffectDiagnostic> *diagnostics = nullptr,
                      const std::string &owner = {});

EffectSummary make_effect_summary(std::string owner, std::string kind,
                                  std::vector<std::string> declared_effects,
                                  std::vector<std::string> observed_effects,
                                  bool has_declared_row);

bool effects_subset_of(const std::vector<std::string> &actual,
                       const std::vector<std::string> &allowed);
EffectValidationResult
validate_effect_summaries(const std::vector<EffectSummary> &summaries,
                          const std::vector<std::string> &allowed_effects = {},
                          bool enforce_allowed_effects = false);

std::string validation_to_json(const EffectValidationResult &result);

} // namespace amber::effect
