#pragma once

#include "frontend/ast/expr.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace amber::macros {

struct ExpandResult {
  bool ok = true;
  std::string error; // diagnostic message when ok == false
};

// One staged macro definition of an already-staged provider module: public
// exported macros are triggerable by importers; private macro defs ride along
// only as compile-time helpers for exported macro bodies.
struct MacroExport {
  std::string public_name;
  std::shared_ptr<const ast::Expr> def;
  bool public_export = true;
};

// Provider table for cross-module macro staging (DESIGN-macro-system §11):
// provider module path -> its `export macro` table. Built by the driver in
// dependency (v1: manifest) order, so importers only ever see providers whose
// macro definitions already staged.
using MacroProviderMap = std::map<std::string, std::vector<MacroExport>>;

// Harvest a parsed module's staged macro table. If the module has at least one
// valid `export macro`, all of its macro defs are returned: exported entries are
// public trigger surfaces, and private entries are helper definitions available
// to those public macros. A macro-marked export without a matching definition is
// skipped (the importer will report the unknown name).
std::vector<MacroExport>
collect_macro_exports(const std::vector<std::unique_ptr<ast::Expr>> &items);

// Persisted artifact macro section (§11): a macro-providing module's build
// artifact embeds its `export macro` table under this module attribute, so a
// later build can stage against the artifact without re-parsing provider
// source. The payload is schema-tagged (`amber.macro.exports.v2`) and
// carries, per staged macro: public/helper visibility, public name, surface kind
// (`call` / `string_tag`), the definition's source line, and the `macro def`
// source slice itself — the parser is the deserializer, so staging from an
// artifact is exactly staging from source. v1 payloads remain readable as
// public-only tables.
inline constexpr const char *kMacroExportsAttrKey = "amber.macro.exports";

// Encode `exports` (harvested from a module whose text is `module_source`)
// as an artifact macro-section payload.
std::string serialize_macro_exports(const std::vector<MacroExport> &exports,
                                    const std::string &module_source);

// Decode an artifact macro-section payload back into an export table.
// `provider_path` names the provider source file for diagnostic spans; the
// re-parsed definitions carry their original source lines. Returns an empty
// table with `*error` set when the payload is malformed.
std::vector<MacroExport> parse_macro_exports(const std::string &payload,
                                             const std::string &provider_path,
                                             std::string *error);

// Macro expansion pass (F1.5, DESIGN-macro-system §3). Runs after parsing and
// before binding. Collects `macro def` declarations, compiles them as ordinary
// functions on an expander VM, then rewrites each function-like macro call in
// the remaining items by executing the macro body with the call's argument ASTs
// (as `Ast` values) at compile time and splicing the returned AST at the call
// site. Macro definitions are removed from `items` on success. A module with no
// macro definitions is returned unchanged (zero cost).
//
// `source` is the original module text, so `Ast` argument values report the
// verbatim source slice via `.source`. Expansion is bounded by a fixpoint depth
// limit; overruns and macro faults are reported as `ok == false` diagnostics
// rather than hangs or crashes.
//
// The provider-aware overload additionally resolves imports against
// `providers` (§11 exports/imports): each `from <module> import name [as
// alias]` match is cloned under the importer's local alias, and each
// `import <module> as alias` binds every public macro export of the provider
// under the dotted spelling `alias.name` (usable through the call, string-tag,
// block-suffix, annotation, and `use` surfaces). Imported names that are not
// in the provider table stay ordinary runtime imports. Private provider macro
// defs are staged as compile-time helpers, but ordinary runtime helper
// functions are not.
ExpandResult expand_macros(std::vector<std::unique_ptr<ast::Expr>> &items,
                           const std::string &module_name,
                           const std::string &source);
ExpandResult expand_macros(std::vector<std::unique_ptr<ast::Expr>> &items,
                           const std::string &module_name,
                           const std::string &source,
                           const MacroProviderMap &providers);

} // namespace amber::macros
