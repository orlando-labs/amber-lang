#include "profile/data.h"

#include "frontend/lexer/token.h"
#include "profile/effects.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace amber::data {

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

std::string line_unescape(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  bool escaping = false;
  for (const char c : value) {
    if (escaping) {
      if (c == 'n') {
        out.push_back('\n');
      } else if (c == 'r') {
        out.push_back('\r');
      } else {
        out.push_back(c);
      }
      escaping = false;
      continue;
    }
    if (c == '\\') {
      escaping = true;
      continue;
    }
    out.push_back(c);
  }
  if (escaping) {
    out.push_back('\\');
  }
  return out;
}

const std::set<std::string> &canonical_type_set() {
  static const std::set<std::string> names = {
      "bool",    "bytes", "date",   "decimal", "float",
      "integer", "json",  "string", "symbol",  "timestamp"};
  return names;
}

const std::set<std::string> &table_op_set() {
  static const std::set<std::string> names = {
      "aggregate",   "collect", "filter", "group_by", "join",  "limit", "map",
      "materialize", "project", "scan",   "sort",     "union", "window"};
  return names;
}

bool valid_name_char(char c) {
  const unsigned char ch = static_cast<unsigned char>(c);
  return std::isalnum(ch) != 0 || c == '_' || c == '-' || c == '.';
}

DataDiagnostic diagnostic(std::string error_name, std::string message,
                          std::string subject = {}, std::string field = {}) {
  DataDiagnostic out;
  out.error_name = std::move(error_name);
  out.message = std::move(message);
  out.subject = std::move(subject);
  out.field = std::move(field);
  return out;
}

template <typename T> std::vector<T> sorted_unique(std::vector<T> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::vector<ColumnDependency>
sorted_dependencies(std::vector<ColumnDependency> deps) {
  std::sort(deps.begin(), deps.end(),
            [](const ColumnDependency &left, const ColumnDependency &right) {
              if (left.table_ref != right.table_ref) {
                return left.table_ref < right.table_ref;
              }
              return left.column_ref < right.column_ref;
            });
  deps.erase(std::unique(deps.begin(), deps.end(),
                         [](const ColumnDependency &left,
                            const ColumnDependency &right) {
                           return left.table_ref == right.table_ref &&
                                  left.column_ref == right.column_ref;
                         }),
             deps.end());
  return deps;
}

bool parse_u32(const std::string &value, std::uint32_t *out) {
  try {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed, 10);
    if (consumed != value.size() || parsed > 0xffffffffUL) {
      return false;
    }
    if (out != nullptr) {
      *out = static_cast<std::uint32_t>(parsed);
    }
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool parse_bool(const std::string &value, bool *out) {
  const std::string text = trim(value);
  if (text == "1" || text == "true" || text == "yes") {
    if (out != nullptr) {
      *out = true;
    }
    return true;
  }
  if (text == "0" || text == "false" || text == "no") {
    if (out != nullptr) {
      *out = false;
    }
    return true;
  }
  return false;
}

std::map<std::string, std::string> parse_lines(const std::string &source) {
  std::map<std::string, std::string> values;
  std::istringstream in(source);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (first) {
      values["schema"] = trim(line);
      first = false;
      continue;
    }
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    const std::size_t equals = trimmed.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    values[trim(trimmed.substr(0, equals))] =
        line_unescape(trimmed.substr(equals + 1U));
  }
  return values;
}

std::string value_or_empty(const std::map<std::string, std::string> &values,
                           const std::string &key) {
  const auto found = values.find(key);
  return found == values.end() ? std::string{} : found->second;
}

bool read_count(const std::map<std::string, std::string> &values,
                const std::string &key, std::uint32_t *count,
                std::vector<DataDiagnostic> *diagnostics,
                bool required = true) {
  const std::string raw = value_or_empty(values, key);
  if (raw.empty()) {
    if (required && diagnostics != nullptr) {
      diagnostics->push_back(
          diagnostic("DataProfileParseError", "missing count: " + key));
    }
    if (count != nullptr) {
      *count = 0;
    }
    return !required;
  }
  if (!parse_u32(raw, count)) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(
          diagnostic("DataProfileParseError", "invalid count: " + key));
    }
    return false;
  }
  return true;
}

