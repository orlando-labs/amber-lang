#pragma once

#include "frontend/lexer/token.h"
#include "profile/capabilities.h"
#include "profile/data.h"
#include "profile/effects.h"
#include "profile/modern.h"
#include "profile/replay.h"
#include "profile/wasm_accel.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace amber::bytecode {

struct Version {
  std::uint16_t major = 1;
  std::uint16_t minor = 0;
};

enum class SectionKind {
  Strs,
  Syms,
  Kons,
  Code,
  Meth,
  Clas,
  Deps,
  Expt,
  Init,
  Pats,
  Span,
  Line,
  Locs,
  Attr,
  Prof,
  Caps,
  Efct,
  Obsv,
  Rply,
  Scma,
  Tabl,
  Wasm,
  Accl,
  Agnt,
  Cntr,
  Priv,
  Wflw,
  Hash
};

enum class ConstantKind {
  Null,
  Bool,
  Integer,
  Float,
  SymbolRef,
  StringRef,
  CodeRef,
  KeySet,
  Path
};

enum class Opcode : std::uint8_t {
  LoadK = 0x01,
  LoadNull = 0x02,
  LoadBool = 0x03,
  Move = 0x04,
  LoadSelf = 0x05,
  GetLast = 0x06,
  SetLast = 0x07,
  MakeList = 0x08,
  MakeTuple = 0x09,
  MakeMap = 0x0A,
  Freeze = 0x0B,
  MakeSet = 0x0C,
  LoadUpval = 0x10,
  StoreUpval = 0x11,
  LoadIvar = 0x12,
  StoreIvar = 0x13,
  LoadCvar = 0x14,
  StoreCvar = 0x15,
  LookupConst = 0x16,
  MakeClosure = 0x17,
  ObjDestroy = 0x18,
  ObjDealloc = 0x19,
  CloseUpvalues = 0x1A,
  WatchLocal = 0x1B,
  WatchUpval = 0x1C,
  WatchIvar = 0x1D,
  Send = 0x20,
  SendDyn = 0x21,
  Call = 0x22,
  InOp = 0x23,
  TripleEq = 0x24,
  TypeCheck = 0x25,
  IAdd = 0x26,
  ISub = 0x27,
  ILt = 0x28,
  IGt = 0x29,
  IAddK = 0x2A,
  ISubK = 0x2B,
  ILtK = 0x2C,
  IGtK = 0x2D,
  Jump = 0x30,
  JumpIfTrue = 0x31,
  JumpIfFalse = 0x32,
  JumpIfNull = 0x33,
  Return = 0x34,
  Raise = 0x35,
  Safepoint = 0x36,
  PPrepSeq = 0x40,
  PPrepMap = 0x41,
  PCheckEq = 0x42,
  PCheckPin = 0x43,
  PCheckLenEq = 0x44,
  PCheckLenGte = 0x45,
  PGetIndex = 0x46,
  PHasKey = 0x47,
  PGetKey = 0x48,
  PTripleEq = 0x49,
  PBind = 0x4A,
  PCommit = 0x4B,
  PFail = 0x4C
};

enum class CodeKind { Module, Method, Block, Ensure, Rescue, DefaultThunk };

struct SectionEntry {
  SectionKind kind;
  std::uint64_t offset = 0;
  std::uint32_t size = 0;
  std::uint32_t align = 1;
  std::uint32_t flags = 0;
};

struct Constant {
  ConstantKind kind = ConstantKind::Null;
  bool bool_value = false;
  std::int64_t int_value = 0;
  double float_value = 0.0;
  std::uint32_t ref_id = 0;
  std::vector<std::uint32_t> items;
};

struct SlotLayoutEntry {
  std::uint32_t slot = 0;
  std::uint32_t name_str_id = 0;
  std::uint32_t role_str_id = 0;
  std::uint32_t binding_kind_str_id = 0;
};

struct CaptureLayoutEntry {
  std::uint32_t slot = 0;
  std::uint32_t name_str_id = 0;
  std::uint32_t source_kind_str_id = 0;
  std::uint32_t source_name_str_id = 0;
};

struct InstructionOperand {
  std::int64_t value = 0;
  bool signed_immediate = false;
};

struct Instruction {
  Opcode opcode = Opcode::Return;
  std::vector<InstructionOperand> operands;
};

