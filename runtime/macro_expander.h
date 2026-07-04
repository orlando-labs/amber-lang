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

// One `export macro` entry of an already-staged provider module: the public
// name plus an owned clone of the `macro def` AST (still is_macro-tagged).
struct MacroExport {
  std::string public_name;
  std::shared_ptr<const ast::Expr> def;
};

// Provider table for cross-module macro staging (DESIGN-macro-system §11):
// provider module path -> its `export macro` table. Built by the driver in
// dependency (v1: manifest) order, so importers only ever see providers whose
// macro definitions already staged.
using MacroProviderMap = std::map<std::string, std::vector<MacroExport>>;

// Harvest a parsed module's `export macro` table: every AstExportStmt item
// marked `macro` whose local name matches a `macro def` in `items` yields a
// MacroExport under its public name. A macro-marked export without a matching
// definition is skipped (the importer will report the unknown name).
std::vector<MacroExport>
collect_macro_exports(const std::vector<std::unique_ptr<ast::Expr>> &items);

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
// The provider-aware overload additionally resolves `from <module> import
// name [as alias]` items against `providers` (§11 exports/imports): each
// matching macro export is cloned under the importer's local alias and joins
// the module's compile-time macro namespace. Imported names that are not in
// the provider table stay ordinary runtime imports. v1 limits: only the
// `from … import` form binds macros (module-alias dotted calls are later),
// and an imported macro body may only call other macros/builtins — provider
// runtime helpers are not staged with it.
ExpandResult expand_macros(std::vector<std::unique_ptr<ast::Expr>> &items,
                           const std::string &module_name,
                           const std::string &source);
ExpandResult expand_macros(std::vector<std::unique_ptr<ast::Expr>> &items,
                           const std::string &module_name,
                           const std::string &source,
                           const MacroProviderMap &providers);

} // namespace amber::macros