bool valid_integer_text(const std::string &value) {
  if (value.empty()) {
    return false;
  }
  std::size_t index = 0;
  if (value[index] == '-' || value[index] == '+') {
    ++index;
  }
  if (index == value.size()) {
    return false;
  }
  for (; index < value.size(); ++index) {
    if (std::isdigit(static_cast<unsigned char>(value[index])) == 0) {
      return false;
    }
  }
  return true;
}

bool valid_float_text(const std::string &value) {
  try {
    std::size_t consumed = 0;
    std::stod(value, &consumed);
    return consumed == value.size();
  } catch (const std::exception &) {
    return false;
  }
}

bool valid_date_text(const std::string &value) {
  if (value.size() != 10U || value[4] != '-' || value[7] != '-') {
    return false;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 4U || i == 7U) {
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(value[i])) == 0) {
      return false;
    }
  }
  return true;
}

bool valid_jsonish_text(const std::string &value) {
  if (value.empty()) {
    return false;
  }
  const char first = value.front();
  return first == '{' || first == '[' || first == '"' || first == '-' ||
         std::isdigit(static_cast<unsigned char>(first)) != 0 ||
         value == "true" || value == "false" || value == "null";
}

bool value_matches_type(const std::string &type, const std::string &value) {
  const std::string canonical = canonical_schema_type(type);
  if (canonical == "string" || canonical == "symbol" || canonical == "bytes") {
    return true;
  }
  if (canonical == "bool") {
    bool ignored = false;
    return parse_bool(value, &ignored);
  }
  if (canonical == "integer") {
    return valid_integer_text(value);
  }
  if (canonical == "float" || canonical == "decimal") {
    return valid_float_text(value);
  }
  if (canonical == "date") {
    return valid_date_text(value);
  }
  if (canonical == "timestamp") {
    const std::size_t t = value.find('T');
    return t == 10U && valid_date_text(value.substr(0, 10U));
  }
  if (canonical == "json") {
    return valid_jsonish_text(value);
  }
  return false;
}

void emit_diagnostics(std::ostringstream &out,
                      const std::vector<DataDiagnostic> &diagnostics) {
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error\":\"" << json_escape(diagnostics[i].error_name)
        << "\",\"message\":\"" << json_escape(diagnostics[i].message)
        << "\",\"subject\":\"" << json_escape(diagnostics[i].subject)
        << "\",\"field\":\"" << json_escape(diagnostics[i].field) << "\"}";
  }
  out << "\n  ]\n";
}

const SchemaDefinition *
find_schema(const std::vector<SchemaDefinition> &schemas,
            const std::string &name, std::uint32_t version) {
  const SchemaDefinition *latest = nullptr;
  for (const SchemaDefinition &schema : schemas) {
    if (schema.name != name) {
      continue;
    }
    if (version != 0U && schema.version == version) {
      return &schema;
    }
    if (version == 0U &&
        (latest == nullptr || latest->version < schema.version)) {
      latest = &schema;
    }
  }
  return latest;
}

std::map<std::string, SchemaField>
field_map_for(const SchemaDefinition &schema) {
  std::map<std::string, SchemaField> fields;
  for (const SchemaField &field : schema.fields) {
    fields[field.name] = field;
  }
  return fields;
}

void validate_record(const SchemaRecord &record,
                     const std::vector<SchemaDefinition> &schemas,
                     std::vector<DataDiagnostic> *diagnostics) {
  const SchemaDefinition *schema =
      find_schema(schemas, record.schema_name, record.version);
  if (schema == nullptr) {
    diagnostics->push_back(diagnostic("SchemaViolationError",
                                      "record references unknown schema",
                                      record.schema_name));
    return;
  }

  const std::map<std::string, SchemaField> fields = field_map_for(*schema);
  std::map<std::string, std::string> values;
  for (const SchemaValue &value : record.values) {
    values[value.field] = value.value;
    const auto field = fields.find(value.field);
    if (field == fields.end()) {
      diagnostics->push_back(
          diagnostic("SchemaViolationError", "record contains unknown field",
                     schema_version_id(*schema), value.field));
      continue;
    }
    if (value.value == "null") {
      if (!field->second.nullable) {
        diagnostics->push_back(
            diagnostic("SchemaViolationError", "field does not allow null",
                       schema_version_id(*schema), value.field));
      }
      continue;
    }
    if (!value_matches_type(field->second.type, value.value)) {
      diagnostics->push_back(diagnostic(
          "SchemaViolationError", "field value does not match schema type",
          schema_version_id(*schema), value.field));
    }
  }

  for (const SchemaField &field : schema->fields) {
    if (!field.required) {
      continue;
    }
    if (values.find(field.name) == values.end() &&
        field.default_value.empty()) {
      diagnostics->push_back(
          diagnostic("SchemaViolationError", "missing required field",
                     schema_version_id(*schema), field.name));
    }
  }
}