struct HandlerEntry {
  std::uint32_t protected_from = 0;
  std::uint32_t protected_to = 0;
  std::uint32_t handler_pc = 0;
  std::uint32_t handler_code_id = 0;
  std::uint32_t flags = 0;
};

struct CacheSiteEntry {
  std::uint32_t pc = 0;
  std::uint32_t slot = 0;
  std::uint32_t symbol_id = 0;
  std::uint32_t flags = 0;
};

inline constexpr std::uint32_t kCallSiteFlagPropertyAccess = 0x1U;
inline constexpr std::uint32_t kCallSiteFlagPropertyAssignment = 0x2U;

struct SourceSpanEntry {
  std::uint32_t pc_from = 0;
  std::uint32_t pc_to = 0;
  lexer::Span span;
};

struct SafepointEntry {
  std::uint32_t pc = 0;
  std::uint32_t flags = 0;
};

struct ClauseEntry {
  std::uint32_t pattern_program_id = 0;
  std::uint32_t pattern_code_id = 0;
  std::uint32_t guard_code_id = 0;
  std::uint32_t body_code_id = 0;
  std::uint32_t flags = 0;
};

struct AutoAssignEntry {
  std::uint32_t local_name_str_id = 0;
  std::uint32_t target_name_str_id = 0;
  std::uint32_t flags = 0;
};

inline constexpr std::uint32_t kMethodParamFlagKeyword = 0x1U;
inline constexpr std::uint32_t kMethodParamFlagHasDefault = 0x2U;

struct MethodParamEntry {
  std::uint32_t external_name_sym_id = 0;
  std::uint32_t local_name_str_id = 0;
  std::uint32_t flags = 0;
};

struct BcCode {
  std::uint32_t code_id = 0;
  CodeKind kind = CodeKind::Method;
  std::uint32_t reg_count = 0;
  std::vector<SlotLayoutEntry> local_layout;
  std::vector<CaptureLayoutEntry> capture_layout;
  std::vector<Instruction> instructions;
  std::vector<HandlerEntry> handler_table;
  std::vector<CacheSiteEntry> call_site_table;
  std::vector<CacheSiteEntry> ivar_site_table;
  std::vector<SourceSpanEntry> source_spans;
  std::vector<SafepointEntry> safepoint_table;
  std::uint32_t flags = 0;
};

struct BcMethod {
  std::uint32_t selector_sym_id = 0;
  std::uint32_t owner_dispatch_ref = 0;
  std::uint32_t signature_blob_id = 0;
  std::vector<MethodParamEntry> params;
  std::vector<std::uint32_t> default_thunk_ids;
  std::vector<std::uint32_t> type_hook_ids;
  std::vector<ClauseEntry> clause_table;
  std::vector<AutoAssignEntry> auto_assign_desc;
  std::uint32_t entry_code_id = 0;
  std::uint32_t flags = 0;
};

inline constexpr std::uint32_t kMethodFlagInstance = 0x1U;
inline constexpr std::uint32_t kMethodFlagClass = 0x2U;
inline constexpr std::uint32_t kMethodFlagPropertyGetter = 0x4U;
inline constexpr std::uint32_t kMethodFlagPropertySetter = 0x8U;

inline constexpr std::uint32_t kClassFlagMixin = 0x1U;

struct BcClass {
  std::uint32_t class_name_sym_id = 0;
  bool has_superclass_ref = false;
  std::uint32_t superclass_ref = 0;
  std::uint32_t ivar_schema_id = 0;
  std::uint32_t method_range_start = 0;
  std::uint32_t method_range_count = 0;
  std::vector<std::uint32_t> direct_include_refs;
  std::vector<std::uint32_t> direct_extend_refs;
  std::uint32_t flags = 0;
  bool has_class_init_code_id = false;
  std::uint32_t class_init_code_id = 0;
};

struct DepEntry {
  std::uint32_t module_name_str_id = 0;
  Version required_format;
  Version min_language_version;
  bool has_max_language_version = false;
  Version max_language_version;
  bool has_abi_requirement = false;
  std::array<std::uint8_t, 32> abi_requirement{};
  std::uint32_t flags = 0;
};

