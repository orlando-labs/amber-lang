#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber::data {

inline constexpr std::uint32_t kSchemaFieldFlagPrimaryKey = 0x1U;
inline constexpr std::uint32_t kSchemaMigrationFlagCompatible = 0x1U;
inline constexpr std::uint32_t kTablePlanFlagLazy = 0x1U;

struct SchemaField {
  std::string name;
  std::string type;
  bool required = true;
  bool nullable = false;
  std::string default_value;
  std::uint32_t flags = 0;
};

struct SchemaDefinition {
  std::string name;
  std::uint32_t version = 1;
  std::vector<SchemaField> fields;
  std::uint32_t flags = 0;
};

struct SchemaMigration {
  std::string schema_name;
  std::uint32_t from_version = 0;
  std::uint32_t to_version = 0;
  std::string kind;
  std::uint32_t flags = 0;
};

struct SchemaValue {
  std::string field;
  std::string value;
};

struct SchemaRecord {
  std::string schema_name;
  std::uint32_t version = 0;
  std::vector<SchemaValue> values;
};

struct ColumnDependency {
  std::string table_ref;
  std::string column_ref;
};

struct TablePlan {
  std::string plan_id;
  std::string op;
  std::vector<std::string> input_refs;
  std::vector<std::string> arguments;
  std::vector<ColumnDependency> column_dependencies;
  std::vector<std::string> effect_row;
  std::uint32_t flags = 0;
};

struct DataDiagnostic {
  std::string error_name;
  std::string message;
  std::string subject;
  std::string field;
};

struct SchemaDocument {
  std::vector<SchemaDefinition> schemas;
  std::vector<SchemaMigration> migrations;
  std::vector<SchemaRecord> records;
};

struct SchemaValidationResult {
  bool ok = false;
  std::vector<SchemaDefinition> schemas;
  std::vector<SchemaMigration> migrations;
  std::vector<DataDiagnostic> diagnostics;
};

struct SchemaDocumentParseResult {
  SchemaDocument document;
  std::vector<DataDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

struct TablePlanValidationResult {
  bool ok = false;
  std::vector<TablePlan> plans;
  std::vector<DataDiagnostic> diagnostics;
};

struct TablePlanParseResult {
  std::vector<TablePlan> plans;
  std::vector<DataDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

std::vector<std::string> canonical_schema_types();
std::string canonical_schema_type(const std::string &type);
bool valid_schema_type(const std::string &type);
bool valid_schema_name(const std::string &name);
bool valid_table_op(const std::string &op);

SchemaField normalize_schema_field(SchemaField field);
SchemaDefinition normalize_schema(SchemaDefinition schema);
SchemaMigration normalize_schema_migration(SchemaMigration migration);
SchemaRecord normalize_schema_record(SchemaRecord record);
TablePlan normalize_table_plan(TablePlan plan);

SchemaValidationResult
validate_schemas(const std::vector<SchemaDefinition> &schemas,
                 const std::vector<SchemaMigration> &migrations = {},
                 const std::vector<SchemaRecord> &records = {});
TablePlanValidationResult
validate_table_plans(const std::vector<TablePlan> &plans);

std::string schema_version_id(const SchemaDefinition &schema);
std::string table_plan_fingerprint(const TablePlan &plan);

SchemaDocumentParseResult parse_schema_document(const std::string &source);
TablePlanParseResult parse_table_plan_document(const std::string &source);

std::string schema_validation_to_json(const SchemaValidationResult &result);
std::string
table_plan_validation_to_json(const TablePlanValidationResult &result);
std::string table_plans_to_json(const std::vector<TablePlan> &plans);

} // namespace amber::data