void validate_compatible_migration(const SchemaMigration &migration,
                                   const SchemaDefinition &from,
                                   const SchemaDefinition &to,
                                   std::vector<DataDiagnostic> *diagnostics) {
  if (migration.kind == "breaking") {
    return;
  }
  const std::map<std::string, SchemaField> from_fields = field_map_for(from);
  const std::map<std::string, SchemaField> to_fields = field_map_for(to);
  for (const auto &entry : from_fields) {
    const auto found = to_fields.find(entry.first);
    if (found == to_fields.end()) {
      diagnostics->push_back(diagnostic(
          "SchemaViolationError", "compatible migration removes existing field",
          migration.schema_name, entry.first));
      continue;
    }
    if (canonical_schema_type(found->second.type) !=
        canonical_schema_type(entry.second.type)) {
      diagnostics->push_back(
          diagnostic("SchemaViolationError",
                     "compatible migration changes existing field type",
                     migration.schema_name, entry.first));
    }
  }
  for (const auto &entry : to_fields) {
    if (from_fields.find(entry.first) != from_fields.end()) {
      continue;
    }
    if (entry.second.required && !entry.second.nullable &&
        entry.second.default_value.empty()) {
      diagnostics->push_back(
          diagnostic("SchemaViolationError",
                     "compatible migration adds required field without default",
                     migration.schema_name, entry.first));
    }
  }
}

void emit_schema_array(std::ostringstream &out,
                       const std::vector<SchemaDefinition> &schemas) {
  out << "  \"schemas\": [";
  for (std::size_t i = 0; i < schemas.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(schemas[i].name)
        << "\",\"version\":" << schemas[i].version << ",\"fields\":[";
    for (std::size_t j = 0; j < schemas[i].fields.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      const SchemaField &field = schemas[i].fields[j];
      out << "{\"name\":\"" << json_escape(field.name) << "\",\"type\":\""
          << json_escape(field.type)
          << "\",\"required\":" << (field.required ? "true" : "false")
          << ",\"nullable\":" << (field.nullable ? "true" : "false")
          << ",\"default\":\"" << json_escape(field.default_value)
          << "\",\"flags\":" << field.flags << "}";
    }
    out << "],\"flags\":" << schemas[i].flags << "}";
  }
  out << "\n  ]";
}

void emit_migration_array(std::ostringstream &out,
                          const std::vector<SchemaMigration> &migrations) {
  out << "  \"migrations\": [";
  for (std::size_t i = 0; i < migrations.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"schema\":\"" << json_escape(migrations[i].schema_name)
        << "\",\"from\":" << migrations[i].from_version
        << ",\"to\":" << migrations[i].to_version << ",\"kind\":\""
        << json_escape(migrations[i].kind)
        << "\",\"flags\":" << migrations[i].flags << "}";
  }
  out << "\n  ]";
}

void emit_plan_array(std::ostringstream &out,
                     const std::vector<TablePlan> &plans,
                     bool include_fingerprint) {
  out << "  \"plans\": [";
  for (std::size_t i = 0; i < plans.size(); ++i) {
    const TablePlan plan = normalize_table_plan(plans[i]);
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"plan_id\":\"" << json_escape(plan.plan_id)
        << "\",\"op\":\"" << json_escape(plan.op) << "\",\"inputs\":[";
    for (std::size_t j = 0; j < plan.input_refs.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      out << "\"" << json_escape(plan.input_refs[j]) << "\"";
    }
    out << "],\"arguments\":[";
    for (std::size_t j = 0; j < plan.arguments.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      out << "\"" << json_escape(plan.arguments[j]) << "\"";
    }
    out << "],\"column_dependencies\":[";
    for (std::size_t j = 0; j < plan.column_dependencies.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      out << "{\"table\":\""
          << json_escape(plan.column_dependencies[j].table_ref)
          << "\",\"column\":\""
          << json_escape(plan.column_dependencies[j].column_ref) << "\"}";
    }
    out << "],\"effect_row\":\""
        << json_escape(effect::effect_row_to_text(plan.effect_row))
        << "\",\"flags\":" << plan.flags;
    if (include_fingerprint) {
      out << ",\"fingerprint\":\"" << table_plan_fingerprint(plan) << "\"";
    }
    out << "}";
  }
  out << "\n  ]";
}

} // namespace