struct ExportEntry {
  std::uint32_t symbol_id = 0;
  std::uint32_t target_kind_str_id = 0;
  std::uint32_t target_index = 0;
  std::uint32_t visibility_flags = 0;
  bool has_reexport_module_name = false;
  std::uint32_t reexport_module_name_str_id = 0;
};

struct InitEntry {
  bool has_entry_code_id = false;
  std::uint32_t entry_code_id = 0;
  std::uint32_t flags = 0;
};

struct PatternProgramEntry {
  std::uint32_t pattern_id = 0;
  std::uint32_t binding_count = 0;
  std::uint32_t flags = 0;
};

struct LineEntry {
  std::uint32_t code_id = 0;
  std::uint32_t pc = 0;
  std::uint32_t line = 0;
};

struct LocalDebugEntry {
  std::uint32_t code_id = 0;
  std::uint32_t slot = 0;
  std::uint32_t name_str_id = 0;
  std::uint32_t start_pc = 0;
  std::uint32_t end_pc = 0;
};

struct AttrEntry {
  std::uint32_t key_str_id = 0;
  std::uint32_t value_str_id = 0;
};

struct HashEntry {
  SectionKind section = SectionKind::Hash;
  std::vector<std::uint8_t> digest;
};

struct BcModule {
  Version format_version;
  Version language_version;
  std::uint32_t profile_flags = 0;
  std::uint32_t file_flags = 0;
  std::array<std::uint8_t, 32> abi_hash{};
  std::vector<std::string> strings;
  std::vector<std::string> symbols;
  std::vector<Constant> const_pool;
  std::vector<BcCode> code_objects;
  std::vector<BcMethod> methods;
  std::vector<BcClass> classes;
  std::vector<DepEntry> dependencies;
  std::vector<ExportEntry> exports;
  InitEntry init;
  std::vector<PatternProgramEntry> pattern_programs;
  std::vector<LineEntry> line_table;
  std::vector<LocalDebugEntry> local_debug;
  std::vector<AttrEntry> attrs;
  std::vector<std::string> required_features;
  std::vector<std::string> optional_features;
  std::vector<std::string> forbidden_features;
  std::vector<capability::CapabilityRequest> capabilities;
  std::vector<effect::EffectSummary> effects;
  std::vector<replay::ObservabilitySite> observability_sites;
  replay::ReplayMetadata replay_metadata;
  std::vector<data::SchemaDefinition> schemas;
  std::vector<data::SchemaMigration> schema_migrations;
  std::vector<data::TablePlan> table_plans;
  std::vector<wasm_accel::WasmComponent> wasm_components;
  std::vector<wasm_accel::AcceleratorKernel> accelerator_kernels;
  std::vector<modern::AgentSymbol> agent_symbols;
  std::vector<modern::AgentPatch> agent_patches;
  std::vector<modern::ProvenanceRecord> provenance_records;
  std::vector<modern::ContractSpec> contracts;
  std::vector<modern::PropertySpec> properties;
  std::vector<modern::PrivacyLabel> privacy_labels;
  std::vector<modern::PrivacyPolicyRule> privacy_policies;
  std::vector<modern::LineageNode> lineage_nodes;
  std::vector<modern::WorkflowStep> workflow_steps;
  std::vector<modern::WorkflowHistoryEvent> workflow_history;
  std::vector<HashEntry> hashes;
};

struct VerifyError {
  std::string code;
  std::string message;
  std::string section;
  std::uint64_t offset = 0;
};

struct DecodeResult {
  BcModule module;
  std::vector<SectionEntry> sections;
  std::vector<VerifyError> errors;

  bool ok() const { return errors.empty(); }
};

std::vector<std::uint8_t> serialize_module(const BcModule &module);
DecodeResult deserialize_module(const std::vector<std::uint8_t> &bytes);

std::string section_kind_name(SectionKind kind);
std::string constant_kind_name(ConstantKind kind);
std::string opcode_name(Opcode opcode);
std::string code_kind_name(CodeKind kind);

std::string module_to_json(const BcModule &module,
                           const std::vector<SectionEntry> &sections,
                           const std::string &source_hash);
std::string module_to_disasm(const BcModule &module,
                             const std::vector<SectionEntry> &sections,
                             const std::string &source_hash);
std::string verify_errors_to_json(const std::vector<VerifyError> &errors);

} // namespace amber::bytecode