std::vector<std::string> canonical_schema_types() {
  return std::vector<std::string>(canonical_type_set().begin(),
                                  canonical_type_set().end());
}

std::string canonical_schema_type(const std::string &type) {
  static const std::map<std::string, std::string> aliases = {
      {"boolean", "bool"},      {"int", "integer"},  {"i64", "integer"},
      {"number", "float"},      {"double", "float"}, {"str", "string"},
      {"datetime", "timestamp"}};
  const std::string value = trim(type);
  const auto found = aliases.find(value);
  return found == aliases.end() ? value : found->second;
}

bool valid_schema_type(const std::string &type) {
  const std::string canonical = canonical_schema_type(type);
  if (canonical_type_set().find(canonical) != canonical_type_set().end()) {
    return true;
  }
  return valid_schema_name(canonical) &&
         canonical.find('.') != std::string::npos;
}

bool valid_schema_name(const std::string &name) {
  const std::string value = trim(name);
  if (value.empty() || value.front() == '.' || value.back() == '.') {
    return false;
  }
  for (const char c : value) {
    if (!valid_name_char(c)) {
      return false;
    }
  }
  return true;
}

bool valid_table_op(const std::string &op) {
  return table_op_set().find(trim(op)) != table_op_set().end();
}

SchemaField normalize_schema_field(SchemaField field) {
  field.name = trim(field.name);
  field.type = canonical_schema_type(field.type);
  return field;
}

SchemaDefinition normalize_schema(SchemaDefinition schema) {
  schema.name = trim(schema.name);
  if (schema.version == 0U) {
    schema.version = 1U;
  }
  for (SchemaField &field : schema.fields) {
    field = normalize_schema_field(std::move(field));
  }
  std::sort(schema.fields.begin(), schema.fields.end(),
            [](const SchemaField &left, const SchemaField &right) {
              return left.name < right.name;
            });
  return schema;
}

SchemaMigration normalize_schema_migration(SchemaMigration migration) {
  migration.schema_name = trim(migration.schema_name);
  migration.kind = trim(migration.kind);
  if (migration.kind.empty()) {
    migration.kind = "compatible";
  }
  return migration;
}

SchemaRecord normalize_schema_record(SchemaRecord record) {
  record.schema_name = trim(record.schema_name);
  std::sort(record.values.begin(), record.values.end(),
            [](const SchemaValue &left, const SchemaValue &right) {
              return left.field < right.field;
            });
  return record;
}

TablePlan normalize_table_plan(TablePlan plan) {
  plan.plan_id = trim(plan.plan_id);
  plan.op = trim(plan.op);
  for (std::string &input : plan.input_refs) {
    input = trim(input);
  }
  for (std::string &argument : plan.arguments) {
    argument = trim(argument);
  }
  plan.column_dependencies = sorted_dependencies(plan.column_dependencies);
  plan.effect_row = effect::normalize_effects(plan.effect_row);
  return plan;
}

SchemaValidationResult
validate_schemas(const std::vector<SchemaDefinition> &schemas,
                 const std::vector<SchemaMigration> &migrations,
                 const std::vector<SchemaRecord> &records) {
  SchemaValidationResult result;
  std::set<std::string> seen_schemas;

  for (SchemaDefinition schema : schemas) {
    schema = normalize_schema(std::move(schema));
    result.schemas.push_back(schema);
    if (!valid_schema_name(schema.name)) {
      result.diagnostics.push_back(diagnostic(
          "SchemaViolationError", "schema name is invalid", schema.name));
    }
    const std::string key = schema_version_id(schema);
    if (!seen_schemas.insert(key).second) {
      result.diagnostics.push_back(
          diagnostic("SchemaViolationError", "duplicate schema version", key));
    }
    std::set<std::string> seen_fields;
    for (const SchemaField &field : schema.fields) {
      if (!valid_schema_name(field.name)) {
        result.diagnostics.push_back(diagnostic(
            "SchemaViolationError", "field name is invalid", key, field.name));
      }
      if (!valid_schema_type(field.type)) {
        result.diagnostics.push_back(diagnostic(
            "SchemaViolationError", "field type is invalid", key, field.name));
      }
      if (!seen_fields.insert(field.name).second) {
        result.diagnostics.push_back(diagnostic(
            "SchemaViolationError", "duplicate field", key, field.name));
      }
    }
  }

  std::sort(result.schemas.begin(), result.schemas.end(),
            [](const SchemaDefinition &left, const SchemaDefinition &right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.version < right.version;
            });

  for (SchemaMigration migration : migrations) {
    migration = normalize_schema_migration(std::move(migration));
    result.migrations.push_back(migration);
    if (!valid_schema_name(migration.schema_name)) {
      result.diagnostics.push_back(
          diagnostic("SchemaViolationError", "migration schema name is invalid",
                     migration.schema_name));
      continue;
    }
    if (migration.from_version == 0U || migration.to_version == 0U ||
        migration.from_version >= migration.to_version) {
      result.diagnostics.push_back(diagnostic(
          "SchemaViolationError", "migration version range is invalid",
          migration.schema_name));
      continue;
    }
    if (migration.kind != "compatible" && migration.kind != "additive" &&
        migration.kind != "breaking") {
      result.diagnostics.push_back(diagnostic("SchemaViolationError",
                                              "migration kind is invalid",
                                              migration.schema_name));
      continue;
    }
    const SchemaDefinition *from = find_schema(
        result.schemas, migration.schema_name, migration.from_version);
    const SchemaDefinition *to = find_schema(
        result.schemas, migration.schema_name, migration.to_version);
    if (from == nullptr || to == nullptr) {
      result.diagnostics.push_back(diagnostic(
          "SchemaViolationError", "migration references unknown schema version",
          migration.schema_name));
      continue;
    }
    validate_compatible_migration(migration, *from, *to, &result.diagnostics);
  }

  std::sort(result.migrations.begin(), result.migrations.end(),
            [](const SchemaMigration &left, const SchemaMigration &right) {
              if (left.schema_name != right.schema_name) {
                return left.schema_name < right.schema_name;
              }
              if (left.from_version != right.from_version) {
                return left.from_version < right.from_version;
              }
              return left.to_version < right.to_version;
            });

  for (SchemaRecord record : records) {
    record = normalize_schema_record(std::move(record));
    validate_record(record, result.schemas, &result.diagnostics);
  }

  result.ok = result.diagnostics.empty();
  return result;
}

TablePlanValidationResult
validate_table_plans(const std::vector<TablePlan> &plans) {
  TablePlanValidationResult result;
  std::set<std::string> seen;
  for (TablePlan plan : plans) {
    plan = normalize_table_plan(std::move(plan));
    result.plans.push_back(plan);
    if (!valid_schema_name(plan.plan_id)) {
      result.diagnostics.push_back(diagnostic(
          "TablePlanError", "table plan id is invalid", plan.plan_id));
    }
    if (!valid_table_op(plan.op)) {
      result.diagnostics.push_back(diagnostic("TablePlanError",
                                              "table plan operation is invalid",
                                              plan.plan_id, plan.op));
    }
    if (plan.op != "scan" && plan.input_refs.empty()) {
      result.diagnostics.push_back(
          diagnostic("TablePlanError", "non-scan table plan requires an input",
                     plan.plan_id));
    }
    if (!seen.insert(plan.plan_id).second) {
      result.diagnostics.push_back(diagnostic(
          "TablePlanError", "duplicate table plan id", plan.plan_id));
    }
    for (const ColumnDependency &dependency : plan.column_dependencies) {
      if (!valid_schema_name(dependency.table_ref) ||
          !valid_schema_name(dependency.column_ref)) {
        result.diagnostics.push_back(diagnostic(
            "TablePlanError", "column dependency is invalid", plan.plan_id,
            dependency.table_ref + "." + dependency.column_ref));
      }
    }
    for (const std::string &label : plan.effect_row) {
      if (!effect::valid_effect_name(label)) {
        result.diagnostics.push_back(
            diagnostic("TablePlanError", "table plan effect label is invalid",
                       plan.plan_id, label));
      }
    }
  }
  std::sort(result.plans.begin(), result.plans.end(),
            [](const TablePlan &left, const TablePlan &right) {
              return left.plan_id < right.plan_id;
            });
  result.ok = result.diagnostics.empty();
  return result;
}

std::string schema_version_id(const SchemaDefinition &schema) {
  return schema.name + "@" + std::to_string(schema.version);
}

std::string table_plan_fingerprint(const TablePlan &plan) {
  const TablePlan normalized = normalize_table_plan(plan);
  std::ostringstream body;
  body << "amber.table.plan.v1\n";
  body << "id=" << normalized.plan_id << "\n";
  body << "op=" << normalized.op << "\n";
  body << "flags=" << normalized.flags << "\n";
  for (const std::string &input : normalized.input_refs) {
    body << "input=" << input << "\n";
  }
  for (const std::string &argument : normalized.arguments) {
    body << "arg=" << argument << "\n";
  }
  for (const ColumnDependency &dependency : normalized.column_dependencies) {
    body << "dep=" << dependency.table_ref << "." << dependency.column_ref
         << "\n";
  }
  body << "effects=" << effect::effect_row_to_text(normalized.effect_row)
       << "\n";
  return lexer::sha256_hex(body.str());
}

SchemaDocumentParseResult parse_schema_document(const std::string &source) {
  SchemaDocumentParseResult result;
  const std::map<std::string, std::string> values = parse_lines(source);
  if (value_or_empty(values, "schema") != "amber.schema.v1") {
    result.diagnostics.push_back(
        diagnostic("DataProfileParseError",
                   "schema document must start with amber.schema.v1"));
  }

  std::uint32_t schema_count = 0;
  read_count(values, "schema.count", &schema_count, &result.diagnostics);
  for (std::uint32_t i = 0; i < schema_count; ++i) {
    const std::string prefix = "schema." + std::to_string(i) + ".";
    SchemaDefinition schema;
    schema.name = value_or_empty(values, prefix + "name");
    parse_u32(value_or_empty(values, prefix + "version"), &schema.version);
    parse_u32(value_or_empty(values, prefix + "flags"), &schema.flags);

    std::uint32_t field_count = 0;
    read_count(values, prefix + "field.count", &field_count,
               &result.diagnostics);
    for (std::uint32_t j = 0; j < field_count; ++j) {
      const std::string field_prefix =
          prefix + "field." + std::to_string(j) + ".";
      SchemaField field;
      field.name = value_or_empty(values, field_prefix + "name");
      field.type = value_or_empty(values, field_prefix + "type");
      parse_bool(value_or_empty(values, field_prefix + "required"),
                 &field.required);
      parse_bool(value_or_empty(values, field_prefix + "nullable"),
                 &field.nullable);
      field.default_value = value_or_empty(values, field_prefix + "default");
      parse_u32(value_or_empty(values, field_prefix + "flags"), &field.flags);
      schema.fields.push_back(std::move(field));
    }
    result.document.schemas.push_back(normalize_schema(std::move(schema)));
  }

  std::uint32_t migration_count = 0;
  read_count(values, "migration.count", &migration_count, &result.diagnostics,
             false);
  for (std::uint32_t i = 0; i < migration_count; ++i) {
    const std::string prefix = "migration." + std::to_string(i) + ".";
    SchemaMigration migration;
    migration.schema_name = value_or_empty(values, prefix + "schema");
    parse_u32(value_or_empty(values, prefix + "from"), &migration.from_version);
    parse_u32(value_or_empty(values, prefix + "to"), &migration.to_version);
    migration.kind = value_or_empty(values, prefix + "kind");
    parse_u32(value_or_empty(values, prefix + "flags"), &migration.flags);
    result.document.migrations.push_back(
        normalize_schema_migration(std::move(migration)));
  }

  std::uint32_t record_count = 0;
  read_count(values, "record.count", &record_count, &result.diagnostics, false);
  for (std::uint32_t i = 0; i < record_count; ++i) {
    const std::string prefix = "record." + std::to_string(i) + ".";
    SchemaRecord record;
    record.schema_name = value_or_empty(values, prefix + "schema");
    parse_u32(value_or_empty(values, prefix + "version"), &record.version);
    std::uint32_t value_count = 0;
    read_count(values, prefix + "value.count", &value_count,
               &result.diagnostics);
    for (std::uint32_t j = 0; j < value_count; ++j) {
      const std::string value_prefix =
          prefix + "value." + std::to_string(j) + ".";
      SchemaValue value;
      value.field = value_or_empty(values, value_prefix + "field");
      value.value = value_or_empty(values, value_prefix + "value");
      record.values.push_back(std::move(value));
    }
    result.document.records.push_back(
        normalize_schema_record(std::move(record)));
  }

  return result;
}

TablePlanParseResult parse_table_plan_document(const std::string &source) {
  TablePlanParseResult result;
  const std::map<std::string, std::string> values = parse_lines(source);
  if (value_or_empty(values, "schema") != "amber.table.v1") {
    result.diagnostics.push_back(
        diagnostic("DataProfileParseError",
                   "table document must start with amber.table.v1"));
  }
  std::uint32_t plan_count = 0;
  read_count(values, "plan.count", &plan_count, &result.diagnostics);
  for (std::uint32_t i = 0; i < plan_count; ++i) {
    const std::string prefix = "plan." + std::to_string(i) + ".";
    TablePlan plan;
    plan.plan_id = value_or_empty(values, prefix + "id");
    if (plan.plan_id.empty()) {
      plan.plan_id = value_or_empty(values, prefix + "plan_id");
    }
    plan.op = value_or_empty(values, prefix + "op");
    parse_u32(value_or_empty(values, prefix + "flags"), &plan.flags);

    std::uint32_t input_count = 0;
    read_count(values, prefix + "input.count", &input_count,
               &result.diagnostics, false);
    for (std::uint32_t j = 0; j < input_count; ++j) {
      plan.input_refs.push_back(
          value_or_empty(values, prefix + "input." + std::to_string(j)));
    }

    std::uint32_t arg_count = 0;
    read_count(values, prefix + "arg.count", &arg_count, &result.diagnostics,
               false);
    for (std::uint32_t j = 0; j < arg_count; ++j) {
      plan.arguments.push_back(
          value_or_empty(values, prefix + "arg." + std::to_string(j)));
    }

    std::uint32_t dep_count = 0;
    read_count(values, prefix + "dep.count", &dep_count, &result.diagnostics,
               false);
    for (std::uint32_t j = 0; j < dep_count; ++j) {
      const std::string dep_prefix = prefix + "dep." + std::to_string(j) + ".";
      plan.column_dependencies.push_back(
          {value_or_empty(values, dep_prefix + "table"),
           value_or_empty(values, dep_prefix + "column")});
    }

    std::vector<effect::EffectDiagnostic> effect_diagnostics;
    effect::parse_effect_row(value_or_empty(values, prefix + "effect_row"),
                             &plan.effect_row, &effect_diagnostics,
                             plan.plan_id);
    for (const effect::EffectDiagnostic &entry : effect_diagnostics) {
      result.diagnostics.push_back(diagnostic(entry.error_name, entry.message,
                                              entry.owner, entry.effect));
    }

    result.plans.push_back(normalize_table_plan(std::move(plan)));
  }
  return result;
}

std::string schema_validation_to_json(const SchemaValidationResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.schema.validation.v1\",\n";
  out << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n";
  emit_schema_array(out, result.schemas);
  out << ",\n";
  emit_migration_array(out, result.migrations);
  out << ",\n";
  emit_diagnostics(out, result.diagnostics);
  out << "}\n";
  return out.str();
}

std::string
table_plan_validation_to_json(const TablePlanValidationResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.table.validation.v1\",\n";
  out << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n";
  emit_plan_array(out, result.plans, true);
  out << ",\n";
  emit_diagnostics(out, result.diagnostics);
  out << "}\n";
  return out.str();
}

std::string table_plans_to_json(const std::vector<TablePlan> &plans) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.table.explain.v1\",\n";
  emit_plan_array(out, plans, true);
  out << "\n}\n";
  return out.str();
}

} // namespace amber::data
